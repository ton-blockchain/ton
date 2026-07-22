"""Tests for generated Python code from TL-B cellref schema."""

from generated.cellref import (
    bool_false as cr_bool_false,
)
from generated.cellref import (
    bool_true as cr_bool_true,
)
from generated.cellref import (
    double_ref,
    nested_ref,
    opaque_ref,
    ref_to_record,
    simple_ref,
)
from generated.cellref import (
    tick_tock as cr_tick_tock,
)
from tlb.object import Ref, UintTypeConstructor

_uint8 = UintTypeConstructor(8)
_uint32 = UintTypeConstructor(32)
_uint64 = UintTypeConstructor(64)
_ref_uint8 = Ref[int].instantiate(_uint8)

# ── Cell references ───────────────────────────────────────────────────


class TestCellRef:
    def test_simple_ref_roundtrip(self):
        """^uint32: value stored in a referenced cell, accessed via .ref."""
        obj = simple_ref(x=Ref(_uint32, 42))
        result = simple_ref.load_from(obj.serialize().begin_parse())
        assert result.x.ref == 42

    def test_simple_ref_lazy(self):
        """Ref wrapper is lazy — doesn't parse until .ref is accessed."""
        obj = simple_ref(x=Ref(_uint32, 999))
        result = simple_ref.load_from(obj.serialize().begin_parse())
        # Cell is stored, not yet parsed
        assert result.x._value_cell is not None  # pyright: ignore[reportPrivateUsage]
        val = result.x.ref
        assert val == 999
        # After access, cell is released
        assert result.x._value_cell is None  # pyright: ignore[reportPrivateUsage]

    def test_simple_ref_structure(self):
        """simple_ref serializes as 0 data bits + 1 ref."""
        cell = simple_ref(x=Ref(_uint32, 999)).serialize()
        cs = cell.begin_parse()
        assert cs.remaining_bits == 0
        assert cs.remaining_refs == 1

    def test_nested_ref_roundtrip(self):
        """Mix of inline and ref fields."""
        obj = nested_ref(a=10, b=Ref(_uint64, 20), c=-5)
        result = nested_ref.load_from(obj.serialize().begin_parse())
        assert result.a == 10
        assert result.b.ref == 20
        assert result.c == -5

    def test_nested_ref_structure(self):
        """a:uint32 (32 bits) + b:^uint64 (1 ref) + c:int8 (8 bits) = 40 bits + 1 ref."""
        cell = nested_ref(a=0, b=Ref(_uint64, 0), c=0).serialize()
        cs = cell.begin_parse()
        assert cs.remaining_bits == 40  # 32 + 8
        assert cs.remaining_refs == 1

    def test_ref_to_user_type(self):
        """^TickTock: user-defined type in a referenced cell."""
        tt = cr_tick_tock(tick=cr_bool_true(), tock=cr_bool_false())
        obj = ref_to_record(inner=Ref(cr_tick_tock, tt))
        result = ref_to_record.load_from(obj.serialize().begin_parse())
        inner = result.inner.ref
        assert isinstance(inner, cr_tick_tock)
        assert isinstance(inner.tick, cr_bool_true)
        assert isinstance(inner.tock, cr_bool_false)

    def test_ref_to_user_type_structure(self):
        """ref_to_record: 0 data bits + 1 ref (containing 2 bits for TickTock)."""
        tt = cr_tick_tock(tick=cr_bool_false(), tock=cr_bool_false())
        cell = ref_to_record(inner=Ref(cr_tick_tock, tt)).serialize()
        cs = cell.begin_parse()
        assert cs.remaining_bits == 0
        assert cs.remaining_refs == 1
        ref_cs = cs.load_ref().begin_parse()
        assert ref_cs.remaining_bits == 2

    def test_double_ref_roundtrip(self):
        """^^uint8: two levels of cell references, access via .ref.ref."""
        inner = Ref(_uint8, 42)
        obj = double_ref(x=Ref(_ref_uint8, inner))
        result = double_ref.load_from(obj.serialize().begin_parse())
        assert result.x.ref.ref == 42

    def test_double_ref_structure(self):
        """^^uint8: outer cell has 0 bits + 1 ref, inner ref has 0 bits + 1 ref, innermost has 8 bits."""
        inner = Ref(_uint8, 255)
        cell = double_ref(x=Ref(_ref_uint8, inner)).serialize()
        cs = cell.begin_parse()
        assert cs.remaining_bits == 0
        assert cs.remaining_refs == 1
        inner1 = cs.load_ref().begin_parse()
        assert inner1.remaining_bits == 0
        assert inner1.remaining_refs == 1
        inner2 = inner1.load_ref().begin_parse()
        assert inner2.remaining_bits == 8
        assert inner2.load_uint(8) == 255

    def test_wrapper_from_cell(self):
        """Ref constructed from a raw Cell (e.g. pruned branch)."""
        from pytoniq_core import Builder

        # Serialize a uint32 into a cell manually
        b = Builder()
        _ = b.store_uint(12345, 32)
        cell = b.end_cell()
        # Create wrapper from cell, not from value
        wrapper: Ref[int] = Ref(_uint32, cell)
        assert wrapper.ref == 12345

    def test_wrapper_serialize_from_cell(self):
        """Ref constructed from cell can serialize back."""
        from pytoniq_core import Builder

        b = Builder()
        _ = b.store_uint(777, 32)
        cell = b.end_cell()
        obj = simple_ref(x=Ref(_uint32, cell))
        result = simple_ref.load_from(obj.serialize().begin_parse())
        assert result.x.ref == 777

    def test_opaque_cell_ref_roundtrip(self):
        """^Cell: opaque cell reference stored and loaded without parsing."""
        from pytoniq_core import Builder

        b = Builder()
        _ = b.store_uint(0xDEAD, 16)
        _ = b.store_uint(0xBEEF, 16)
        child_cell = b.end_cell()

        obj = opaque_ref(data=42, child=child_cell)
        result = opaque_ref.load_from(obj.serialize().begin_parse())
        assert result.data == 42
        # child is a raw Cell, not parsed
        cs = result.child.begin_parse()
        assert cs.load_uint(16) == 0xDEAD
        assert cs.load_uint(16) == 0xBEEF

    def test_opaque_cell_ref_special_cell(self):
        """^Cell accepts special cells without error (opaque, never parsed)."""
        from bitarray import bitarray
        from pytoniq_core import Builder, Cell
        from pytoniq_core.boc.exotic import CellTypes

        pb = Builder()
        _ = pb.store_uint(1, 8)  # pruned branch tag
        _ = pb.store_uint(1, 8)  # level mask
        _ = pb.store_bits(bitarray("0" * 256))
        _ = pb.store_uint(0, 16)
        raw = pb.end_cell()
        cs = raw.begin_parse()
        special_cell = Cell(
            cs.load_bits(cs.remaining_bits),
            [],
            cell_type=CellTypes.pruned_branch,
        )

        obj = opaque_ref(data=1, child=special_cell)
        result = opaque_ref.load_from(obj.serialize().begin_parse())
        assert result.data == 1
        assert result.child.begin_parse().is_special()

    def test_generic_ref_set_cell(self):
        """Ref[X].set_cell replaces the value with a raw cell."""
        from pytoniq_core import Builder

        ref: Ref[int] = Ref(_uint32, 42)
        assert ref.ref == 42

        # Replace with a different cell via set_cell
        b = Builder()
        _ = b.store_uint(999, 32)
        ref.set_cell(b.end_cell())
        assert ref.ref == 999

    def test_cell_from_value(self):
        """Ref[X].cell serializes the value into a Cell."""
        ref: Ref[int] = Ref(_uint32, 12345)
        cell = ref.cell
        assert cell.begin_parse().load_uint(32) == 12345

    def test_cell_from_raw_cell(self):
        """Ref[X].cell returns the raw cell when constructed from a Cell."""
        from pytoniq_core import Builder

        b = Builder()
        _ = b.store_uint(777, 32)
        original = b.end_cell()
        ref: Ref[int] = Ref(_uint32, original)
        assert ref.cell is original

    def test_cell_does_not_deserialize(self):
        """Ref[X].cell does not trigger deserialization of a raw cell."""
        from pytoniq_core import Builder

        b = Builder()
        _ = b.store_uint(42, 32)
        ref: Ref[int] = Ref(_uint32, b.end_cell())
        _ = ref.cell
        assert ref._value_cell is not None  # pyright: ignore[reportPrivateUsage]
        assert ref._value is None  # pyright: ignore[reportPrivateUsage]
