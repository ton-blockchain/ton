"""End-to-end test: deserialize a real TON block using generated block.tlb code."""

from pathlib import Path

import pytest
from generated.block import block, block_info, bool_false, bool_true
from pytoniq_core import Cell

DATA_DIR = Path(__file__).parent / "data"


@pytest.fixture()
def testnet_block() -> block:
    boc_path = DATA_DIR / "testnet_mc_49375158_block.boc"
    with open(boc_path, "rb") as f:
        boc_data = f.read()
    cell = Cell.one_from_boc(boc_data)
    result = block.deserialize(cell)
    assert isinstance(result, block)
    return result


class TestBlockDeserialization:
    def test_deserialize(self, testnet_block: block) -> None:
        assert testnet_block is not None

    def test_global_id(self, testnet_block: block) -> None:
        """Testnet has global_id = -3."""
        assert testnet_block.global_id == -3

    def test_block_info_basic(self, testnet_block: block) -> None:
        info = testnet_block.info.ref
        assert isinstance(info, block_info)
        assert info.version == 0
        assert info.not_master == 0
        assert info.seq_no == 49375158
        assert info.after_merge == 0
        assert info.before_split == 0
        assert info.after_split == 0
        assert isinstance(info.want_split, bool_false)
        assert isinstance(info.want_merge, bool_true)
        assert isinstance(info.key_block, bool_false)
        assert info.vert_seqno_incr == 0
        assert info.flags == 1
        assert info.vert_seq_no == 0

    def test_block_info_shard(self, testnet_block: block) -> None:
        info = testnet_block.info.ref
        assert isinstance(info, block_info)
        shard = info.shard
        assert shard.shard_pfx_bits == 0
        assert shard.workchain_id == -1
        assert shard.shard_prefix == 0

    def test_block_info_times(self, testnet_block: block) -> None:
        info = testnet_block.info.ref
        assert isinstance(info, block_info)
        assert info.gen_utime == 1775074403
        assert info.start_lt == 60003527000000
        assert info.end_lt == 60003527000004

    def test_block_info_seqnos(self, testnet_block: block) -> None:
        info = testnet_block.info.ref
        assert isinstance(info, block_info)
        assert info.gen_catchain_seqno == 516198
        assert info.min_ref_mc_seqno == 49375155
        assert info.prev_key_block_seqno == 49375146

    def test_block_info_gen_software(self, testnet_block: block) -> None:
        from generated.block import capabilities

        info = testnet_block.info.ref
        assert isinstance(info, block_info)
        sw = info.gen_software
        assert sw is not None  # flags & 1 means gen_software is present
        assert isinstance(sw, capabilities)
        assert sw.version == 13
        assert sw.capabilities == 1006

    def test_block_info_prev_ref(self, testnet_block: block) -> None:
        from generated.block import prev_blk_info

        info = testnet_block.info.ref
        assert isinstance(info, block_info)
        prev_info = info.prev_ref.ref
        assert isinstance(prev_info, prev_blk_info)
        assert prev_info.prev.seq_no == 49375157
        assert prev_info.prev.end_lt == 60003524000004

    def test_value_flow_from_prev_blk(self, testnet_block: block) -> None:
        vf = testnet_block.value_flow.ref
        anon = vf.field.ref  # ^[from_prev_blk:... to_next_blk:... ...]
        assert anon.from_prev_blk.grams.amount.value == 5076513319519884701

    def test_value_flow_fees(self, testnet_block: block) -> None:
        vf = testnet_block.value_flow.ref
        assert vf.fees_collected.grams.amount.value == 4700000000

    def test_extra_currencies_hashmap(self, testnet_block: block) -> None:
        """Walk a small Hashmap (3 leaves) in extra_currencies and verify values."""
        from generated.block import hm_edge, hme_root, hmn_fork, hmn_leaf

        vf = testnet_block.value_flow.ref
        anon = vf.field.ref
        ec = anon.from_prev_blk.other
        d = ec.dict
        assert isinstance(d, hme_root)
        root = d.root.ref
        assert isinstance(root, hm_edge)
        node = root.node
        assert isinstance(node, hmn_fork)

        # Left branch: hml_same v=0 n=23, then fork with 2 leaves
        left_edge = node.left.ref
        assert isinstance(left_edge, hm_edge)
        left_fork = left_edge.node
        assert isinstance(left_fork, hmn_fork)

        # Leaf 1: key contains 0xC9 (201), value 1400000000
        ll_edge = left_fork.left.ref
        assert isinstance(ll_edge, hm_edge)
        assert isinstance(ll_edge.node, hmn_leaf)
        assert ll_edge.node.value.value == 1400000000

        # Leaf 2: key contains 0xDF (223), value 666666666666
        lr_edge = left_fork.right.ref
        assert isinstance(lr_edge, hm_edge)
        assert isinstance(lr_edge.node, hmn_leaf)
        assert lr_edge.node.value.value == 666666666666

        # Right branch: single leaf, value 1000000000000
        right_edge = node.right.ref
        assert isinstance(right_edge, hm_edge)
        assert isinstance(right_edge.node, hmn_leaf)
        assert right_edge.node.value.value == 1000000000000
