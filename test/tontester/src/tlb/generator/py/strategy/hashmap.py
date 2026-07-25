"""HashmapStrategy / HashmapAugStrategy: simplified hashmap → HashmapDict."""

from typing import final, override

from ...sema.types import (
    AnonymousRecordType,
    CellRefType,
    ParamKind,
    TupleType,
    TypeApply,
    TypeParamRef,
    is_nat,
    is_type,
)
from ..context import PyContext
from ..name_scope import NameScope
from ..nat_expr import NatExpr
from ..source_builder import SourceBuilder
from ._base import StrategyBuilderProtocol, TypeStrategy


@final
class HashmapStrategy(TypeStrategy):
    """Simplified Hashmap/HashmapE → HashmapDict[V, None]."""

    def __init__(
        self,
        type_expr: TypeApply,
        ctx: PyContext,
        scope: NameScope,
        builder: StrategyBuilderProtocol,
        allow_empty: bool,
    ) -> None:
        self._ctx = ctx
        self._allow_empty = allow_empty
        ctx.use("HashmapDict")
        ctx.use("UnitTypeInfo")
        ctx.use("UnitAug")

        t = type_expr.type
        # First param is key_bits (nat), second is value type
        assert len(t.type_level_params) == 2
        assert t.type_level_params[0].kind == ParamKind.NAT
        assert t.type_level_params[1].kind == ParamKind.TYPE

        key_arg = type_expr.arguments[0]
        assert is_nat(key_arg)
        self._key_bits = NatExpr(key_arg, scope)
        self._key_bits_self = NatExpr(key_arg, scope)

        val_arg = type_expr.arguments[1]
        assert isinstance(
            val_arg, TypeParamRef | TypeApply | TupleType | CellRefType | AnonymousRecordType
        )
        val_strat = builder.build(val_arg)
        self._val_ti = val_strat.type_info_expr()
        self._val_ti_self = val_strat.type_info_expr_self()
        self._val_py_type = val_strat.py_type()

    @override
    def py_type(self) -> str:
        return f"HashmapDict[{self._val_py_type}, None]"

    @override
    def type_info_expr(self) -> str:
        allow = "True" if self._allow_empty else "False"
        return (
            f"{self.py_type()}.type_info("
            f"{self._key_bits.local}, {self._val_ti}, allow_empty={allow})"
        )

    @override
    def type_info_expr_self(self) -> str:
        return (
            f"{self.py_type()}.type_info("
            f"{self._key_bits_self.self_}, {self._val_ti_self},"
            f" allow_empty={self._allow_empty})"
        )

    @override
    def emit_serialize_assertions(self, field_name: str, sb: SourceBuilder) -> bool:
        key_expr = self._key_bits_self.self_
        allow = self._allow_empty
        sb.line(f"assert {field_name}.key_bits == {key_expr}")
        sb.line(f"assert {field_name}.value_ti == {self._val_ti_self}")
        sb.line(f"assert {field_name}.allow_empty is {allow}")
        sb.line(f"assert {field_name}.extra_ti is UnitTypeInfo")
        sb.line(f"assert {field_name}.aug is UnitAug")
        return True

    @override
    def emit_store(self, value: str, builder: str, sb: SourceBuilder) -> None:
        sb.line(f"{value}.serialize_to({builder})")

    @override
    def emit_load(self, target: str, cs: str, sb: SourceBuilder) -> None:
        allow = "True" if self._allow_empty else "False"
        sb.line(
            (
                f"{target} = {self.py_type()}.load_from("
                f"{cs}, {self._key_bits.local}, {self._val_ti}, allow_empty={allow})"
            )
        )

    @override
    def init_param_type(self) -> str:
        return f"{self.py_type()} | Mapping[int, {self._val_py_type}]"

    @override
    def emit_init_assignment(self, target: str, source: str, sb: SourceBuilder) -> None:
        self._ctx.use("Mapping")
        allow = "True" if self._allow_empty else "False"
        # Use self_-form expressions: by the time a field is assigned, the
        # constructor's params have already been written to `self`.
        sb.line(
            (
                f"{target} = {self.py_type()}.of({source}, "
                f"key_bits={self._key_bits_self.self_}, "
                f"value_ti={self._val_ti_self}, allow_empty={allow})"
            )
        )


@final
class HashmapAugStrategy(TypeStrategy):
    """Simplified HashmapAugE → HashmapDict[V, E] with an explicit augmentation.

    Only used when SimplifyConfig provides an aug class for the containing
    typedef. The aug class is referenced by bare name; it must be defined in
    the generated module (typically through aug_source splicing).
    """

    def __init__(
        self,
        type_expr: TypeApply,
        ctx: PyContext,
        scope: NameScope,
        builder: StrategyBuilderProtocol,
        aug_class: str,
    ) -> None:
        self._ctx = ctx
        self._aug_class = aug_class
        ctx.use("HashmapDict")

        t = type_expr.type
        # HashmapAugE has 3 type-level params: n (nat), X (value type), Y (extra type).
        assert len(t.type_level_params) == 3
        assert t.type_level_params[0].kind == ParamKind.NAT
        assert t.type_level_params[1].kind == ParamKind.TYPE
        assert t.type_level_params[2].kind == ParamKind.TYPE

        key_arg = type_expr.arguments[0]
        assert is_nat(key_arg)
        self._key_bits = NatExpr(key_arg, scope)
        self._key_bits_self = NatExpr(key_arg, scope)

        val_arg = type_expr.arguments[1]
        assert is_type(val_arg)
        assert isinstance(
            val_arg, TypeParamRef | TypeApply | TupleType | CellRefType | AnonymousRecordType
        )
        val_strat = builder.build(val_arg)
        self._val_ti = val_strat.type_info_expr()
        self._val_ti_self = val_strat.type_info_expr_self()
        self._val_py_type = val_strat.py_type()

        extra_arg = type_expr.arguments[2]
        assert is_type(extra_arg)
        assert isinstance(
            extra_arg,
            TypeParamRef | TypeApply | TupleType | CellRefType | AnonymousRecordType,
        )
        extra_strat = builder.build(extra_arg)
        self._extra_ti = extra_strat.type_info_expr()
        self._extra_ti_self = extra_strat.type_info_expr_self()
        self._extra_py_type = extra_strat.py_type()

    @override
    def py_type(self) -> str:
        return f"HashmapDict[{self._val_py_type}, {self._extra_py_type}]"

    @override
    def type_info_expr(self) -> str:
        # Augmented HashmapDict can't be expressed as a TypeInfo without
        # carrying the aug — that path isn't needed for current uses.
        raise AssertionError("TODO: HashmapAugStrategy used as a generic argument is not supported")

    @override
    def type_info_expr_self(self) -> str:
        raise AssertionError("TODO: HashmapAugStrategy used as a generic argument is not supported")

    @override
    def emit_serialize_assertions(self, field_name: str, sb: SourceBuilder) -> bool:
        key_expr = self._key_bits_self.self_
        sb.line(f"assert {field_name}.key_bits == {key_expr}")
        sb.line(f"assert {field_name}.value_ti == {self._val_ti_self}")
        sb.line(f"assert {field_name}.extra_ti == {self._extra_ti_self}")
        sb.line(f"assert isinstance({field_name}.aug, {self._aug_class})")
        return True

    @override
    def emit_store(self, value: str, builder: str, sb: SourceBuilder) -> None:
        sb.line(f"{value}.serialize_to({builder})")

    @override
    def emit_load(self, target: str, cs: str, sb: SourceBuilder) -> None:
        sb.line(
            (
                f"{target} = {self.py_type()}.load_from("
                f"{cs}, {self._key_bits.local}, {self._val_ti}, "
                f"extra_ti={self._extra_ti}, aug={self._aug_class}())"
            )
        )

    @override
    def init_param_type(self) -> str:
        return f"{self.py_type()} | Mapping[int, {self._val_py_type}]"

    @override
    def emit_init_assignment(self, target: str, source: str, sb: SourceBuilder) -> None:
        self._ctx.use("Mapping")
        sb.line(
            (
                f"{target} = {self.py_type()}.of({source}, "
                f"key_bits={self._key_bits_self.self_}, "
                f"value_ti={self._val_ti_self}, extra_ti={self._extra_ti_self}, "
                f"aug={self._aug_class}())"
            )
        )
