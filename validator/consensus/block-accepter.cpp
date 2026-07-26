/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include <set>

#include "validator/full-node.h"

#include "bus.h"
#include "stats.h"

namespace ton::validator::consensus {

namespace {

class BlockAccepterImpl : public td::actor::SpawnsWith<Bus>, public td::actor::ConnectsTo<Bus> {
 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  static bool should_be_spawned(const Bus& bus) {
    return bus.is_validator() || bus.config.observers_in_private_overlay();
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const StopRequested>) {
    stop();
  }

  template <>
  td::actor::Task<> process(BusHandle, std::shared_ptr<FinalizeBlock> event) {
    auto& bus = *owning_bus();

    const auto& block = std::get<BlockCandidate>(event->candidate->block);
    auto block_data = create_block(block.id, block.data.clone()).move_as_ok();

    bool is_leader = bus.is_validator() && event->candidate->leader == bus.local_id->idx;

    int block_broadcast_mode = fullnode::FullNode::broadcast_mode_custom;
    int finality_broadcast_mode = 0;
    bool send_shard_block_desc = true;
    if (bus.config.enable_plumtree_broadcast()) {
      finality_broadcast_mode = fullnode::FullNode::broadcast_mode_custom |
                                fullnode::FullNode::broadcast_mode_fast_sync |
                                fullnode::FullNode::broadcast_mode_public;
      if (is_leader) {
        block_broadcast_mode |= fullnode::FullNode::broadcast_mode_public;
      }
      send_shard_block_desc = false;
    } else {
      if (is_leader) {
        block_broadcast_mode |=
            fullnode::FullNode::broadcast_mode_public | fullnode::FullNode::broadcast_mode_fast_sync;
      }
      if (sent_candidate_broadcasts_.contains(block.id)) {
        block_broadcast_mode &=
            ~(fullnode::FullNode::broadcast_mode_fast_sync | fullnode::FullNode::broadcast_mode_custom);
      }
    }
    if (last_mc_finalized_seqno_ >= 2 && block.id.seqno() < last_mc_finalized_seqno_ - 2) {
      block_broadcast_mode = 0;
      finality_broadcast_mode = 0;
    }
    co_await td::actor::ask(bus.manager, &ManagerFacade::accept_block, block.id, block_data,
                            event->candidate->leader.value(), event->signatures, block_broadcast_mode,
                            finality_broadcast_mode, send_shard_block_desc, true);
    owning_bus().publish<TraceEvent>(stats::BlockAccepted::create(event->candidate->id));
    co_return {};
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const BlockFinalizedInMasterchain> event) {
    last_mc_finalized_seqno_ = std::max(event->block.seqno(), last_mc_finalized_seqno_);
  }

  template <>
  void handle(BusHandle bus, std::shared_ptr<const CandidateGenerated> event) {
    if (bus->config.enable_plumtree_broadcast()) {
      return;
    }
    if (bus->shard.is_masterchain() || event->candidate->is_empty()) {
      return;
    }
    const BlockCandidate& candidate = std::get<BlockCandidate>(event->candidate->block);
    if (!sent_candidate_broadcasts_.insert(candidate.id).second) {
      return;
    }
    td::actor::send_closure(bus->manager, &ManagerFacade::send_block_candidate_broadcast, candidate.id,
                            candidate.data.clone(),
                            fullnode::FullNode::broadcast_mode_fast_sync | fullnode::FullNode::broadcast_mode_custom);
  }

 private:
  BlockSeqno last_mc_finalized_seqno_ = 0;
  std::set<BlockIdExt> sent_candidate_broadcasts_;
};

}  // namespace

void BlockAccepter::register_in(td::actor::Runtime& runtime) {
  runtime.register_actor<BlockAccepterImpl>("BlockAccepter");
}

}  // namespace ton::validator::consensus
