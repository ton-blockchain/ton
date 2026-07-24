"""Tests for serialize/deserialize validation assertions."""

import pytest
from bitarray import bitarray
from generated.validation import (
    Anon_1,
    ValType,
    constrained,
    container_empty,
    container_full,
    container_parent,
    eq_field,
    fixed_bits,
    inline_generic,
    just,
    nat_field,
    nothing,
    ordered,
    pair,
    typed_parent,
    val_n,
    val_parent,
    val_zero,
    var_bits_field,
)
from pytoniq_core import Builder
from tlb.object import TlbModelError


class TestSerializeNatParamNonNegative:
    """serialize_to asserts nat params >= 0."""

    def test_valid_nat_param(self):
        obj = val_n(n=4, x=10)
        _ = obj.serialize()

    def test_negative_nat_param(self):
        obj = val_n(n=-1, x=0)
        with pytest.raises(AssertionError):
            _ = obj.serialize()


class TestSerializeFieldConstraints:
    """serialize_to re-checks field constraints."""

    def test_constrained_valid(self):
        obj = constrained(flags=1, x=42)
        result = constrained.load_from(obj.serialize().begin_parse())
        assert result.flags == 1
        assert result.x == 42

    def test_constrained_zero(self):
        obj = constrained(flags=0, x=99)
        result = constrained.load_from(obj.serialize().begin_parse())
        assert result.flags == 0

    def test_constrained_too_high(self):
        """flags > 1 fails assertion during serialization."""
        obj = constrained(flags=2, x=0)
        with pytest.raises(AssertionError):
            _ = obj.serialize()

    def test_ordered_valid(self):
        obj = ordered(a=3, b=5)
        result = ordered.load_from(obj.serialize().begin_parse())
        assert result.a == 3 and result.b == 5

    def test_ordered_equal(self):
        obj = ordered(a=10, b=10)
        result = ordered.load_from(obj.serialize().begin_parse())
        assert result.a == result.b == 10

    def test_ordered_wrong(self):
        """b < a fails assertion during serialization."""
        obj = ordered(a=5, b=3)
        with pytest.raises(AssertionError):
            _ = obj.serialize()


class TestSerializeSubtypeConsistency:
    """serialize_to asserts sub-type nat params match parent expectations."""

    def test_parent_with_matching_val_n(self):
        inner = val_n(n=4, x=10)
        obj = val_parent(n=4, inner=inner)
        result = val_parent.load_from(obj.serialize().begin_parse())
        assert result.n == 4
        assert isinstance(result.inner, val_n)
        assert result.inner.x == 10

    def test_parent_with_val_zero(self):
        obj = val_parent(n=0, inner=val_zero())
        result = val_parent.load_from(obj.serialize().begin_parse())
        assert result.n == 0
        assert isinstance(result.inner, val_zero)

    def test_parent_with_wrong_val_zero(self):
        """val_zero is Val 0, but parent claims n=5 — assertion catches mismatch."""
        obj = val_parent(n=5, inner=val_zero())
        with pytest.raises(AssertionError):
            _ = obj.serialize()

    def test_parent_with_wrong_val_n(self):
        """val_n(n=3) is Val 3, but parent claims n=5 — assertion catches mismatch."""
        inner = val_n(n=3, x=5)
        obj = val_parent(n=5, inner=inner)
        with pytest.raises(AssertionError):
            _ = obj.serialize()

    def test_mutated_inner_caught(self):
        """Deserialize correctly, then mutate inner, serialization catches it."""
        obj = val_parent(n=4, inner=val_n(n=4, x=10))
        result = val_parent.load_from(obj.serialize().begin_parse())
        result.inner = val_zero()
        with pytest.raises(AssertionError):
            _ = result.serialize()


class TestDeserializeNatParamNonNegative:
    """load_from asserts nat type-level params >= 0."""

    def test_valid_type_arg(self):
        obj = val_n(n=4, x=10)
        result = ValType().load_from(obj.serialize().begin_parse(), 4)
        assert isinstance(result, val_n)

    def test_negative_type_arg_type_info(self):
        """Negative nat type arg to TypeInfo.load_from triggers assertion."""
        b = Builder()
        _ = b.store_uint(1, 1)  # tag for val_n
        with pytest.raises(AssertionError):
            _ = ValType().load_from(b.end_cell().begin_parse(), -1)

    def test_negative_type_arg_constructor(self):
        """Negative nat type arg to constructor load_from triggers assertion."""
        b = Builder()
        _ = b.store_uint(1, 1)  # tag for val_n
        with pytest.raises(AssertionError):
            _ = val_n.load_from(b.end_cell().begin_parse(), -1)


class TestNatFieldAsOutputParam:
    """m:# = NatField ~m — explicit field exposed as output type-level param."""

    def test_roundtrip(self):
        obj = nat_field(m=42)
        result = nat_field.load_from(obj.serialize().begin_parse())
        assert result.m == 42

    def test_get_output(self):
        obj = nat_field(m=7)
        assert obj.get_output(0) == 7


class TestEqFieldConstraint:
    """{m:#} n:# { m = n } = EqField m — implicit param constrained to equal a field."""

    def test_roundtrip(self):
        obj = eq_field(m=42, n=42)
        result = eq_field.load_from(obj.serialize().begin_parse(), 42)
        assert result.m == 42 and result.n == 42

    def test_serialize_mismatch(self):
        """m != n fails assertion during serialization."""
        obj = eq_field(m=5, n=10)
        with pytest.raises(AssertionError):
            _ = obj.serialize()

    def test_deserialize_mismatch(self):
        """Stream has n=10 but type arg m=5 — constraint fails on load."""
        b = Builder()
        _ = b.store_uint(10, 32)
        with pytest.raises(TlbModelError, match="constraint failed"):
            _ = eq_field.load_from(b.end_cell().begin_parse(), 5)


class TestVarWidthBits:
    """Variable-width bits field: {n:#} s:(bits n) = VarBitsField n."""

    def test_roundtrip(self):
        obj = var_bits_field(n=4, s=bitarray("1010"))
        result = var_bits_field.load_from(obj.serialize().begin_parse(), 4)
        assert result.s == bitarray("1010")

    def test_zero_width(self):
        obj = var_bits_field(n=0, s=bitarray())
        result = var_bits_field.load_from(obj.serialize().begin_parse(), 0)
        assert result.s == bitarray()

    def test_wrong_length_on_serialize(self):
        """bits n with n=4 rejects a 2-bit value on serialize."""
        obj = var_bits_field(n=4, s=bitarray("10"))
        with pytest.raises(AssertionError):
            _ = obj.serialize()

    def test_constant_width_roundtrip(self):
        obj = fixed_bits(s=bitarray("10101010"))
        result = fixed_bits.load_from(obj.serialize().begin_parse())
        assert result.s == bitarray("10101010")

    def test_constant_width_wrong_length(self):
        """Constant-width bits8 rejects wrong length on serialize."""
        obj = fixed_bits(s=bitarray("1"))
        with pytest.raises(AssertionError):
            _ = obj.serialize()


class TestInlineRecordAsGenericArg:
    """Inline record [a:uint32 b:uint64] as argument to Maybe."""

    def test_just_roundtrip(self):
        inner = Anon_1(a=10, b=20)
        obj = inline_generic(x=just(Anon_1, value=inner))
        result = inline_generic.load_from(obj.serialize().begin_parse())
        assert isinstance(result.x, just)
        assert result.x.value.a == 10
        assert result.x.value.b == 20

    def test_nothing_roundtrip(self):
        obj = inline_generic(x=nothing())
        result = inline_generic.load_from(obj.serialize().begin_parse())
        assert isinstance(result.x, nothing)


class TestTypeInfoEquality:
    """TypeInfo __eq__ works correctly for runtime and generated types."""

    def test_uint_equal(self):
        from tlb.object import UintTypeConstructor

        assert UintTypeConstructor(32) == UintTypeConstructor(32)
        assert UintTypeConstructor(32) != UintTypeConstructor(64)

    def test_bounded_uint_equal(self):
        from tlb.object import BoundedUintTypeConstructor

        assert BoundedUintTypeConstructor(100, inclusive=True) == BoundedUintTypeConstructor(
            100, inclusive=True
        )
        assert BoundedUintTypeConstructor(100, inclusive=True) != BoundedUintTypeConstructor(
            100, inclusive=False
        )
        assert BoundedUintTypeConstructor(100, inclusive=True) != BoundedUintTypeConstructor(
            50, inclusive=True
        )

    def test_int_equal(self):
        from tlb.object import IntTypeConstructor

        assert IntTypeConstructor(8) == IntTypeConstructor(8)
        assert IntTypeConstructor(8) != IntTypeConstructor(16)

    def test_bits_equal(self):
        from tlb.object import BitsTypeConstructor

        assert BitsTypeConstructor(256) == BitsTypeConstructor(256)
        assert BitsTypeConstructor(256) != BitsTypeConstructor(128)

    def test_tuple_equal(self):
        from tlb.object import TupleTypeConstructor, UintTypeConstructor

        a = TupleTypeConstructor(3, UintTypeConstructor(8))
        b = TupleTypeConstructor(3, UintTypeConstructor(8))
        c = TupleTypeConstructor(3, UintTypeConstructor(16))
        d = TupleTypeConstructor(4, UintTypeConstructor(8))
        assert a == b
        assert a != c
        assert a != d

    def test_generated_type_info_equal(self):
        assert ValType() == ValType()
        assert constrained == constrained
        assert ValType() != constrained

    def test_instantiated_equal(self):
        a = ValType.instantiate(4)
        b = ValType.instantiate(4)
        c = ValType.instantiate(5)
        assert a == b
        assert a != c

    def test_cross_type_not_equal(self):
        from tlb.object import IntTypeConstructor, UintTypeConstructor

        assert UintTypeConstructor(32) != IntTypeConstructor(32)


class TestCheckType:
    """check_type verifies type params match during serialization."""

    def test_typed_parent_roundtrip(self):
        from tlb.object import UintTypeConstructor

        inner = pair(UintTypeConstructor(32), UintTypeConstructor(64), first=10, second=20)
        obj = typed_parent(inner=inner)
        result = typed_parent.load_from(obj.serialize().begin_parse())
        assert result.inner.first == 10
        assert result.inner.second == 20

    def test_typed_parent_wrong_type_arg(self):
        """Pair constructed with uint8 but parent expects uint32 — caught on serialize."""
        from tlb.object import UintTypeConstructor

        inner = pair(UintTypeConstructor(8), UintTypeConstructor(64), first=10, second=20)
        obj = typed_parent(inner=inner)
        with pytest.raises(AssertionError):
            _ = obj.serialize()

    def test_check_type_direct(self):
        """check_type on pair directly."""
        from tlb.object import IntTypeConstructor, UintTypeConstructor

        p = pair(UintTypeConstructor(32), UintTypeConstructor(64), first=1, second=2)
        p.check_type(0, UintTypeConstructor(32))
        p.check_type(1, UintTypeConstructor(64))
        with pytest.raises(AssertionError):
            p.check_type(0, IntTypeConstructor(32))

    def test_check_type_skips_untracked_param(self):
        """container_empty doesn't track X, so check_type(0, ...) is a no-op."""
        from tlb.object import UintTypeConstructor

        obj = container_empty(UintTypeConstructor(64), extra=42)
        # idx=0 (X) — not tracked, should silently pass regardless of what ti is
        obj.check_type(0, UintTypeConstructor(32))
        obj.check_type(0, UintTypeConstructor(999))
        # idx=1 (Y) — tracked, should assert
        obj.check_type(1, UintTypeConstructor(64))
        with pytest.raises(AssertionError):
            obj.check_type(1, UintTypeConstructor(32))
        # idx=2 — not a type param at all, should raise ValueError
        with pytest.raises(ValueError, match="no type param at index 2"):
            obj.check_type(2, UintTypeConstructor(32))

    def test_container_parent_empty_roundtrip(self):
        """Wrapper serializing container_empty — exercises check_type on untracked X param."""
        from tlb.object import UintTypeConstructor

        inner = container_empty(UintTypeConstructor(64), extra=100)
        obj = container_parent(inner=inner)
        result = container_parent.load_from(obj.serialize().begin_parse())
        assert isinstance(result.inner, container_empty)
        assert result.inner.extra == 100

    def test_container_parent_full_roundtrip(self):
        """Wrapper serializing container_full — all type params tracked and checked."""
        from tlb.object import UintTypeConstructor

        inner = container_full(UintTypeConstructor(32), UintTypeConstructor(64), value=7, extra=200)
        obj = container_parent(inner=inner)
        result = container_parent.load_from(obj.serialize().begin_parse())
        assert isinstance(result.inner, container_full)
        assert result.inner.value == 7
        assert result.inner.extra == 200

    def test_container_parent_full_wrong_type(self):
        """container_full with wrong X type — caught by check_type on serialize."""
        from tlb.object import UintTypeConstructor

        inner = container_full(UintTypeConstructor(8), UintTypeConstructor(64), value=7, extra=200)
        obj = container_parent(inner=inner)
        with pytest.raises(AssertionError):
            _ = obj.serialize()


class TestDeserializeConstraints:
    """load_from raises TlbModelError on constraint violations."""

    def test_constrained_invalid_on_load(self):
        """flags=2 in the stream violates { flags <= 1 }."""
        b = Builder()
        _ = b.store_uint(2, 8)
        _ = b.store_uint(0, 32)
        with pytest.raises(TlbModelError, match="constraint failed"):
            _ = constrained.load_from(b.end_cell().begin_parse())

    def test_ordered_invalid_on_load(self):
        """a=10, b=5 in the stream violates { b >= a }."""
        b = Builder()
        _ = b.store_uint(10, 32)
        _ = b.store_uint(5, 32)
        with pytest.raises(TlbModelError, match="constraint failed"):
            _ = ordered.load_from(b.end_cell().begin_parse())

    def test_wrong_type_arg_for_val_zero(self):
        """val_zero expects type_arg_0 == 0, passing 5 triggers TlbModelError."""
        obj = val_zero()
        with pytest.raises(TlbModelError, match="constraint failed"):
            _ = val_zero.load_from(obj.serialize().begin_parse(), 5)
