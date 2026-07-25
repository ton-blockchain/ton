"""Tests for generated code with special (exotic) cell types."""

import pytest
from bitarray import bitarray
from generated.special_cells import (
    EitherProofUpdateType,
    either_proof,
    merkle_proof,
    merkle_update,
)
from pytoniq_core import Builder, Cell
from pytoniq_core.boc.exotic import CellTypes
from tlb.object import TlbModelError, UintTypeConstructor


def _as_special(cell: Cell, cell_type: int) -> Cell:
    """Re-create a cell as a special cell with the given type."""
    cs = cell.begin_parse()
    data = cs.load_bits(cs.remaining_bits)
    refs = [cs.load_ref() for _ in range(cs.remaining_refs)]
    return Cell(data, refs, cell_type=cell_type)


def _make_pruned_branch(original: Cell) -> Cell:
    """Create a pruned branch cell that replaces original."""
    b = Builder()
    _ = b.store_uint(1, 8)  # pruned branch type tag
    _ = b.store_uint(1, 8)  # level mask
    _ = b.store_bits(_hash_bits(original))
    _ = b.store_uint(original.get_depth(0), 16)
    return _as_special(b.end_cell(), CellTypes.pruned_branch)


def _hash_bits(cell: Cell) -> bitarray:
    """Get a cell's hash as a 256-bit bitarray."""
    result = bitarray()
    result.frombytes(cell.get_hash(0))  # pyright: ignore[reportUnknownMemberType]
    return result


class TestMerkleProof:
    def test_deserialize_rejects_ordinary_cell(self):
        """merkle_proof deserialize should reject non-special cells."""
        b = Builder()
        _ = b.store_uint(0x03, 8)
        with pytest.raises(TlbModelError, match="special"):
            _ = merkle_proof[int].deserialize(b.end_cell(), UintTypeConstructor(32))

    def test_roundtrip(self):
        """Create a merkle proof special cell and deserialize it."""
        uint32_ti = UintTypeConstructor(32)

        inner_b = Builder()
        _ = inner_b.store_uint(42, 32)
        inner_cell = inner_b.end_cell()

        b = Builder()
        _ = b.store_uint(3, 8)
        _ = b.store_bits(_hash_bits(inner_cell))
        _ = b.store_uint(inner_cell.get_depth(0), 16)
        _ = b.store_ref(inner_cell)
        proof_cell = _as_special(b.end_cell(), CellTypes.merkle_proof)

        result = merkle_proof[int].deserialize(proof_cell, uint32_ti)
        assert isinstance(result, merkle_proof)
        assert result.depth == 0
        assert result.virtual_hash == _hash_bits(inner_cell)
        assert result.virtual_root.ref == 42

    def test_pruned_branch_child(self):
        """Merkle proof with a pruned branch as virtual_root — parses without accessing .ref."""
        uint32_ti = UintTypeConstructor(32)

        inner_b = Builder()
        _ = inner_b.store_uint(99, 32)
        inner_cell = inner_b.end_cell()
        pruned = _make_pruned_branch(inner_cell)

        b = Builder()
        _ = b.store_uint(3, 8)
        _ = b.store_bits(_hash_bits(inner_cell))
        _ = b.store_uint(inner_cell.get_depth(0), 16)
        _ = b.store_ref(pruned)
        proof_cell = _as_special(b.end_cell(), CellTypes.merkle_proof)

        result = merkle_proof[int].deserialize(proof_cell, uint32_ti)
        assert isinstance(result, merkle_proof)
        assert result.virtual_hash == _hash_bits(inner_cell)
        assert result.depth == 0
        with pytest.raises(TlbModelError, match="special"):
            _ = result.virtual_root.ref


class TestMerkleUpdate:
    def test_deserialize_rejects_ordinary_cell(self):
        """merkle_update deserialize should reject non-special cells."""
        b = Builder()
        _ = b.store_uint(0x04, 8)
        with pytest.raises(TlbModelError, match="special"):
            _ = merkle_update[int].deserialize(b.end_cell(), UintTypeConstructor(32))

    def test_roundtrip(self):
        """Create a merkle update special cell and deserialize it."""
        uint32_ti = UintTypeConstructor(32)

        old_b = Builder()
        _ = old_b.store_uint(10, 32)
        old_cell = old_b.end_cell()

        new_b = Builder()
        _ = new_b.store_uint(20, 32)
        new_cell = new_b.end_cell()

        b = Builder()
        _ = b.store_uint(4, 8)
        _ = b.store_bits(_hash_bits(old_cell))
        _ = b.store_bits(_hash_bits(new_cell))
        _ = b.store_uint(old_cell.get_depth(0), 16)
        _ = b.store_uint(new_cell.get_depth(0), 16)
        _ = b.store_ref(old_cell)
        _ = b.store_ref(new_cell)
        update_cell = _as_special(b.end_cell(), CellTypes.merkle_update)

        result = merkle_update[int].deserialize(update_cell, uint32_ti)
        assert isinstance(result, merkle_update)
        assert result.old.ref == 10
        assert result.new.ref == 20
        assert result.old_hash == _hash_bits(old_cell)
        assert result.new_hash == _hash_bits(new_cell)


class TestMultiConstructorSpecial:
    """EitherProofUpdate — multi-constructor special type with TypeInfo deserialize."""

    def test_deserialize_rejects_ordinary_cell(self):
        b = Builder()
        _ = b.store_uint(0x03, 8)
        ti = EitherProofUpdateType[int]().instantiate(UintTypeConstructor(32))
        with pytest.raises(TlbModelError, match="special"):
            _ = ti.deserialize(b.end_cell())

    def test_roundtrip_proof_variant(self):
        uint32_ti = UintTypeConstructor(32)
        inner_b = Builder()
        _ = inner_b.store_uint(42, 32)
        inner_cell = inner_b.end_cell()

        b = Builder()
        _ = b.store_uint(3, 8)
        _ = b.store_bits(_hash_bits(inner_cell))
        _ = b.store_uint(inner_cell.get_depth(0), 16)
        _ = b.store_ref(inner_cell)
        proof_cell = _as_special(b.end_cell(), CellTypes.merkle_proof)

        ti = EitherProofUpdateType[int]().instantiate(uint32_ti)
        result = ti.deserialize(proof_cell)
        assert isinstance(result, either_proof)
        assert result.virtual_root.ref == 42
