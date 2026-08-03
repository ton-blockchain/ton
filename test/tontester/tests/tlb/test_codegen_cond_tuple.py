"""Tests for generated Python code from TL-B cond_tuple schema."""

from generated.cond_tuple import bit as ct_bit
from generated.cond_tuple import (
    fixed_bits,
    generic_opt,
    multi_opt,
    optional,
    var_bits,
)
from tlb.object import MaybeTypeConstructor, UintTypeConstructor, UnitTypeInfo

# ── Conditional fields ────────────────────────────────────────────────


class TestConditional:
    def test_optional_present(self):
        obj = optional(has_x=1, x=42)
        result = optional.load_from(obj.serialize().begin_parse())
        assert result.has_x == 1
        assert result.x == 42

    def test_optional_absent(self):
        obj = optional(has_x=0, x=None)
        result = optional.load_from(obj.serialize().begin_parse())
        assert result.has_x == 0
        assert result.x is None

    def test_optional_present_bit_count(self):
        """has_x (1 bit) + x (32 bits) = 33 bits when present."""
        cell = optional(has_x=1, x=0).serialize()
        assert cell.begin_parse().remaining_bits == 33

    def test_optional_absent_bit_count(self):
        """has_x (1 bit) only when absent."""
        cell = optional(has_x=0, x=None).serialize()
        assert cell.begin_parse().remaining_bits == 1

    def test_multi_opt_all_present(self):
        obj = multi_opt(flags=0b11, a=10, b=-5)
        result = multi_opt.load_from(obj.serialize().begin_parse())
        assert result.flags == 0b11
        assert result.a == 10
        assert result.b == -5

    def test_multi_opt_none_present(self):
        obj = multi_opt(flags=0, a=None, b=None)
        result = multi_opt.load_from(obj.serialize().begin_parse())
        assert result.a is None
        assert result.b is None

    def test_multi_opt_first_only(self):
        obj = multi_opt(flags=0b01, a=99, b=None)
        result = multi_opt.load_from(obj.serialize().begin_parse())
        assert result.a == 99
        assert result.b is None


# ── Tuple fields ──────────────────────────────────────────────────────


class TestTuple:
    def test_fixed_bits_roundtrip(self):
        bits = [ct_bit(1), ct_bit(0), ct_bit(1)]
        obj = fixed_bits(s=bits)
        result = fixed_bits.load_from(obj.serialize().begin_parse())
        assert len(result.s) == 3

    def test_fixed_bits_count(self):
        """3 * Bit = 3 * (## 1) = 3 bits, but each Bit also has its own (## 1) field."""
        bits = [ct_bit(0), ct_bit(0), ct_bit(0)]
        cell = fixed_bits(s=bits).serialize()
        assert cell.begin_parse().remaining_bits == 3

    def test_var_bits_roundtrip(self):
        bits = [ct_bit(1), ct_bit(1), ct_bit(0), ct_bit(1)]
        obj = var_bits(n=4, s=bits)
        result = var_bits.load_from(obj.serialize().begin_parse())
        assert result.n == 4
        assert len(result.s) == 4

    def test_var_bits_empty(self):
        obj = var_bits(n=0, s=[])
        result = var_bits.load_from(obj.serialize().begin_parse())
        assert result.n == 0
        assert result.s == []


# ── Generic conditional fields ───────────────────────────────────────


class TestGenericOpt:
    """GenericOpt X: has_x?(X) where X can be non-nullable or nullable."""

    def test_nonnull_type_present(self):
        """X = uint32 (non-nullable), flag set."""
        ti = UintTypeConstructor(32)
        obj = generic_opt[int](ti, has_x=1, x=99)
        result = generic_opt[int].load_from(obj.serialize().begin_parse(), ti)
        assert result.has_x == 1
        assert result.x == 99

    def test_nonnull_type_absent(self):
        """X = uint32, flag unset."""
        ti = UintTypeConstructor(32)
        obj = generic_opt[int](ti, has_x=0, x=None)
        result = generic_opt[int].load_from(obj.serialize().begin_parse(), ti)
        assert result.has_x == 0
        assert result.x is None

    def test_maybe_type_present_with_value(self):
        """X = Maybe uint32 (nullable), flag set, value present."""
        ti = MaybeTypeConstructor(UintTypeConstructor(32))
        obj = generic_opt[int | None](ti, has_x=1, x=42)
        result = generic_opt[int | None].load_from(obj.serialize().begin_parse(), ti)
        assert result.has_x == 1
        assert result.x == 42

    def test_maybe_type_present_with_nothing(self):
        """X = Maybe uint32 (nullable), flag set, value is Nothing (None)."""
        ti = MaybeTypeConstructor(UintTypeConstructor(32))
        obj = generic_opt[int | None](ti, has_x=1, x=None)
        result = generic_opt[int | None].load_from(obj.serialize().begin_parse(), ti)
        assert result.has_x == 1
        assert result.x is None

    def test_maybe_type_absent(self):
        """X = Maybe uint32, flag unset."""
        ti = MaybeTypeConstructor(UintTypeConstructor(32))
        obj = generic_opt[int | None](ti, has_x=0, x=None)
        result = generic_opt[int | None].load_from(obj.serialize().begin_parse(), ti)
        assert result.has_x == 0
        assert result.x is None

    def test_unit_type_present(self):
        """X = Unit (always None), flag set."""
        obj = generic_opt[None](UnitTypeInfo, has_x=1, x=None)
        result = generic_opt[None].load_from(obj.serialize().begin_parse(), UnitTypeInfo)
        assert result.has_x == 1
        assert result.x is None

    def test_unit_type_absent(self):
        """X = Unit, flag unset."""
        obj = generic_opt[None](UnitTypeInfo, has_x=0, x=None)
        result = generic_opt[None].load_from(obj.serialize().begin_parse(), UnitTypeInfo)
        assert result.has_x == 0
        assert result.x is None
