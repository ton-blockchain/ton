/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "vm/boc.h"
#include "vm/cells/MerkleUpdate.h"

#include "block-auto.h"
#include "fabric.h"
#include "utils.h"

namespace ton::validator::consensus {

td::Result<double> get_candidate_gen_utime_exact(const BlockCandidate& candidate) {
  TRY_RESULT(cdata_roots, vm::std_boc_deserialize_multi(candidate.collated_data));
  for (const td::Ref<vm::Cell>& root : cdata_roots) {
    if (!block::gen::t_ConsensusExtraData.validate_ref(10000, root)) {
      continue;
    }
    block::gen::ConsensusExtraData::Record rec;
    CHECK(block::gen::unpack_cell(root, rec));
    return (double)rec.gen_utime_ms / 1000.0;
  }
  return td::Status::Error("no ConsensusExtraData in candidate");
}

td::Result<std::pair<td::Ref<vm::Cell>, td::Ref<BlockData>>> apply_block_to_state(
    const std::vector<td::Ref<vm::Cell>>& state_roots, const BlockCandidate& candidate) {
  try {
    td::Ref<vm::Cell> state_root;
    if (state_roots.size() == 1) {
      state_root = state_roots[0];
    } else {
      CHECK(state_roots.size() == 2);
      if (!block::gen::t_ShardState.cell_pack_split_state(state_root, state_roots[0], state_roots[1])) {
        return td::Status::Error("failed to make split_state");
      }
    }

    TRY_RESULT(block, create_block(candidate.id, candidate.data.clone()));
    block::gen::Block::Record rec;
    if (!block::gen::unpack_cell(block->root_cell(), rec)) {
      return td::Status::Error("failed to unpack Block");
    }
    auto result = vm::MerkleUpdate::apply(state_root, rec.state_update);
    if (result.is_null()) {
      return td::Status::Error("failed to apply Merkle update");
    }
    return std::make_pair(result, block);
  } catch (vm::CellBuilder::CellCreateError& e) {
    return td::Status::Error("failed to apply Merkle update: CellCreateError");
  } catch (vm::CellBuilder::CellWriteError& e) {
    return td::Status::Error("failed to apply Merkle update: CellWriteError");
  } catch (vm::VmError& e) {
    return e.as_status();
  }
}

td::Result<bool> get_before_split(const td::Ref<BlockData>& block) {
  block::gen::Block::Record blk;
  block::gen::BlockInfo::Record info;
  if (!(tlb::unpack_cell(block->root_cell(), blk) && tlb::unpack_cell(blk.info, info))) {
    return td::Status::Error("cannot unpack block header");
  }
  return info.before_split;
}

}  // namespace ton::validator::consensus
