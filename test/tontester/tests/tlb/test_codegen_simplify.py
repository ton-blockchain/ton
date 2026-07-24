"""Tests for type simplifications."""

import pytest
from bitarray import bitarray
from generated.simplify import (
    coins,
    cond_maybe,
    dict_field,
    enum_fields,
    inferred_unary,
    just,
    maybe_ref,
    mixed,
    nested_maybe,
    nonempty_dict,
    nothing,
    pair,
    signed_val,
    simple_maybe,
    sized,
)
from tlb.hashmap import HashmapDict
from tlb.object import MaybeTypeConstructor, Ref, UintTypeConstructor, UnaryTypeInfo


class TestSimpleMaybe:
    """Maybe uint32 → int | None."""

    def test_just_roundtrip(self):
        obj = simple_maybe(x=42)
        result = simple_maybe.load_from(obj.serialize().begin_parse())
        assert result.x == 42

    def test_nothing_roundtrip(self):
        obj = simple_maybe(x=None)
        result = simple_maybe.load_from(obj.serialize().begin_parse())
        assert result.x is None

    def test_just_bit_count(self):
        cell = simple_maybe(x=0).serialize()
        assert cell.begin_parse().remaining_bits == 33  # 1 tag + 32 value

    def test_nothing_bit_count(self):
        cell = simple_maybe(x=None).serialize()
        assert cell.begin_parse().remaining_bits == 1  # just the 0 tag bit


class TestMixed:
    """Maybe in a record with other fields."""

    def test_just_roundtrip(self):
        obj = mixed(a=10, b=20, c=-5)
        result = mixed.load_from(obj.serialize().begin_parse())
        assert result.a == 10
        assert result.b == 20
        assert result.c == -5

    def test_nothing_roundtrip(self):
        obj = mixed(a=10, b=None, c=-5)
        result = mixed.load_from(obj.serialize().begin_parse())
        assert result.a == 10
        assert result.b is None
        assert result.c == -5


class TestNestedMaybe:
    """Maybe (Maybe uint32) — outer NOT simplified, uses full Maybe type.

    Inner Maybe uint32 is simplified to int | None, so the outer
    Maybe's type arg X = int | None. Outer stays as nothing | just[int | None].
    """

    def test_just_some_roundtrip(self):
        inner_ti = MaybeTypeConstructor(UintTypeConstructor(32))
        obj = nested_maybe(x=just(inner_ti, value=42))
        result = nested_maybe.load_from(obj.serialize().begin_parse())
        assert isinstance(result.x, just)
        assert result.x.value == 42

    def test_just_none_roundtrip(self):
        inner_ti = MaybeTypeConstructor(UintTypeConstructor(32))
        obj = nested_maybe(x=just(inner_ti, value=None))
        result = nested_maybe.load_from(obj.serialize().begin_parse())
        assert isinstance(result.x, just)
        assert result.x.value is None

    def test_nothing_roundtrip(self):
        obj = nested_maybe(x=nothing())
        result = nested_maybe.load_from(obj.serialize().begin_parse())
        assert isinstance(result.x, nothing)


class TestEnumLiterals:
    """Bool → bool, Unit → None, True → None (unit type)."""

    def test_all_fields_roundtrip(self):
        obj = enum_fields(a=True, b=None, c=None, x=42)
        result = enum_fields.load_from(obj.serialize().begin_parse())
        assert result.a is True
        assert result.b is None
        assert result.c is None
        assert result.x == 42

    def test_bool_false_roundtrip(self):
        obj = enum_fields(a=False, b=None, c=None, x=99)
        result = enum_fields.load_from(obj.serialize().begin_parse())
        assert result.a is False
        assert result.b is None
        assert result.c is None
        assert result.x == 99

    def test_serialized_bits(self):
        """a:Bool(1) + b:Unit(0) + c:True(0) + x:uint32(32) = 33 bits."""
        obj = enum_fields(a=True, b=None, c=None, x=0x12345678)
        cs = obj.serialize().begin_parse()
        assert cs.remaining_bits == 33
        assert cs.load_uint(1) == 1  # a = True
        # b = Unit, 0 bits
        # c = True, 0 bits (unit type)
        assert cs.load_uint(32) == 0x12345678  # x

    def test_bool_false_serialized_bits(self):
        obj = enum_fields(a=False, b=None, c=None, x=0)
        cs = obj.serialize().begin_parse()
        assert cs.load_uint(1) == 0  # a = False
        # c = True, 0 bits
        assert cs.load_uint(32) == 0


class TestUnary:
    """Unary ~n → int, with output param extraction."""

    def test_sized_zero(self):
        obj = sized(0, len=0, data=bitarray())
        result = sized.load_from(obj.serialize().begin_parse())
        assert result.len == 0
        assert result.n == 0
        assert result.data == bitarray()

    def test_sized_three(self):
        obj = sized(3, len=3, data=bitarray("101"))
        result = sized.load_from(obj.serialize().begin_parse())
        assert result.len == 3
        assert result.n == 3
        assert result.data == bitarray("101")

    def test_inferred_through_pair(self):
        """Output param inferred through Pair: x.first is the Unary value."""
        inner = pair(UnaryTypeInfo, UintTypeConstructor(32), first=2, second=99)
        obj = inferred_unary(2, x=inner, y=3)
        result = inferred_unary.load_from(obj.serialize().begin_parse())
        assert result.n == 2
        assert result.x.first == 2
        assert result.x.second == 99
        assert result.y == 3

    def test_inferred_zero(self):
        inner = pair(UnaryTypeInfo, UintTypeConstructor(32), first=0, second=42)
        obj = inferred_unary(0, x=inner, y=0)
        result = inferred_unary.load_from(obj.serialize().begin_parse())
        assert result.n == 0
        assert result.y == 0

    def test_serialized_bits(self):
        """Unary 3 = 1110 (4 bits), then 3 data bits."""
        obj = sized(3, len=3, data=bitarray("101"))
        cs = obj.serialize().begin_parse()
        assert cs.load_uint(1) == 1  # unary 1
        assert cs.load_uint(1) == 1  # unary 1
        assert cs.load_uint(1) == 1  # unary 1
        assert cs.load_uint(1) == 0  # unary terminator
        assert cs.load_bits(3) == bitarray("101")  # data


class TestMaybeRef:
    """Maybe ^uint32 → Ref[int] | None."""

    def test_just_roundtrip(self):
        obj = maybe_ref(x=Ref(UintTypeConstructor(32), 99))
        result = maybe_ref.load_from(obj.serialize().begin_parse())
        assert result.x is not None
        assert result.x.ref == 99

    def test_nothing_roundtrip(self):
        obj = maybe_ref(x=None)
        result = maybe_ref.load_from(obj.serialize().begin_parse())
        assert result.x is None


class TestCondMaybe:
    """Conditional field with simplified Maybe: flag?(Maybe uint32)."""

    def test_flag_set_with_value(self):
        obj = cond_maybe(flag=1, x=42)
        result = cond_maybe.load_from(obj.serialize().begin_parse())
        assert result.flag == 1
        assert result.x == 42

    def test_flag_set_with_nothing(self):
        obj = cond_maybe(flag=1, x=None)
        result = cond_maybe.load_from(obj.serialize().begin_parse())
        assert result.flag == 1
        assert result.x is None

    def test_flag_unset(self):
        obj = cond_maybe(flag=0, x=None)
        result = cond_maybe.load_from(obj.serialize().begin_parse())
        assert result.flag == 0
        assert result.x is None


class TestHashmapSimplification:
    """HashmapE n X → HashmapDict[X] field."""

    def _make_dict_field(self, entries: dict[int, int], extra: int = 0) -> dict_field:
        from pytoniq_core import Builder, HashMap

        d: HashmapDict[int] = HashmapDict(8, UintTypeConstructor(32))
        if entries:
            hm = HashMap(8)
            for k, v in entries.items():
                _ = hm.set_int_key(k, Builder().store_uint(v, 32).end_cell().begin_parse())  # pyright: ignore[reportUnknownMemberType]
            cell = hm.serialize()
            assert cell is not None
            d = HashmapDict(8, UintTypeConstructor(32), cell=cell)
        return dict_field(data=d, extra=extra)

    def test_empty_roundtrip(self):
        obj = self._make_dict_field({}, extra=42)
        result = dict_field.load_from(obj.serialize().begin_parse())
        assert result.data.is_empty()
        assert result.extra == 42

    def test_nonempty_roundtrip(self):
        obj = self._make_dict_field({1: 100, 5: 500, 255: 999}, extra=7)
        result = dict_field.load_from(obj.serialize().begin_parse())
        assert result.data[1] == 100
        assert result.data[5] == 500
        assert result.data[255] == 999
        assert result.extra == 7

    def test_mutation_roundtrip(self):
        obj = self._make_dict_field({1: 100, 2: 200}, extra=1)
        result = dict_field.load_from(obj.serialize().begin_parse())
        result.data[3] = 300
        del result.data[1]

        b = result.serialize()
        result2 = dict_field.load_from(b.begin_parse())
        assert result2.data.to_dict() == {2: 200, 3: 300}
        assert result2.extra == 1

    def test_iteration_sorted(self):
        obj = self._make_dict_field({255: 3, 0: 1, 128: 2})
        result = dict_field.load_from(obj.serialize().begin_parse())
        keys = list(result.data.keys())
        assert keys == sorted(keys)

    def test_serialized_bits(self):
        """Empty dict serializes as single 0 bit, followed by extra field."""
        obj = self._make_dict_field({}, extra=0xABCD)
        cs = obj.serialize().begin_parse()
        assert cs.load_uint(1) == 0  # hme_empty tag
        assert cs.load_uint(16) == 0xABCD

    def test_wrong_key_bits_caught(self):
        """HashmapDict with wrong key_bits is caught on serialize."""
        wrong_dict: HashmapDict[int] = HashmapDict(16, UintTypeConstructor(32))
        obj = dict_field(data=wrong_dict, extra=0)
        with pytest.raises(AssertionError):
            _ = obj.serialize()

    def test_wrong_value_ti_caught(self):
        """HashmapDict with wrong value TypeInfo is caught on serialize."""
        from tlb.object import IntTypeConstructor

        wrong_dict: HashmapDict[int] = HashmapDict(8, IntTypeConstructor(32))
        obj = dict_field(data=wrong_dict, extra=0)
        with pytest.raises(AssertionError):
            _ = obj.serialize()

    def test_wrong_allow_empty_caught(self):
        """HashmapDict with wrong allow_empty is caught on serialize."""
        wrong_dict: HashmapDict[int] = HashmapDict(8, UintTypeConstructor(32), allow_empty=False)
        obj = dict_field(data=wrong_dict, extra=0)
        with pytest.raises(AssertionError):
            _ = obj.serialize()

    def test_wrong_extra_ti_caught(self):
        """A different TypeInfo[None] (not UnitTypeInfo) is caught on serialize."""
        from tlb.object import (
            UnitTypeInfo,
            _EnumTypeConstructor,  # pyright: ignore[reportPrivateUsage]
        )

        fake_unit = _EnumTypeConstructor(0, lambda _: 0, lambda _: None)
        assert fake_unit is not UnitTypeInfo
        wrong_dict: HashmapDict[int] = HashmapDict(8, UintTypeConstructor(32), extra_ti=fake_unit)
        obj = dict_field(data=wrong_dict, extra=0)
        with pytest.raises(AssertionError):
            _ = obj.serialize()


class TestNonemptyHashmap:
    """Hashmap n X (allow_empty=False) → HashmapDict[X]."""

    def _make_nonempty(self, entries: dict[int, int]) -> nonempty_dict:
        from pytoniq_core import Builder, HashMap

        assert entries, "Hashmap (non-empty) must have at least one entry"
        hm = HashMap(8)
        for k, v in entries.items():
            _ = hm.set_int_key(k, Builder().store_uint(v, 32).end_cell().begin_parse())  # pyright: ignore[reportUnknownMemberType]
        cell = hm.serialize()
        assert cell is not None
        d: HashmapDict[int] = HashmapDict(8, UintTypeConstructor(32), cell=cell, allow_empty=False)
        return nonempty_dict(data=d)

    def test_roundtrip(self):
        obj = self._make_nonempty({10: 100, 20: 200})
        result = nonempty_dict.load_from(obj.serialize().begin_parse())
        assert result.data[10] == 100
        assert result.data[20] == 200

    def test_wrong_allow_empty_caught(self):
        """HashmapDict with allow_empty=True is caught for Hashmap field."""
        d: HashmapDict[int] = HashmapDict(8, UintTypeConstructor(32), allow_empty=True)
        obj = nonempty_dict(data=d)
        with pytest.raises(AssertionError):
            _ = obj.serialize()


class TestVarUInteger:
    """VarUInteger n → int."""

    def test_zero(self):
        obj = coins(amount=0)
        result = coins.load_from(obj.serialize().begin_parse())
        assert result.amount == 0

    def test_small(self):
        obj = coins(amount=255)
        result = coins.load_from(obj.serialize().begin_parse())
        assert result.amount == 255

    def test_large(self):
        obj = coins(amount=10**18)
        result = coins.load_from(obj.serialize().begin_parse())
        assert result.amount == 10**18

    def test_serialized_zero(self):
        """VarUInteger 16 with amount=0: len=0 (4 bits), no value bytes."""
        cs = coins(amount=0).serialize().begin_parse()
        assert cs.load_uint(4) == 0  # len
        assert cs.remaining_bits == 0

    def test_serialized_small(self):
        """VarUInteger 16 with amount=255: len=1, value=1 byte."""
        cs = coins(amount=255).serialize().begin_parse()
        assert cs.load_uint(4) == 1  # len = 1 byte
        assert cs.load_uint(8) == 255

    def test_serialized_two_bytes(self):
        """VarUInteger 16 with amount=256: len=2, value=2 bytes."""
        cs = coins(amount=256).serialize().begin_parse()
        assert cs.load_uint(4) == 2
        assert cs.load_uint(16) == 256


class TestVarInteger:
    """VarInteger n → int (signed)."""

    def test_zero(self):
        obj = signed_val(x=0)
        result = signed_val.load_from(obj.serialize().begin_parse())
        assert result.x == 0

    def test_positive(self):
        obj = signed_val(x=127)
        result = signed_val.load_from(obj.serialize().begin_parse())
        assert result.x == 127

    def test_negative(self):
        obj = signed_val(x=-42)
        result = signed_val.load_from(obj.serialize().begin_parse())
        assert result.x == -42

    def test_serialized_negative(self):
        """VarInteger 4 with x=-1: len=1, value=0xFF (signed -1 in 8 bits)."""
        cs = signed_val(x=-1).serialize().begin_parse()
        assert cs.load_uint(2) == 1  # #< 4 → 2 bits, len=1
        assert cs.load_int(8) == -1

    def test_serialized_positive(self):
        """VarInteger 4 with x=256: len=2, value=256 in 16 bits."""
        cs = signed_val(x=256).serialize().begin_parse()
        assert cs.load_uint(2) == 2  # len=2
        assert cs.load_int(16) == 256
