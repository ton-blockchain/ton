/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "validator/full-node.h"

#include "bus.h"
#include "utils.h"

namespace ton::validator::consensus {

namespace {

class BlockAccepterImpl : public runtime::SpawnsWith<Bus>, public runtime::ConnectsTo<Bus> {
 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  void start_up() override {
    auto [awaiter, promise] = td::actor::StartedTask<StartEvent>::make_bridge();
    genesis_promise_ = std::move(promise);
    genesis_ = std::move(awaiter);
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const StopRequested>) {
    stop();
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const Start> event) {
    genesis_promise_.set_value(std::move(event));
  }

  template <>
  td::actor::Task<> process(BusHandle, std::shared_ptr<FinalizeBlock> event) {
    const auto& block = std::get<BlockCandidate>(event->candidate->block);
    auto block_data = create_block(block.id, block.data.clone()).move_as_ok();
    auto block_parents = (co_await genesis_.get())->convert_id_to_blocks(event->parent_id);

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
    co_return co_await td::actor::ask(owning_bus()->manager, &ManagerFacade::accept_block, block.id, block_data,
                                      block_parents, event->candidate->leader.value(), event->signatures,
                                      broadcast_mode, true);
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
  td::Promise<StartEvent> genesis_promise_;
  SharedFuture<StartEvent> genesis_;
  BlockSeqno last_mc_finalized_seqno_ = 0;
  std::set<BlockIdExt> sent_candidate_broadcasts_;
};

}  // namespace

void BlockAccepter::register_in(runtime::Runtime& runtime) {
  runtime.register_actor<BlockAccepterImpl>("BlockAccepter");
}

}  // namespace ton::validator::consensus
