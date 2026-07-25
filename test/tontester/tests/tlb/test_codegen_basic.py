"""Tests for generated Python code from TL-B basic schema."""

import pytest
from generated.basic import (
    BoolType,
    MsgType,
    bool_false,
    bool_true,
    mixed,
    msg_a,
    msg_b,
    msg_c,
    tick_tock,
    unit,
)
from pytoniq_core import Builder

# ── Bool ──────────────────────────────────────────────────────────────


class TestBool:
    def test_serialize_false_is_one_zero_bit(self):
        cell = bool_false().serialize()
        cs = cell.begin_parse()
        assert cs.remaining_bits == 1
        assert cs.load_bit() == 0

    def test_serialize_true_is_one_one_bit(self):
        cell = bool_true().serialize()
        cs = cell.begin_parse()
        assert cs.remaining_bits == 1
        assert cs.load_bit() == 1

    def test_roundtrip_false(self):
        result = BoolType().load_from(bool_false().serialize().begin_parse())
        assert isinstance(result, bool_false)

    def test_roundtrip_true(self):
        result = BoolType().load_from(bool_true().serialize().begin_parse())
        assert isinstance(result, bool_true)

    def test_deserialize_from_explicit_bits(self):
        b = Builder()
        _ = b.store_bit(0)
        assert isinstance(BoolType().load_from(b.end_cell().begin_parse()), bool_false)
        b = Builder()
        _ = b.store_bit(1)
        assert isinstance(BoolType().load_from(b.end_cell().begin_parse()), bool_true)

    def test_wrong_tag_fails(self):
        from tlb.object import TlbModelError

        with pytest.raises(TlbModelError, match="tag mismatch"):
            _ = bool_false.load_from(bool_true().serialize().begin_parse())
        with pytest.raises(TlbModelError, match="tag mismatch"):
            _ = bool_true.load_from(bool_false().serialize().begin_parse())


# ── Unit ──────────────────────────────────────────────────────────────


class TestUnit:
    def test_serialize_is_zero_bits(self):
        assert unit().serialize().begin_parse().remaining_bits == 0

    def test_roundtrip(self):
        assert isinstance(unit.load_from(unit().serialize().begin_parse()), unit)


# ── TickTock ──────────────────────────────────────────────────────────


class TestTickTock:
    def test_roundtrip(self):
        obj = tick_tock(tick=bool_true(), tock=bool_false())
        result = tick_tock.load_from(obj.serialize().begin_parse())
        assert isinstance(result.tick, bool_true)
        assert isinstance(result.tock, bool_false)

    def test_explicit_bits(self):
        """tick_tock(true, false) = bits 1 0."""
        b = Builder()
        _ = b.store_bit(1)
        _ = b.store_bit(0)
        result = tick_tock.load_from(b.end_cell().begin_parse())
        assert isinstance(result.tick, bool_true)
        assert isinstance(result.tock, bool_false)

    def test_bit_count(self):
        cell = tick_tock(tick=bool_false(), tock=bool_false()).serialize()
        assert cell.begin_parse().remaining_bits == 2


# ── Mixed fields ─────────────────────────────────────────────────────


class TestMixed:
    def test_roundtrip(self):
        obj = mixed(a=100, b=-42, c=2**63, d=-100000)
        result = mixed.load_from(obj.serialize().begin_parse())
        assert result.a == 100
        assert result.b == -42
        assert result.c == 2**63
        assert result.d == -100000

    def test_bit_count(self):
        """32 + 8 + 64 + 32 = 136 bits."""
        assert mixed(a=0, b=0, c=0, d=0).serialize().begin_parse().remaining_bits == 136

    def test_empty_cell_fails(self):
        with pytest.raises(Exception):
            _ = mixed.load_from(Builder().end_cell().begin_parse())


# ── Three-way dispatch ────────────────────────────────────────────────


class TestMsg:
    def test_roundtrip_a(self):
        result = MsgType().load_from(msg_a(x=42).serialize().begin_parse())
        assert isinstance(result, msg_a)
        assert result.x == 42

    def test_roundtrip_b(self):
        result = MsgType().load_from(msg_b(y=-1).serialize().begin_parse())
        assert isinstance(result, msg_b)
        assert result.y == -1

    def test_roundtrip_c(self):
        result = MsgType().load_from(msg_c(z=999).serialize().begin_parse())
        assert isinstance(result, msg_c)
        assert result.z == 999

    def test_tag_bits(self):
        cs = msg_a(x=0).serialize().begin_parse()
        assert (cs.load_bit(), cs.load_bit()) == (0, 0)
        cs = msg_b(y=0).serialize().begin_parse()
        assert (cs.load_bit(), cs.load_bit()) == (0, 1)
        cs = msg_c(z=0).serialize().begin_parse()
        assert cs.load_bit() == 1
