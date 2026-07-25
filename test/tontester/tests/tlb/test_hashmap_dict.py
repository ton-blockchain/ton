"""Tests for HashmapDict — lazy dictionary backed by HashmapE cells."""

import pytest
from pytoniq_core import Builder, HashMap
from tlb.hashmap import HashmapDict
from tlb.object import UintTypeConstructor


def _build_hme(key_bits: int, entries: dict[int, int], value_bits: int = 32) -> Builder:
    """Build a HashmapE cell from int→int entries."""
    b = Builder()
    if not entries:
        _ = b.store_uint(0, 1)
    else:
        hm = HashMap(key_bits)
        for k, v in entries.items():
            _ = hm.set_int_key(k, Builder().store_uint(v, value_bits).end_cell().begin_parse())  # pyright: ignore[reportUnknownMemberType]
        cell = hm.serialize()
        assert cell is not None
        _ = b.store_uint(1, 1)
        _ = b.store_ref(cell)
    return b


class TestHashmapDict:
    def test_empty(self):
        cs = _build_hme(8, {}).end_cell().begin_parse()
        d = HashmapDict[int].load_from(cs, 8, UintTypeConstructor(32))
        assert d.is_empty()
        assert len(d) == 0
        assert list(d.items()) == []

    def test_single_entry(self):
        cs = _build_hme(8, {42: 100}).end_cell().begin_parse()
        d = HashmapDict[int].load_from(cs, 8, UintTypeConstructor(32))
        assert not d.is_empty()
        assert len(d) == 1
        assert d[42] == 100
        assert 42 in d
        assert 0 not in d

    def test_multiple_entries(self):
        entries = {1: 100, 5: 500, 255: 999}
        cs = _build_hme(8, entries).end_cell().begin_parse()
        d = HashmapDict[int].load_from(cs, 8, UintTypeConstructor(32))
        assert len(d) == 3
        for k, v in entries.items():
            assert d[k] == v

    def test_iteration_order(self):
        """Keys are iterated in ascending order (binary tree left-to-right)."""
        entries = {255: 3, 0: 1, 128: 2}
        cs = _build_hme(8, entries).end_cell().begin_parse()
        d = HashmapDict[int].load_from(cs, 8, UintTypeConstructor(32))
        keys = list(d.keys())
        assert keys == sorted(keys)

    def test_key_not_found(self):
        cs = _build_hme(8, {1: 10}).end_cell().begin_parse()
        d = HashmapDict[int].load_from(cs, 8, UintTypeConstructor(32))
        with pytest.raises(KeyError):
            _ = d[99]

    def test_get_with_default(self):
        cs = _build_hme(8, {1: 10}).end_cell().begin_parse()
        d = HashmapDict[int].load_from(cs, 8, UintTypeConstructor(32))
        assert d.get(1) == 10
        assert d.get(99) is None
        assert d.get(99, -1) == -1

    def test_to_dict(self):
        entries = {10: 100, 20: 200}
        cs = _build_hme(8, entries).end_cell().begin_parse()
        d = HashmapDict[int].load_from(cs, 8, UintTypeConstructor(32))
        assert d.to_dict() == entries

    def test_serialize_roundtrip(self):
        entries = {1: 100, 5: 500, 255: 999}
        cs = _build_hme(8, entries).end_cell().begin_parse()
        d = HashmapDict[int].load_from(cs, 8, UintTypeConstructor(32))

        b = Builder()
        d.serialize_to(b)
        cs2 = b.end_cell().begin_parse()
        d2 = HashmapDict[int].load_from(cs2, 8, UintTypeConstructor(32))
        assert d2.to_dict() == entries

    def test_serialize_empty(self):
        cs = _build_hme(8, {}).end_cell().begin_parse()
        d = HashmapDict[int].load_from(cs, 8, UintTypeConstructor(32))
        b = Builder()
        d.serialize_to(b)
        cs2 = b.end_cell().begin_parse()
        assert cs2.load_uint(1) == 0  # hme_empty tag

    def test_setitem_new_key(self):
        cs = _build_hme(8, {1: 100}).end_cell().begin_parse()
        d = HashmapDict[int].load_from(cs, 8, UintTypeConstructor(32))
        d[2] = 200
        assert d[2] == 200
        assert d[1] == 100
        assert len(d) == 2

    def test_setitem_override(self):
        cs = _build_hme(8, {1: 100}).end_cell().begin_parse()
        d = HashmapDict[int].load_from(cs, 8, UintTypeConstructor(32))
        d[1] = 999
        assert d[1] == 999
        assert len(d) == 1

    def test_delitem(self):
        cs = _build_hme(8, {1: 100, 2: 200}).end_cell().begin_parse()
        d = HashmapDict[int].load_from(cs, 8, UintTypeConstructor(32))
        del d[1]
        assert 1 not in d
        assert d[2] == 200
        assert len(d) == 1

    def test_delitem_missing(self):
        cs = _build_hme(8, {1: 100}).end_cell().begin_parse()
        d = HashmapDict[int].load_from(cs, 8, UintTypeConstructor(32))
        with pytest.raises(KeyError):
            del d[99]

    def test_mutation_serialize_roundtrip(self):
        """Mutations are properly serialized into a new hashmap cell."""
        cs = _build_hme(8, {1: 100, 5: 500}).end_cell().begin_parse()
        d = HashmapDict[int].load_from(cs, 8, UintTypeConstructor(32))
        d[1] = 111
        d[10] = 1000
        del d[5]

        b = Builder()
        d.serialize_to(b)
        cs2 = b.end_cell().begin_parse()
        d2 = HashmapDict[int].load_from(cs2, 8, UintTypeConstructor(32))
        assert d2.to_dict() == {1: 111, 10: 1000}

    def test_delete_all_is_empty(self):
        cs = _build_hme(8, {1: 100, 2: 200}).end_cell().begin_parse()
        d = HashmapDict[int].load_from(cs, 8, UintTypeConstructor(32))
        assert not d.is_empty()
        del d[1]
        assert not d.is_empty()
        del d[2]
        assert d.is_empty()
        assert len(d) == 0

    def test_insert_into_empty(self):
        cs = _build_hme(8, {}).end_cell().begin_parse()
        d = HashmapDict[int].load_from(cs, 8, UintTypeConstructor(32))
        d[42] = 999
        assert d[42] == 999
        b = Builder()
        d.serialize_to(b)
        cs2 = b.end_cell().begin_parse()
        d2 = HashmapDict[int].load_from(cs2, 8, UintTypeConstructor(32))
        assert d2[42] == 999

    def test_pruned_branch_child(self):
        """Dict with a pruned branch replacing one subtree — accessible keys still work."""
        from bitarray import bitarray
        from pytoniq_core import Cell
        from pytoniq_core.boc.exotic import CellTypes

        # Build a dict with keys 0 and 128 (split at bit 0: left=0, right=128)
        entries = {0: 100, 128: 200}
        hme_cell = _build_hme(8, entries).end_cell()
        d_original = HashmapDict[int].load_from(hme_cell.begin_parse(), 8, UintTypeConstructor(32))
        assert d_original[0] == 100
        assert d_original[128] == 200

        # Now replace the right subtree cell (key 128) with a pruned branch
        # Get the root hashmap cell
        cs = hme_cell.begin_parse()
        assert cs.load_bit() == 1  # hme_root tag
        root_cell = cs.load_ref()

        # The root is hm_edge with a fork node that has left/right refs
        root_cs = root_cell.begin_parse()
        # Skip label (short label $0, unary 0 = just $0 bit)
        assert root_cs.load_bit() == 0  # hml_short tag
        assert root_cs.load_bit() == 0  # unary zero (length 0)
        # Now at the fork node: two refs
        left_ref = root_cs.load_ref()
        right_ref = root_cs.load_ref()

        # Create a pruned branch replacing the right subtree
        pb_builder = Builder()
        _ = pb_builder.store_uint(1, 8)  # pruned branch type tag
        _ = pb_builder.store_uint(1, 8)  # level mask
        hash_bits = bitarray()
        hash_bits.frombytes(right_ref.get_hash(0))  # pyright: ignore[reportUnknownMemberType]
        _ = pb_builder.store_bits(hash_bits)
        _ = pb_builder.store_uint(right_ref.get_depth(0), 16)
        pb_cs = pb_builder.end_cell().begin_parse()
        pruned_right = Cell(
            pb_cs.load_bits(pb_cs.remaining_bits), [], cell_type=CellTypes.pruned_branch
        )

        # Rebuild root with pruned right
        new_root_builder = Builder()
        _ = new_root_builder.store_uint(0, 1)  # hml_short
        _ = new_root_builder.store_uint(0, 1)  # unary zero
        _ = new_root_builder.store_ref(left_ref)
        _ = new_root_builder.store_ref(pruned_right)
        new_root_cell = new_root_builder.end_cell()

        # Rebuild hme_root
        hme_builder = Builder()
        _ = hme_builder.store_uint(1, 1)
        _ = hme_builder.store_ref(new_root_cell)

        d = HashmapDict[int].load_from(
            hme_builder.end_cell().begin_parse(), 8, UintTypeConstructor(32)
        )

        # Left subtree (key 0) should still be accessible
        assert d[0] == 100

        # Right subtree (key 128) is pruned — accessing it should fail
        with pytest.raises(Exception):
            _ = d[128]

        # Iteration should yield the left entry, then fail on right
        items: list[tuple[int, int]] = []
        with pytest.raises(Exception):
            for item in d.items():
                items.append(item)
        assert items == [(0, 100)]

    def _build_dict_with_pruned_right(self):
        """Helper: build a HashmapDict whose right subtree has been replaced
        by a pruned branch. Returns (dict, original_pruned_cell)."""
        from bitarray import bitarray
        from pytoniq_core import Cell
        from pytoniq_core.boc.exotic import CellTypes

        hme_cell = _build_hme(8, {0: 100, 128: 200}).end_cell()
        cs = hme_cell.begin_parse()
        assert cs.load_bit() == 1
        root_cell = cs.load_ref()
        root_cs = root_cell.begin_parse()
        assert root_cs.load_bit() == 0  # hml_short
        assert root_cs.load_bit() == 0  # unary zero
        left_ref = root_cs.load_ref()
        right_ref = root_cs.load_ref()

        pb_builder = Builder()
        _ = pb_builder.store_uint(1, 8)
        _ = pb_builder.store_uint(1, 8)
        hash_bits = bitarray()
        hash_bits.frombytes(right_ref.get_hash(0))  # pyright: ignore[reportUnknownMemberType]
        _ = pb_builder.store_bits(hash_bits)
        _ = pb_builder.store_uint(right_ref.get_depth(0), 16)
        pb_cs = pb_builder.end_cell().begin_parse()
        pruned_right = Cell(
            pb_cs.load_bits(pb_cs.remaining_bits), [], cell_type=CellTypes.pruned_branch
        )

        new_root_builder = Builder()
        _ = new_root_builder.store_uint(0, 1)
        _ = new_root_builder.store_uint(0, 1)
        _ = new_root_builder.store_ref(left_ref)
        _ = new_root_builder.store_ref(pruned_right)
        new_root_cell = new_root_builder.end_cell()

        hme_builder = Builder()
        _ = hme_builder.store_uint(1, 1)
        _ = hme_builder.store_ref(new_root_cell)
        d = HashmapDict[int].load_from(
            hme_builder.end_cell().begin_parse(), 8, UintTypeConstructor(32)
        )
        return d, pruned_right

    def test_serialize_after_adding_to_unpruned_side(self):
        """Adding a key on the unpruned side must serialize without forcing
        the pruned subtree to be loaded."""
        d, pruned_right = self._build_dict_with_pruned_right()
        # Add a new key on the LEFT side (key 1, prefix 00000001).
        d[1] = 999
        # Serializing must succeed without touching the pruned right subtree.
        b = Builder()
        d.serialize_to(b)
        new_cell = b.end_cell()

        # The pruned right subtree must be preserved verbatim in the output.
        new_cs = new_cell.begin_parse()
        assert new_cs.load_bit() == 1
        new_root_cell = new_cs.load_ref()
        new_root_cs = new_root_cell.begin_parse()
        # Skip the label (still empty: hml_short + unary zero)
        assert new_root_cs.load_bit() == 0
        assert new_root_cs.load_bit() == 0
        _new_left = new_root_cs.load_ref()
        new_right = new_root_cs.load_ref()
        assert new_right.get_hash(0) == pruned_right.get_hash(0)  # pyright: ignore[reportUnknownMemberType]

        # And re-loading the result lets us read back the new + original left keys.
        d2 = HashmapDict[int].load_from(new_cell.begin_parse(), 8, UintTypeConstructor(32))
        assert d2[0] == 100
        assert d2[1] == 999
        # Right side still pruned — accessing 128 still raises.
        with pytest.raises(Exception):
            _ = d2[128]

    def test_large_keys(self):
        """256-bit keys (like account addresses)."""
        entries = {0: 1, 2**256 - 1: 2, 2**128: 3}
        cs = _build_hme(256, entries).end_cell().begin_parse()
        d = HashmapDict[int].load_from(cs, 256, UintTypeConstructor(32))
        assert len(d) == 3
        for k, v in entries.items():
            assert d[k] == v


class _SumAug:
    """Sum augmentation: extra at each node = sum of leaf values in subtree."""

    @property
    def extra_ti(self):
        return UintTypeConstructor(32)

    def eval_leaf(self, value: int) -> int:
        return value

    def merge(self, left: int, right: int) -> int:
        return left + right

    def eval_empty(self) -> int:
        return 0


_sum_aug = _SumAug()
_uint32 = UintTypeConstructor(32)


class TestAugmentedHashmapDict:
    """Tests for HashmapDict with augmentation (HashmapAugE)."""

    def test_serialize_empty(self):
        """Empty augmented dict serializes as $0 + extra."""
        d = HashmapDict(8, _uint32, _uint32, _sum_aug)
        b = Builder()
        d.serialize_to(b)
        cs = b.end_cell().begin_parse()
        assert cs.load_uint(1) == 0  # ahme_empty tag
        assert cs.load_uint(32) == 0  # eval_empty() extra

    def test_serialize_single(self):
        """Single entry: root extra = eval_leaf(value)."""
        d = HashmapDict(8, _uint32, _uint32, _sum_aug)
        d[42] = 100
        b = Builder()
        d.serialize_to(b)
        cs = b.end_cell().begin_parse()
        assert cs.load_uint(1) == 1  # ahme_root tag
        _ = cs.load_ref()  # skip root cell
        assert cs.load_uint(32) == 100  # root extra = eval_leaf(100)

    def test_serialize_multiple_merges(self):
        """Multiple entries: root extra = sum of all values."""
        d = HashmapDict(8, _uint32, _uint32, _sum_aug)
        d[0] = 10
        d[128] = 20
        d[255] = 30
        b = Builder()
        d.serialize_to(b)
        cs = b.end_cell().begin_parse()
        assert cs.load_uint(1) == 1  # ahme_root tag
        _ = cs.load_ref()
        assert cs.load_uint(32) == 60  # 10 + 20 + 30

    def test_roundtrip(self):
        """Serialize then deserialize augmented dict, values preserved."""
        d = HashmapDict(8, _uint32, _uint32, _sum_aug)
        d[1] = 100
        d[5] = 500
        d[255] = 999

        b = Builder()
        d.serialize_to(b)
        cs = b.end_cell().begin_parse()
        d2 = HashmapDict[int, int].load_from(cs, 8, _uint32, _uint32, aug=_sum_aug)
        assert d2.to_dict() == {1: 100, 5: 500, 255: 999}

    def test_roundtrip_reserialize(self):
        """Loaded augmented dict re-serializes with correct extras."""
        d = HashmapDict(8, _uint32, _uint32, _sum_aug)
        d[0] = 10
        d[128] = 20

        b = Builder()
        d.serialize_to(b)
        cs = b.end_cell().begin_parse()
        d2 = HashmapDict[int, int].load_from(cs, 8, _uint32, _uint32, aug=_sum_aug)
        d2[64] = 30

        b2 = Builder()
        d2.serialize_to(b2)
        cs2 = b2.end_cell().begin_parse()
        assert cs2.load_uint(1) == 1
        _ = cs2.load_ref()
        assert cs2.load_uint(32) == 60  # 10 + 20 + 30

    def test_getitem_after_load(self):
        """Deserialized augmented dict supports key lookup."""
        d = HashmapDict(8, _uint32, _uint32, _sum_aug)
        d[10] = 100
        d[20] = 200

        b = Builder()
        d.serialize_to(b)
        d2 = HashmapDict[int, int].load_from(
            b.end_cell().begin_parse(), 8, _uint32, _uint32, aug=_sum_aug
        )
        assert d2[10] == 100
        assert d2[20] == 200
        assert 10 in d2
        assert 99 not in d2

    def test_leaf_extras_in_tree(self):
        """Leaf nodes inside the tree contain correct extras."""
        d = HashmapDict(8, _uint32, _uint32, _sum_aug)
        d[0] = 42

        b = Builder()
        d.serialize_to(b)
        cs = b.end_cell().begin_parse()
        assert cs.load_uint(1) == 1  # ahme_root tag
        root = cs.load_ref().begin_parse()
        # Root is ahm_edge: label then ahmn_leaf(extra, value)
        # Key 0 = 00000000 (8 bits all zero) → hml_same $11, v=0, n=8
        assert root.load_uint(2) == 3  # hml_same tag
        assert root.load_uint(1) == 0  # v = False
        assert root.load_uint(4) == 8  # n = 8 (#<= 8)
        # ahmn_leaf: extra then value
        assert root.load_uint(32) == 42  # extra = eval_leaf(42)
        assert root.load_uint(32) == 42  # value

    def test_fork_extras_in_tree(self):
        """Fork nodes inside the tree contain merged extras."""
        d = HashmapDict(8, _uint32, _uint32, _sum_aug)
        d[0] = 10  # left subtree (bit 0 = 0)
        d[128] = 20  # right subtree (bit 0 = 1)

        b = Builder()
        d.serialize_to(b)
        cs = b.end_cell().begin_parse()
        assert cs.load_uint(1) == 1  # ahme_root tag
        _ = cs.load_uint(32)  # root-level extra (from ahme_root)

        # Root extra should be in ahme_root envelope, but that's after the ref.
        # Actually, ahme_root is: $1 ^root extra — let me re-read
        # We already read the tag and skipped the ref... wait, let me re-do
        cs2 = b.end_cell().begin_parse()
        assert cs2.load_uint(1) == 1
        root_cell = cs2.load_ref()
        assert cs2.load_uint(32) == 30  # ahme_root extra = 10 + 20

        # Inside the root cell: ahm_edge with label + ahmn_fork
        root_cs = root_cell.begin_parse()
        assert root_cs.load_uint(1) == 0  # hml_short
        assert root_cs.load_uint(1) == 0  # unary_zero
        # ahmn_fork: left_ref, right_ref, extra
        left_cell = root_cs.load_ref()
        right_cell = root_cs.load_ref()
        fork_extra = root_cs.load_uint(32)
        assert fork_extra == 30  # merge(10, 20)

        # Left leaf: label + extra + value
        left_cs = left_cell.begin_parse()
        # label for remaining 7 bits: key 0 = 0000000
        # hml_same $11, v=0, n=7
        assert left_cs.load_uint(2) == 3  # hml_same tag
        assert left_cs.load_uint(1) == 0  # v = False
        assert left_cs.load_uint(3) == 7  # n = 7 (#<= 7)
        assert left_cs.load_uint(32) == 10  # leaf extra
        assert left_cs.load_uint(32) == 10  # leaf value

        # Right leaf: key 128 = 10000000, remaining 7 bits = 0000000
        right_cs = right_cell.begin_parse()
        assert right_cs.load_uint(2) == 3  # hml_same
        assert right_cs.load_uint(1) == 0  # v = False
        assert right_cs.load_uint(3) == 7  # n = 7
        assert right_cs.load_uint(32) == 20  # leaf extra
        assert right_cs.load_uint(32) == 20  # leaf value

    def test_non_empty_hashmap_aug(self):
        """HashmapAug (allow_empty=False) serialization."""
        d = HashmapDict(8, _uint32, _uint32, _sum_aug, allow_empty=False)
        d[1] = 100
        d[2] = 200

        b = Builder()
        d.serialize_to(b)

        # Deserialize as HashmapAug (no tag, edge directly in slice)
        d2 = HashmapDict[int, int].load_from(
            b.end_cell().begin_parse(), 8, _uint32, _uint32, allow_empty=False, aug=_sum_aug
        )
        assert d2.to_dict() == {1: 100, 2: 200}
