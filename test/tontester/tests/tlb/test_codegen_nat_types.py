"""Tests for generated Python code from TL-B nat_types schema."""

import pytest
from generated.nat_types import (
    hash32,
    hash_width,
    just,
    leq_test,
    lt_nested,
    lt_param,
    lt_test,
    multi_nat,
    nothing,
    zero_width,
)
from tlb.object import TlbModelError

# ── Nat builtins ─────────────────────────────────────────────────────


class TestNatBuiltins:
    def test_hash32_roundtrip(self):
        assert hash32.load_from(hash32(n=42).serialize().begin_parse()).n == 42

    def test_hash32_bit_count(self):
        assert hash32(n=0).serialize().begin_parse().remaining_bits == 32

    def test_hash_width_roundtrip(self):
        assert hash_width.load_from(hash_width(n=31).serialize().begin_parse()).n == 31

    def test_hash_width_bit_count(self):
        assert hash_width(n=0).serialize().begin_parse().remaining_bits == 5

    def test_leq_roundtrip(self):
        assert leq_test.load_from(leq_test(n=100).serialize().begin_parse()).n == 100

    def test_leq_bit_count(self):
        assert leq_test(n=0).serialize().begin_parse().remaining_bits == (100).bit_length()

    def test_lt_roundtrip(self):
        assert lt_test.load_from(lt_test(n=15).serialize().begin_parse()).n == 15

    def test_lt_bit_count(self):
        assert lt_test(n=0).serialize().begin_parse().remaining_bits == (15).bit_length()

    def test_multi_nat_roundtrip(self):
        r = multi_nat.load_from(multi_nat(a=7, b=5, c=42).serialize().begin_parse())
        assert (r.a, r.b, r.c) == (7, 5, 42)

    def test_zero_width_roundtrip(self):
        r = zero_width.load_from(zero_width(n=0).serialize().begin_parse())
        assert r.n == 0

    def test_zero_width_bit_count(self):
        assert zero_width(n=0).serialize().begin_parse().remaining_bits == 0

    def test_lt_param_roundtrip(self):
        obj = lt_param(4, x=3)
        result = lt_param.load_from(obj.serialize().begin_parse(), 4)
        assert result.x == 3
        assert result.n == 4

    def test_lt_param_zero_rejected(self):
        """#< n with n=0 is rejected at runtime by the implicit constraint."""
        with pytest.raises(TlbModelError, match="constraint failed"):
            _ = lt_param.load_from(lt_param(1, x=0).serialize().begin_parse(), 0)

    def test_lt_nested_roundtrip(self):
        """#< n nested inside Maybe — round-trips with just value."""
        from tlb.object import BoundedUintTypeConstructor

        ti = BoundedUintTypeConstructor(4, inclusive=False)
        obj = lt_nested(4, x=just(ti, value=3))
        result = lt_nested.load_from(obj.serialize().begin_parse(), 4)
        assert isinstance(result.x, just)
        assert result.x.value == 3

    def test_lt_nested_nothing(self):
        """#< n nested inside Maybe — round-trips with nothing."""
        obj = lt_nested(4, x=nothing())
        result = lt_nested.load_from(obj.serialize().begin_parse(), 4)
        assert isinstance(result.x, nothing)

    def test_lt_nested_zero_rejected(self):
        """#< n nested inside Maybe — n=0 rejected before reading."""
        with pytest.raises(TlbModelError, match="constraint failed"):
            _ = lt_nested.load_from(lt_nested(1, x=nothing()).serialize().begin_parse(), 0)


class TestBoundsValidation:
    """#< and #<= reject out-of-range values."""

    def test_leq_accepts_max(self):
        """#<= 100 accepts value == bound."""
        obj = leq_test(n=100)
        assert leq_test.load_from(obj.serialize().begin_parse()).n == 100

    def test_leq_accepts_zero(self):
        """#<= 100 accepts value 0."""
        obj = leq_test(n=0)
        assert leq_test.load_from(obj.serialize().begin_parse()).n == 0

    def test_leq_rejects_over_max(self):
        """#<= 100 rejects value > bound on load."""
        from pytoniq_core import Builder

        b = Builder()
        _ = b.store_uint(101, (100).bit_length())
        with pytest.raises(TlbModelError, match="out of range"):
            _ = leq_test.load_from(b.end_cell().begin_parse())

    def test_leq_rejects_negative_on_store(self):
        """#<= 100 rejects negative values on store."""
        with pytest.raises(TlbModelError, match="out of range"):
            _ = leq_test(n=-1).serialize()

    def test_lt_accepts_max(self):
        """#< 16 accepts value == bound - 1."""
        obj = lt_test(n=15)
        assert lt_test.load_from(obj.serialize().begin_parse()).n == 15

    def test_lt_accepts_zero(self):
        """#< 16 accepts value 0."""
        obj = lt_test(n=0)
        assert lt_test.load_from(obj.serialize().begin_parse()).n == 0

    def test_lt_rejects_at_bound(self):
        """#< n rejects value == bound on load.

        #< 5 stores 3 bits (raw range 0..7) with bound 0..4 — value 5
        fits in bits but is out of range.
        """
        from pytoniq_core import Builder

        b = Builder()
        _ = b.store_uint(5, (4).bit_length())
        with pytest.raises(TlbModelError, match="out of range"):
            _ = lt_param.load_from(b.end_cell().begin_parse(), 5)

    def test_lt_rejects_negative_on_store(self):
        """#< 16 rejects negative values on store."""
        with pytest.raises(TlbModelError, match="out of range"):
            _ = lt_test(n=-1).serialize()
