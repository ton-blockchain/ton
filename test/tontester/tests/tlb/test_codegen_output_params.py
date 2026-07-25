"""Tests for generated code with output params (~)."""

import pytest
from generated.output_params import (
    HmNodeType,
    UnaryType,
    bit,
    hm_node_fork,
    hm_node_leaf,
    inferred,
    label,
    mc_inferred,
    mc_left,
    mc_right,
    pair,
    sized,
    unary_succ,
    unary_zero,
)
from pytoniq_core import Builder
from tlb.object import Ref, TlbModelError, UintTypeConstructor


class TestUnary:
    """Unary ~n: output param computed from tag structure."""

    def test_zero(self):
        obj = unary_zero()
        result = UnaryType().load_from(obj.serialize().begin_parse())
        assert isinstance(result, unary_zero)
        assert result.get_output(0) == 0

    def test_one(self):
        obj = unary_succ(0, x=unary_zero())
        result = UnaryType().load_from(obj.serialize().begin_parse())
        assert isinstance(result, unary_succ)
        assert result.get_output(0) == 1

    def test_three(self):
        obj = unary_succ(2, x=unary_succ(1, x=unary_succ(0, x=unary_zero())))
        result = UnaryType().load_from(obj.serialize().begin_parse())
        assert isinstance(result, unary_succ)
        assert result.get_output(0) == 3

    def test_bit_count_zero(self):
        assert unary_zero().serialize().begin_parse().remaining_bits == 1

    def test_bit_count_three(self):
        # 1 + 1 + 1 + 0 = 4 bits
        obj = unary_succ(2, x=unary_succ(1, x=unary_succ(0, x=unary_zero())))
        assert obj.serialize().begin_parse().remaining_bits == 4


class TestSized:
    """Sized: len:(Unary ~n) data:(n * Bit) — output param determines field width."""

    def test_empty(self):
        obj = sized(0, len=unary_zero(), data=[])
        result = sized.load_from(obj.serialize().begin_parse())
        assert isinstance(result, sized)
        assert result.data == []

    def test_three_bits(self):
        obj = sized(
            3,
            len=unary_succ(2, x=unary_succ(1, x=unary_succ(0, x=unary_zero()))),
            data=[bit(1), bit(0), bit(1)],
        )
        result = sized.load_from(obj.serialize().begin_parse())
        assert isinstance(result, sized)
        assert len(result.data) == 3
        # Verify by re-serializing individual bits
        assert result.data[0].field == 1
        assert result.data[1].field == 0
        assert result.data[2].field == 1


class TestInferred:
    """Inferred ~n: output param extracted through inference chain (Pair first field)."""

    def test_roundtrip_n_zero(self):
        uint32_ti = UintTypeConstructor(32)
        inner_pair = pair[unary_zero | unary_succ, int](
            UnaryType(), uint32_ti, first=unary_zero(), second=42
        )
        obj = inferred(0, x=inner_pair, y=0)
        result = inferred.load_from(obj.serialize().begin_parse())
        assert isinstance(result, inferred)
        assert result.n == 0
        assert result.get_output(0) == 0
        assert result.y == 0

    def test_roundtrip_n_two(self):
        uint32_ti = UintTypeConstructor(32)
        unary_2 = unary_succ(1, x=unary_succ(0, x=unary_zero()))
        inner_pair = pair[unary_zero | unary_succ, int](
            UnaryType(), uint32_ti, first=unary_2, second=99
        )
        obj = inferred(2, x=inner_pair, y=3)
        result = inferred.load_from(obj.serialize().begin_parse())
        assert isinstance(result, inferred)
        assert result.n == 2
        assert result.get_output(0) == 2
        assert result.y == 3


class TestLabel:
    """Label ~n m: output param is a NatFieldValue (explicit field n)."""

    def test_roundtrip(self):
        obj = label(8, n=5, s=[bit(1), bit(0), bit(1), bit(1), bit(0)])
        result = label.load_from(obj.serialize().begin_parse(), 8)
        assert isinstance(result, label)
        assert result.n == 5
        assert result.get_output(0) == 5
        assert len(result.s) == 5

    def test_zero_length(self):
        obj = label(8, n=0, s=[])
        result = label.load_from(obj.serialize().begin_parse(), 8)
        assert result.n == 0
        assert result.s == []


class TestHmNode:
    """HmNode (n+1) X: compound SolveConstraint (n = type_arg_0 - 1)."""

    def test_leaf(self):
        uint32_ti = UintTypeConstructor(32)
        obj = hm_node_leaf[int](uint32_ti, value=42)
        result = HmNodeType[int]().load_from(obj.serialize().begin_parse(), 0, uint32_ti)
        assert isinstance(result, hm_node_leaf)
        assert result.value == 42

    def test_fork(self):
        uint32_ti = UintTypeConstructor(32)
        hmn_ti = HmNodeType[int].instantiate(0, uint32_ti)
        left_leaf = hm_node_leaf[int](uint32_ti, value=10)
        right_leaf = hm_node_leaf[int](uint32_ti, value=20)
        obj = hm_node_fork[int](
            0,
            uint32_ti,
            left=Ref(hmn_ti, left_leaf),
            right=Ref(hmn_ti, right_leaf),
        )
        result = HmNodeType[int]().load_from(obj.serialize().begin_parse(), 1, uint32_ti)
        assert isinstance(result, hm_node_fork)
        left_val = result.left.ref
        right_val = result.right.ref
        assert isinstance(left_val, hm_node_leaf)
        assert isinstance(right_val, hm_node_leaf)
        assert left_val.value == 10
        assert right_val.value == 20


class TestMcInferred:
    """McInferred: output param inferred through multi-constructor McPair."""

    def test_mc_left_roundtrip(self):
        uint32_ti = UintTypeConstructor(32)
        unary_2 = unary_succ(1, x=unary_succ(0, x=unary_zero()))
        inner = mc_left(UnaryType(), uint32_ti, first=unary_2, other=99)
        obj = mc_inferred(2, x=inner, y=3)
        result = mc_inferred.load_from(obj.serialize().begin_parse())
        assert isinstance(result, mc_inferred)
        assert result.n == 2
        assert result.y == 3

    def test_mc_right_roundtrip(self):
        uint32_ti = UintTypeConstructor(32)
        unary_1 = unary_succ(0, x=unary_zero())
        inner = mc_right(UnaryType(), uint32_ti, val=unary_1, extra=42)
        obj = mc_inferred(1, x=inner, y=1)
        result = mc_inferred.load_from(obj.serialize().begin_parse())
        assert isinstance(result, mc_inferred)
        assert result.n == 1
        assert result.y == 1


class TestNegativeNatErrors:
    """Negative nat parameters should raise TlbModelError."""

    def test_hm_node_fork_negative_n(self):
        """Calling hm_node_fork.load_from with type_arg_0=0 → n = -1 → TlbModelError."""
        uint32_ti = UintTypeConstructor(32)
        b = Builder()
        _ = b.store_ref(Builder().store_uint(42, 32).end_cell())
        _ = b.store_ref(Builder().store_uint(43, 32).end_cell())
        with pytest.raises(TlbModelError, match="negative"):
            _ = hm_node_fork[int].load_from(b.end_cell().begin_parse(), 0, uint32_ti)
