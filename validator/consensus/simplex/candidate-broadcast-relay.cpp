/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include <algorithm>
#include <limits>
#include <map>
#include <optional>

#include "validator/full-node.h"

#include "bus.h"

namespace ton::validator::consensus::simplex {

namespace {

class CandidateBroadcastRelayImpl : public td::actor::SpawnsWith<Bus>, public td::actor::ConnectsTo<Bus> {
 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  void start_up() override {
    const auto &bus = *owning_bus();

    slots_per_leader_window_ = bus.config.slots_per_leader_window;
    params_ = bus.config.noncritical_params;
    if (bus.first_nonannounced_window != 0) {
      current_window_ = bus.first_nonannounced_window - 1;
    }
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const StopRequested>) {
    stop();
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const NoncriticalParamsUpdated> event) {
    params_ = event->params;
    prune();
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const LeaderWindowObserved> event) {
    current_window_ = std::max(current_window_, event->start_slot / slots_per_leader_window_);
    prune();
  }

  template <>
  void handle(BusHandle bus, std::shared_ptr<const CandidateReceived> event) {
    const CandidateRef &candidate = event->candidate;
    if (candidate->is_empty() || is_out_of_range(candidate->id.slot)) {
      return;
    }

    const auto &block = std::get<BlockCandidate>(candidate->block);
    auto &entry = slots_[candidate->id.slot];
    if (entry.candidate.has_value() && entry.candidate->id != candidate->id) {
      return;
    }
    entry.candidate = CachedCandidate{candidate->id, block.id, block.data.clone()};
    maybe_broadcast(bus, candidate->id.slot);
  }

  template <>
  void handle(BusHandle bus, std::shared_ptr<const NotarizationObserved> event) {
    if (is_out_of_range(event->id.slot)) {
      slots_.erase(event->id.slot);
      return;
    }

    slots_[event->id.slot].certified = event->id;
    maybe_broadcast(bus, event->id.slot);
  }

  template <>
  void handle(BusHandle bus, std::shared_ptr<const FinalizationObserved> event) {
    if (is_out_of_range(event->id.slot)) {
      slots_.erase(event->id.slot);
    } else {
      // A final certificate is sufficient to authorize a candidate that arrived before finalization.
      // Keep it cached so the same candidate can also arrive shortly after finalization.
      slots_[event->id.slot].certified = event->id;
      maybe_broadcast(bus, event->id.slot);
    }

    first_nonfinalized_slot_ = std::max(first_nonfinalized_slot_, event->id.slot + 1);
    prune();
  }

 private:
  // Public candidate broadcasts contain block data only, so retaining collated data here would be wasteful.
  struct CachedCandidate {
    CandidateId id;
    BlockIdExt block_id;
    td::BufferSlice data;
  };

  struct SlotEntry {
    std::optional<CachedCandidate> candidate;
    std::optional<CandidateId> certified;
  };

  static constexpr td::uint32 MAX_FINALIZED_SLOT_GAP = 5;

  bool is_too_old(td::uint32 slot) const {
    return slot < first_nonfinalized_slot_ && first_nonfinalized_slot_ - slot > MAX_FINALIZED_SLOT_GAP;
  }

  td::uint64 first_too_new_slot() const {
    return (static_cast<td::uint64>(current_window_) + params_.max_leader_window_desync + 1) * slots_per_leader_window_;
  }

  bool is_too_new(td::uint32 slot) const {
    return slot >= first_too_new_slot();
  }

  bool is_out_of_range(td::uint32 slot) const {
    return is_too_old(slot) || is_too_new(slot);
  }

  void maybe_broadcast(BusHandle bus, td::uint32 slot) {
    auto it = slots_.find(slot);
    if (it == slots_.end()) {
      return;
    }
    auto &entry = it->second;
    if (!entry.candidate.has_value() || !entry.certified.has_value() || entry.candidate->id != *entry.certified) {
      return;
    }

    BlockIdExt block_id = entry.candidate->block_id;
    td::BufferSlice data = entry.candidate->data.clone();
    slots_.erase(it);

    td::actor::send_closure(bus->manager, &ManagerFacade::send_block_candidate_broadcast, block_id, std::move(data),
                            fullnode::FullNode::broadcast_mode_all);
  }

  void prune() {
    while (!slots_.empty() && is_too_old(slots_.begin()->first)) {
      slots_.erase(slots_.begin());
    }

    auto first_too_new = first_too_new_slot();
    slots_.erase(slots_.lower_bound(static_cast<td::uint32>(first_too_new)), slots_.end());
  }

  td::uint32 first_nonfinalized_slot_ = 0;
  td::uint32 current_window_ = 0;
  td::uint32 slots_per_leader_window_ = 0;
  NewConsensusConfig::NoncriticalParams params_;
  std::map<td::uint32, SlotEntry> slots_;
};

}  // namespace

void CandidateBroadcastRelay::register_in(td::actor::Runtime &runtime) {
  runtime.register_actor<CandidateBroadcastRelayImpl>("CandidateBroadcastRelay");
}

}  // namespace ton::validator::consensus::simplex
