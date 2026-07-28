/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include <algorithm>

#include "td/utils/LRUCache.h"
#include "validator/full-node.h"

#include "bus.h"

namespace ton::validator::consensus::simplex {

namespace {

class CandidateBroadcastRelayImpl : public td::actor::SpawnsWith<Bus>, public td::actor::ConnectsTo<Bus> {
 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  template <>
  void handle(BusHandle, std::shared_ptr<const StopRequested>) {
    stop();
  }

  template <>
  void handle(BusHandle bus, std::shared_ptr<const CandidateReceived> event) {
    const CandidateRef &candidate = event->candidate;
    if (candidate->is_empty() || is_too_old(candidate->id)) {
      return;
    }

    const auto &block = std::get<BlockCandidate>(candidate->block);
    candidates_.put(candidate->id, CachedCandidate{block.id, block.data.clone()});
    maybe_broadcast(bus, candidate->id);
  }

  template <>
  void handle(BusHandle bus, std::shared_ptr<const NotarizationObserved> event) {
    if (is_too_old(event->id)) {
      erase(event->id);
      return;
    }

    certified_.put(event->id, td::Unit{});
    maybe_broadcast(bus, event->id);
  }

  template <>
  void handle(BusHandle bus, std::shared_ptr<const FinalizationObserved> event) {
    if (is_too_old(event->id)) {
      erase(event->id);
      return;
    }

    // A final certificate is sufficient to authorize a candidate that arrived before finalization.
    // Keep it cached so the same candidate can also arrive shortly after finalization.
    certified_.put(event->id, td::Unit{});
    maybe_broadcast(bus, event->id);

    first_nonfinalized_slot_ = std::max(first_nonfinalized_slot_, event->id.slot + 1);
  }

 private:
  // Public candidate broadcasts contain block data only, so retaining collated data here would be wasteful.
  struct CachedCandidate {
    BlockIdExt block_id;
    td::BufferSlice data;
  };

  static constexpr td::uint32 MAX_FINALIZED_SLOT_GAP = 5;

  bool is_too_old(CandidateId id) const {
    return id.slot < first_nonfinalized_slot_ && first_nonfinalized_slot_ - id.slot > MAX_FINALIZED_SLOT_GAP;
  }

  void erase(CandidateId id) {
    candidates_.erase(id);
    certified_.erase(id);
  }

  void maybe_broadcast(BusHandle bus, CandidateId id) {
    if (!certified_.contains(id)) {
      return;
    }
    auto candidate = candidates_.get_if_exists(id);
    if (candidate == nullptr) {
      return;
    }

    BlockIdExt block_id = candidate->block_id;
    td::BufferSlice data = candidate->data.clone();
    erase(id);

    td::actor::send_closure(bus->manager, &ManagerFacade::send_block_candidate_broadcast, block_id, std::move(data),
                            fullnode::FullNode::broadcast_mode_all);
  }

  td::uint32 first_nonfinalized_slot_ = 0;
  td::LRUCache<CandidateId, CachedCandidate> candidates_{MAX_FINALIZED_SLOT_GAP * 2};
  td::LRUCache<CandidateId, td::Unit> certified_{MAX_FINALIZED_SLOT_GAP * 2};
};

}  // namespace

void CandidateBroadcastRelay::register_in(td::actor::Runtime &runtime) {
  runtime.register_actor<CandidateBroadcastRelayImpl>("CandidateBroadcastRelay");
}

}  // namespace ton::validator::consensus::simplex
