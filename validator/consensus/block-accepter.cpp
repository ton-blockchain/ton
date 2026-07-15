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

  template <>
  void handle(BusHandle, std::shared_ptr<const StopRequested>) {
    stop();
  }

  template <>
  td::actor::Task<> process(BusHandle, std::shared_ptr<FinalizeBlock> event) {
    auto& bus = *owning_bus();
    const auto& block = std::get<BlockCandidate>(event->candidate->block);
    auto block_data = create_block(block.id, block.data.clone()).move_as_ok();
    bool send_finality_broadcast = block.id.seqno() + 8 > last_mc_finalized_seqno_;
    co_await td::actor::ask(bus.manager, &ManagerFacade::accept_block, block.id, block_data,
                            event->candidate->leader.value(), event->signatures, send_finality_broadcast,
                            /* accept = */ true);
    owning_bus().publish<TraceEvent>(stats::BlockAccepted::create(event->candidate->id));
    co_return {};
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const BlockFinalizedInMasterchain> event) {
    last_mc_finalized_seqno_ = std::max(event->block.seqno(), last_mc_finalized_seqno_);
  }

 private:
  BlockSeqno last_mc_finalized_seqno_ = 0;
};

}  // namespace

void BlockAccepter::register_in(td::actor::Runtime& runtime) {
  runtime.register_actor<BlockAccepterImpl>("BlockAccepter");
}

}  // namespace ton::validator::consensus
