/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "crypto/block/block-auto.h"
#include "crypto/vm/cells/MerkleUpdate.h"
#include "td/utils/format.h"
#include "validator/fabric.h"

#include "chain-state.h"

namespace ton::validator::consensus {

td::actor::Task<td::Ref<ChainState>> ChainState::from_manager(td::actor::ActorId<ManagerFacade> manager,
                                                              ShardIdFull shard, std::vector<BlockIdExt> blocks,
                                                              BlockIdExt min_mc_block_id) {
  auto make_from = [&](Tip tip) {
    return td::Ref<ChainState>(new ChainState{tip, min_mc_block_id}, td::Ref<ChainState>::acquire_t{});
  };

  if (blocks.size() == 1 && blocks[0].seqno() == 0) {
    CHECK(blocks[0].shard_full() == shard);
    auto state =
        co_await td::actor::ask(manager, &ManagerFacade::wait_block_state_root, blocks[0], td::Timestamp::in(10.0));
    co_return make_from(ZerostateTip{blocks[0], state});
  }

  std::vector<td::actor::StartedTask<td::Ref<vm::Cell>>> wait_state_root;
  std::vector<td::actor::StartedTask<td::Ref<BlockData>>> wait_block_data;
  for (auto block : blocks) {
    wait_state_root.push_back(
        td::actor::ask(manager, &ManagerFacade::wait_block_state_root, block, td::Timestamp::in(10.0)));
    if (block.seqno() != 0) {
      wait_block_data.push_back(
          td::actor::ask(manager, &ManagerFacade::wait_block_data, block, td::Timestamp::in(10.0)));
    }
  }
  auto states = co_await td::actor::all(std::move(wait_state_root));
  auto blocks_data = co_await td::actor::all(std::move(wait_block_data));

  if (blocks.size() == 2) {
    auto shard_0 = shard_child(shard_parent(blocks[0].shard_full()), true);
    auto shard_1 = shard_child(shard_parent(blocks[0].shard_full()), false);
    CHECK(blocks[0].shard_full() == shard_0 && blocks[1].shard_full() == shard_1);

    co_return make_from(BeforeMergeTip{
        .left = NormalTip{blocks_data[0], states[0]},
        .right = NormalTip{blocks_data[1], states[1]},
    });
  } else {
    CHECK(blocks.size() == 1);
    if (shard == blocks[0].shard_full()) {
      co_return make_from(NormalTip{blocks_data[0], states[0]});
    } else {
      CHECK(shard_is_parent(blocks[0].shard_full(), shard));
      co_return make_from(BeforeSplitTip{NormalTip{blocks_data[0], states[0]}});
    }
  }
}

td::Ref<ChainState> ChainState::from_zerostate(BlockIdExt zerostate, td::Ref<vm::Cell> state,
                                               BlockIdExt min_mc_block_id) {
  return td::Ref<ChainState>(new ChainState{ZerostateTip{zerostate, state}, min_mc_block_id},
                             td::Ref<ChainState>::acquire_t{});
}

std::vector<BlockIdExt> ChainState::block_ids() const {
  return std::visit([](const auto& tip) { return tip.block_ids(); }, tip_);
}

std::vector<td::Ref<BlockData>> ChainState::block_data() const {
  return std::visit([](const auto& tip) { return tip.block_data(); }, tip_);
}

std::vector<td::Ref<vm::Cell>> ChainState::state() const {
  return std::visit([](const auto& tip) { return tip.states(); }, tip_);
}

BlockIdExt ChainState::min_mc_block_id() const {
  return min_mc_block_id_;
}

BlockSeqno ChainState::next_seqno() const {
  return std::visit([](const auto& tip) { return tip.next_seqno(); }, tip_);
}

bool ChainState::is_before_split() const {
  if (auto normal_tip = std::get_if<NormalTip>(&tip_)) {
    block::gen::Block::Record blk;
    block::gen::BlockInfo::Record info;
    bool rc = tlb::unpack_cell(normal_tip->block->root_cell(), blk) && tlb::unpack_cell(blk.info, info);
    CHECK(rc);
    return info.before_split;
  }

  return false;
}

BlockIdExt ChainState::assert_normal() const {
  CHECK(std::holds_alternative<NormalTip>(tip_));
  return std::get<NormalTip>(tip_).block->block_id();
}

td::Ref<ChainState> ChainState::apply(const BlockCandidate& candidate) const {
  try {
    auto block = create_block(candidate.id, candidate.data.clone()).move_as_ok();

    block::gen::Block::Record rec;
    bool rc = block::gen::unpack_cell(block->root_cell(), rec);
    LOG_CHECK(rc) << "Failed to unpack block " << candidate.id.to_str();

    auto state = vm::MerkleUpdate::apply(root_, rec.state_update);
    LOG_CHECK(!state.is_null()) << "Failed to apply Merkle update of " << candidate.id.to_str();

    return td::Ref<ChainState>(new ChainState{NormalTip{block, state}, min_mc_block_id_},
                               td::Ref<ChainState>::acquire_t{});
  } catch (vm::CellBuilder::CellCreateError& e) {
    LOG(FATAL) << "Failed to apply Merkle update of " << candidate.id.to_str() << ": CellCreateError";
    __builtin_unreachable();
  } catch (vm::CellBuilder::CellWriteError& e) {
    LOG(FATAL) << "Failed to apply Merkle update of " << candidate.id.to_str() << ": CellWriteError";
    __builtin_unreachable();
  } catch (vm::VmError& e) {
    LOG(FATAL) << "Failed to apply block " << candidate.id.to_str() << ": VmError: " << e.as_status();
    __builtin_unreachable();
  }
}

td::Ref<vm::Cell> ChainState::BeforeMergeTip::root() const {
  td::Ref<vm::Cell> result;
  bool rc = block::gen::t_ShardState.cell_pack_split_state(result, left.state, right.state);
  CHECK(rc);
  return result;
}

ChainState::ChainState(Tip tip, BlockIdExt min_mc_block_id)
    : tip_(std::move(tip)), min_mc_block_id_(std::move(min_mc_block_id)) {
  root_ = std::visit([](const auto& tip) { return tip.root(); }, this->tip_);
}

td::StringBuilder& operator<<(td::StringBuilder& sb, const ChainState& state) {
  std::vector<std::string> blocks;
  for (const auto& block : state.block_ids()) {
    blocks.push_back(block.to_str());
  }

  return sb << "ChainState{min_mc_block_id=" << state.min_mc_block_id().to_str() << ", tip=" << blocks << "}";
}

}  // namespace ton::validator::consensus
