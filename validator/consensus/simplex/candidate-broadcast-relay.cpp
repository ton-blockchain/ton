/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "consensus/stats.h"
#include "consensus/window-producer.h"
#include "td/actor/SharedFuture.h"
#include "td/actor/coro_task.h"
#include "td/utils/CancellationToken.h"

#include "bus.h"
#include "full-node.h"
#include "state.h"

namespace ton::validator::consensus::simplex {

namespace {

class CandidateBroadcastRelayImpl : public td::actor::SpawnsWith<Bus>, public td::actor::ConnectsTo<Bus> {
 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  static bool should_be_spawned(const Bus& bus) {
    return bus.config.observers_in_private_overlay();
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const StopRequested>) {
    stop();
  }

  template <>
  void handle(BusHandle bus, std::shared_ptr<const CandidateReceived> event) {
    if (!bus->is_validator() && !bus->is_collator) {
      bus.publish<StoreCandidate>(event->candidate).start().detach("store candidate");
    }
    if (event->candidate->is_empty()) {
      return;
    }

    int mode = fullnode::FullNode::broadcast_mode_custom | fullnode::FullNode::broadcast_mode_fast_sync;
    if (bus->is_collator) {
      mode = fullnode::FullNode::broadcast_mode_custom;
    }
    const auto& block = std::get<BlockCandidate>(event->candidate->block);
    td::actor::send_closure(bus->manager, &ManagerFacade::send_block_candidate_broadcast, block.id, block.data.clone(),
                            mode);
  }
};

}  // namespace

void CandidateBroadcastRelay::register_in(td::actor::Runtime& runtime) {
  runtime.register_actor<CandidateBroadcastRelayImpl>("CandidateBroadcastRelay");
}

}  // namespace ton::validator::consensus::simplex
