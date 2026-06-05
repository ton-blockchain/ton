/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "validator/full-node.h"

#include "bus.h"
#include "stats.h"

namespace ton::validator::consensus {

namespace {

class BlockAccepterImpl : public td::actor::SpawnsWith<Bus>, public td::actor::ConnectsTo<Bus> {
 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  template <>
  void handle(BusHandle, std::shared_ptr<const StopRequested>) {
    stop();
  }

  template <>
  td::actor::Task<> process(BusHandle, std::shared_ptr<FinalizeBlock> event) {
    const auto& block = std::get<BlockCandidate>(event->candidate->block);
    auto block_data = create_block(block.id, block.data.clone()).move_as_ok();

    int broadcast_mode = fullnode::FullNode::broadcast_mode_custom;
    if (event->candidate->leader == owning_bus()->local_id.idx) {
      broadcast_mode |= fullnode::FullNode::broadcast_mode_public | fullnode::FullNode::broadcast_mode_fast_sync;
    }
    if (last_mc_finalized_seqno_ >= 2 && block.id.seqno() < last_mc_finalized_seqno_ - 2) {
      broadcast_mode = 0;
    }
    if (sent_candidate_broadcasts_.contains(block.id)) {
      broadcast_mode &= ~(fullnode::FullNode::broadcast_mode_fast_sync | fullnode::FullNode::broadcast_mode_custom);
    }
    co_await td::actor::ask(owning_bus()->manager, &ManagerFacade::accept_block, block.id, block_data,
                            event->candidate->leader.value(), event->signatures, broadcast_mode, true);
    owning_bus().publish<TraceEvent>(stats::BlockAccepted::create(event->candidate->id));
    co_return {};
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const BlockFinalizedInMasterchain> event) {
    last_mc_finalized_seqno_ = std::max(event->block.seqno(), last_mc_finalized_seqno_);
  }

  template <>
  void handle(BusHandle bus, std::shared_ptr<const CandidateGenerated> event) {
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
