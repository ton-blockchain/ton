from abc import ABC, ABCMeta, abstractmethod
from collections.abc import Callable
from typing import Literal, Protocol, Self, TypeIs, cast, final, override

from bitarray import bitarray
from pytoniq_core import Builder, Cell, Slice


class TlbModelError(Exception):
    def __init__(self, message: str):
        super().__init__(message)

    @staticmethod
    def raise_if_not_empty(cs: Slice):
        if cs.remaining_bits or cs.remaining_refs:
            raise TlbModelError("Deserialization must consume all bits and references")


class TypeInfo[T, *Args](Protocol):
    def serialize_value(self, value: T, builder: Builder) -> None: ...

    def load_from(self, cs: Slice, *args: *Args) -> T: ...

    def deserialize(self, cell: Cell, *args: *Args) -> T:
        cs = cell.begin_parse()
        if cs.is_special():
            raise TlbModelError("Cell must not be special")
        result = self.load_from(cs, *args)
        TlbModelError.raise_if_not_empty(cs)
        return result

    def is_nonnull(self, value: T | None) -> TypeIs[T]:
        return value is not None

    def _is_type_info(self) -> Literal[True]:
        return True


class SelfTypeInfo(Protocol):
    @classmethod
    def serialize_value(cls, value: Self, builder: Builder) -> None: ...

    @classmethod
    def load_from(cls, cs: Slice) -> Self: ...

    @classmethod
    def deserialize(cls, cell: Cell) -> Self: ...

    @classmethod
    def _is_type_info(cls) -> Literal[True]: ...

    @classmethod
    def is_nonnull(cls, value: Self | None) -> TypeIs[Self]: ...


class SelfTypeInfoTag:
    @classmethod
    def _is_type_info(cls) -> Literal[True]:
        return True

    @classmethod
    def is_nonnull(cls, value: Self | None) -> TypeIs[Self]:
        return value is not None


@final
class InstantiatedGenericType[T, *Args](TypeInfo[T]):
    def __init__(self, generic: TypeInfo[T, *Args], *args: *Args):
        self._generic = generic
        self._args = args

    @override
    def serialize_value(self, value: T, builder: Builder):
        self._generic.serialize_value(value, builder)

    @override
    def load_from(self, cs: Slice) -> T:
        return self._generic.load_from(cs, *self._args)

    @override
    def deserialize(self, cell: Cell) -> T:
        return self._generic.deserialize(cell, *self._args)

    @override
    def __eq__(self, other: object) -> bool:
        return (
            isinstance(other, InstantiatedGenericType)
            and self._generic == other._generic  # pyright: ignore[reportUnknownMemberType]
            and self._args == other._args  # pyright: ignore[reportUnknownMemberType]
        )

    @override
    def __repr__(self):
        args_str = ", ".join(repr(arg) for arg in self._args)
        return f"{repr(self._generic)}({args_str})"


class InstantiableTypeInfo[T, *Args](TypeInfo[T, *Args], Protocol):
    @classmethod
    def instantiate(cls, *args: *Args) -> TypeInfo[T]:
        return InstantiatedGenericType(cls(), *args)


class TLBRecord[*Args](ABC):
    @abstractmethod
    def serialize_to(self, builder: Builder) -> None: ...

    def serialize(self) -> Cell:
        builder = Builder()
        self.serialize_to(builder)
        return builder.end_cell()

    def get_output(self, idx: int) -> int:
        raise TlbModelError(f"type has no output param at index {idx}")

    def check_type[T](self, _idx: int, _ti: TypeInfo[T]) -> None:
        pass

    @classmethod
    @abstractmethod
    def load_from(cls, cs: Slice, *args: *Args) -> Self: ...

    @classmethod
    def deserialize(cls, /, cell: Cell, *args: *Args) -> Self:
        cs = cell.begin_parse()
        if cs.is_special():
            raise TlbModelError("Cell must not be special")
        result = cls.load_from(cs, *args)
        TlbModelError.raise_if_not_empty(cs)
        return result

    @classmethod
    def serialize_value(cls, value: Self, builder: Builder) -> None:
        value.serialize_to(builder)


class TLBType(ABCMeta):
    @override
    def __repr__(cls):
        return cls.__name__


class GenericTLBType(TLBType):
    def instantiate[Self, *Args](cls: TypeInfo[Self, *Args], *args: *Args) -> TypeInfo[Self]:
        return InstantiatedGenericType(cls, *args)


@final
class Ref[X](TLBRecord[TypeInfo[X]], SelfTypeInfoTag, metaclass=GenericTLBType):
    def __init__(self, tx: TypeInfo[X], ref: X | Cell):
        self._tx = tx
        if isinstance(ref, Cell):
            self._value_cell = ref
            self._value = None
        else:
            self._value_cell = None
            self._value = ref

    @property
    def ref(self):
        if self._value_cell is not None:
            self._value = self._tx.deserialize(self._value_cell)
            self._value_cell = None
        return cast(X, self._value)

    @ref.setter
    def ref(self, value: X):
        self._value = value
        self._value_cell = None

    def set_cell(self, cell: Cell):
        self._value_cell = cell
        self._value = None

    @property
    def cell(self):
        if self._value_cell is not None:
            return self._value_cell
        else:
            builder = Builder()
            self._tx.serialize_value(cast(X, self._value), builder)
            return builder.end_cell()

    @override
    def serialize_to(self, builder: Builder):
        _ = builder.store_ref(self.cell)

    @override
    @classmethod
    def load_from(cls, cs: Slice, tx: TypeInfo[X]) -> Self:
        child = cs.load_ref()
        return cls(tx, child)

    @override
    def __repr__(self):
        return f"Ref(_tx={self._tx}, value={self._value if self._value is not None else self._value_cell})"


def ref[T: SelfTypeInfo](value: T) -> Ref[T]:
    """
    WARNING: Be very careful not to pass unions as T. They very likely don't satisfy SelfTypeInfo
    but basedpyright will accept them amyway because of
    https://github.com/microsoft/pyright/issues/11374 .
    """
    return Ref(type(value), value)


@final
class _AnyTypeConstructor(TypeInfo[Slice]):
    @override
    def serialize_value(cls, value: Slice, builder: Builder):
        _ = builder.store_slice(value)

    @override
    def load_from(cls, cs: Slice):
        result = cs.copy()
        drain_slice(cs)
        return result

    @override
    def __eq__(self, other: object) -> bool:
        return isinstance(other, _AnyTypeConstructor)

    @override
    def __repr__(self):
        return "Any"


AnyType = _AnyTypeConstructor()


@final
class _EnumTypeConstructor[T](TypeInfo[T]):
    """TypeInfo for simplified enum types (Unit, Bool, True, BoolFalse, Bit)."""

    def __init__(
        self,
        tag_len: int,
        to_tag: Callable[[T], int],
        from_tag: Callable[[int], T],
    ) -> None:
        self._tag_len = tag_len
        self._to_tag = to_tag
        self._from_tag = from_tag

    @override
    def serialize_value(self, value: T, builder: Builder) -> None:
        if self._tag_len == 0:
            return
        _ = builder.store_uint(self._to_tag(value), self._tag_len)

    @override
    def load_from(self, cs: Slice) -> T:
        if self._tag_len == 0:
            return self._from_tag(0)
        return self._from_tag(cs.load_uint(self._tag_len))

    @override
    def is_nonnull(self, value: T | None) -> TypeIs[T]:
        if self._tag_len == 0:
            return True  # Unit type: None is the only valid value
        return value is not None

    @override
    def __eq__(self, other: object) -> bool:
        return self is other


def _deser_true(tag: int) -> Literal[True]:
    if tag != 1:
        raise TlbModelError(f"expected tag 1 for True, got {tag}")
    return True


def _deser_false(tag: int) -> Literal[False]:
    if tag != 0:
        raise TlbModelError(f"expected tag 0 for False, got {tag}")
    return False


UnitTypeInfo: TypeInfo[None] = _EnumTypeConstructor(0, lambda _: 0, lambda _: None)
BoolTypeInfo: TypeInfo[bool] = _EnumTypeConstructor(1, int, bool)
TrueTypeInfo: TypeInfo[Literal[True]] = _EnumTypeConstructor(1, int, _deser_true)
FalseTypeInfo: TypeInfo[Literal[False]] = _EnumTypeConstructor(1, int, _deser_false)


@final
class _UnaryTypeConstructor(TypeInfo[int]):
    """TypeInfo for simplified Unary ~n → int. Ser/deser as unary encoding."""

    @override
    def serialize_value(self, value: int, builder: Builder) -> None:
        assert value >= 0
        for _ in range(value):
            _ = builder.store_uint(1, 1)
        _ = builder.store_uint(0, 1)

    @override
    def load_from(self, cs: Slice) -> int:
        n = 0
        while cs.load_bit():
            n += 1
        return n

    @override
    def __eq__(self, other: object) -> bool:
        return isinstance(other, _UnaryTypeConstructor)

    @override
    def __repr__(self) -> str:
        return "Unary"


UnaryTypeInfo = _UnaryTypeConstructor()


@final
class VarUIntTypeConstructor(TypeInfo[int]):
    """TypeInfo for VarUInteger n — variable-length unsigned integer."""

    def __init__(self, n: int) -> None:
        assert n > 0
        self._n = n

    @override
    def serialize_value(self, value: int, builder: Builder) -> None:
        assert value >= 0
        byte_len = (value.bit_length() + 7) // 8
        assert byte_len < self._n
        BoundedUintTypeConstructor(self._n, inclusive=False).serialize_value(byte_len, builder)
        if byte_len > 0:
            _ = builder.store_uint(value, byte_len * 8)

    @override
    def load_from(self, cs: Slice) -> int:
        byte_len = BoundedUintTypeConstructor(self._n, inclusive=False).load_from(cs)
        if byte_len == 0:
            return 0
        return cs.load_uint(byte_len * 8)

    @override
    def __eq__(self, other: object) -> bool:
        return isinstance(other, VarUIntTypeConstructor) and self._n == other._n

    @override
    def __repr__(self) -> str:
        return f"VarUInteger({self._n})"


@final
class VarIntTypeConstructor(TypeInfo[int]):
    """TypeInfo for VarInteger n — variable-length signed integer."""

    def __init__(self, n: int) -> None:
        assert n > 0
        self._n = n

    @override
    def serialize_value(self, value: int, builder: Builder) -> None:
        byte_len = (value.bit_length() + 8) // 8  # +1 for sign bit
        assert byte_len < self._n
        BoundedUintTypeConstructor(self._n, inclusive=False).serialize_value(byte_len, builder)
        if byte_len > 0:
            _ = builder.store_int(value, byte_len * 8)

    @override
    def load_from(self, cs: Slice) -> int:
        byte_len = BoundedUintTypeConstructor(self._n, inclusive=False).load_from(cs)
        if byte_len == 0:
            return 0
        return cs.load_int(byte_len * 8)

    @override
    def __eq__(self, other: object) -> bool:
        return isinstance(other, VarIntTypeConstructor) and self._n == other._n

    @override
    def __repr__(self) -> str:
        return f"VarInteger({self._n})"


@final
class MaybeTypeConstructor[X](TypeInfo[X | None]):
    """TypeInfo for simplified Maybe X → X | None. Stores inner TypeInfo."""

    def __init__(self, inner: TypeInfo[X]) -> None:
        self._inner = inner

    @override
    def serialize_value(self, value: X | None, builder: Builder) -> None:
        if value is not None:
            _ = builder.store_uint(1, 1)
            self._inner.serialize_value(value, builder)
        else:
            _ = builder.store_uint(0, 1)

    @override
    def load_from(self, cs: Slice) -> X | None:
        if cs.load_bit():
            return self._inner.load_from(cs)
        return None

    @override
    def is_nonnull(self, value: X | None) -> TypeIs[X | None]:
        return True

    @override
    def __eq__(self, other: object) -> bool:
        return isinstance(other, MaybeTypeConstructor) and self._inner == other._inner  # pyright: ignore[reportUnknownMemberType]

    @override
    def __repr__(self) -> str:
        return f"Maybe({self._inner!r})"


@final
class _CellRefTypeConstructor(TypeInfo[Cell]):
    """TypeInfo for ^Cell — opaque cell reference. Stores/loads entire cells."""

    @override
    def serialize_value(self, value: Cell, builder: Builder) -> None:
        _ = builder.store_ref(value)

    @override
    def load_from(self, cs: Slice) -> Cell:
        return cs.load_ref()

    @override
    def __eq__(self, other: object) -> bool:
        return isinstance(other, _CellRefTypeConstructor)

    @override
    def __repr__(self) -> str:
        return "^Cell"


CellRefType = _CellRefTypeConstructor()


@final
class UintTypeConstructor(TypeInfo[int]):
    def __init__(self, n: int):
        assert n >= 0
        self._n: int = n

    @property
    def n(self):
        return self._n

    @override
    def serialize_value(self, value: int, builder: Builder):
        assert value >= 0, f"uint{self._n}: negative value {value}"
        if self._n == 0:
            assert value == 0, f"uint0: only 0 is valid, got {value}"
        else:
            _ = builder.store_uint(value, self._n)

    @override
    def load_from(self, cs: Slice):
        if self._n == 0:
            return 0
        return cs.load_uint(self._n)

    @override
    def __eq__(self, other: object) -> bool:
        return isinstance(other, UintTypeConstructor) and self._n == other._n

    @override
    def __repr__(self):
        return f"uint{self._n}"


@final
class BoundedUintTypeConstructor(TypeInfo[int]):
    def __init__(self, bound: int, *, inclusive: bool):
        assert bound >= 0
        self._bound = bound
        self._inclusive = inclusive
        if inclusive:
            self._width = bound.bit_length()
            self._max = bound
        else:
            assert bound > 0
            self._width = (bound - 1).bit_length()
            self._max = bound - 1

    @override
    def serialize_value(self, value: int, builder: Builder):
        if value < 0 or value > self._max:
            op = "<=" if self._inclusive else "<"
            raise TlbModelError(f"value {value} out of range for #{op} {self._bound}")
        if self._width > 0:
            _ = builder.store_uint(value, self._width)

    @override
    def load_from(self, cs: Slice):
        value = cs.load_uint(self._width) if self._width > 0 else 0
        if value > self._max:
            op = "<=" if self._inclusive else "<"
            raise TlbModelError(f"value {value} out of range for #{op} {self._bound}")
        return value

    @override
    def __eq__(self, other: object) -> bool:
        return (
            isinstance(other, BoundedUintTypeConstructor)
            and self._bound == other._bound
            and self._inclusive == other._inclusive
        )

    @override
    def __repr__(self):
        op = "<=" if self._inclusive else "<"
        return f"#{op}{self._bound}"


@final
class IntTypeConstructor(TypeInfo[int]):
    def __init__(self, n: int):
        assert n >= 0
        self._n: int = n

    @property
    def n(self):
        return self._n

    @override
    def serialize_value(self, value: int, builder: Builder):
        if self._n == 0:
            assert value == 0, f"int0: only 0 is valid, got {value}"
        else:
            _ = builder.store_int(value, self._n)

    @override
    def load_from(self, cs: Slice):
        if self._n == 0:
            return 0
        return cs.load_int(self._n)

    @override
    def __eq__(self, other: object) -> bool:
        return isinstance(other, IntTypeConstructor) and self._n == other._n

    @override
    def __repr__(self):
        return f"int{self._n}"


@final
class BitsTypeConstructor(TypeInfo[bitarray]):
    def __init__(self, n: int):
        assert n >= 0
        self._n: int = n

    @property
    def n(self):
        return self._n

    @override
    def serialize_value(self, value: bitarray, builder: Builder):
        assert len(value) == self._n, f"bits{self._n}: expected {self._n} bits, got {len(value)}"
        if self._n > 0:
            _ = builder.store_bits(value)

    @override
    def load_from(self, cs: Slice) -> bitarray:
        if self._n == 0:
            return bitarray()
        return cs.load_bits(self._n)

    @override
    def __eq__(self, other: object) -> bool:
        return isinstance(other, BitsTypeConstructor) and self._n == other._n

    @override
    def __repr__(self):
        return f"bytes{self._n}"


@final
class TupleTypeConstructor[X](TypeInfo[list[X]]):
    def __init__(self, count: int, element_ti: TypeInfo[X]) -> None:
        assert count >= 0
        self._count = count
        self._element_ti = element_ti

    @override
    def serialize_value(self, value: list[X], builder: Builder) -> None:
        assert len(value) == self._count, (
            f"tuple: expected {self._count} elements, got {len(value)}"
        )
        for i in range(self._count):
            self._element_ti.serialize_value(value[i], builder)

    @override
    def load_from(self, cs: Slice) -> list[X]:
        result: list[X] = []
        for _ in range(self._count):
            result.append(self._element_ti.load_from(cs))
        return result

    @override
    def __eq__(self, other: object) -> bool:
        return (
            isinstance(other, TupleTypeConstructor)
            and self._count == other._count
            and self._element_ti == other._element_ti  # pyright: ignore[reportUnknownMemberType]
        )

    @override
    def __repr__(self) -> str:
        return f"Tuple({self._count}, {self._element_ti!r})"


def drain_slice(cs: Slice):
    _ = cs.skip_bits(cs.remaining_bits)
    while cs.remaining_refs:
        _ = cs.load_ref()
