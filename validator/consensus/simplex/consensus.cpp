/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "consensus/simplex/state.h"
#include "td/actor/coro_utils.h"

#include "bus.h"

namespace ton::validator::consensus::simplex {

namespace {

struct SlotState {
  SlotState(td::Unit) {
  }

  std::optional<RawCandidateRef> pending_block;
  std::optional<CandidateId> voted_notar;
  bool voted_skip = false;
  bool voted_final = false;
};

struct WindowState {
  WindowState(td::Unit) {
  }

  bool had_timeouts = false;
};

class ConsensusImpl : public runtime::SpawnsWith<Bus>, public runtime::ConnectsTo<Bus> {
  using State = ConsensusState<WindowState, SlotState, td::Unit, td::Unit>;

 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  void start_up() override {
    auto& bus = *owning_bus();

    slots_per_leader_window_ = bus.simplex_config.slots_per_leader_window;
    target_rate_s_ = bus.config.target_rate_ms / 1000.;
    first_block_timeout_s_ = bus.simplex_config.first_block_timeout_ms / 1000.;
    state_.emplace(State(bus.simplex_config.slots_per_leader_window, {}, {}));
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const StopRequested>) {
    stop();
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const FinalizationObserved> event) {
    state_->notify_finalized(event->id.slot);
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const NotarizationObserved> event) {
    auto slot = state_->slot_at(event->id.slot);
    if (!slot.has_value()) {
      return;
    }

    if (!slot->state->voted_skip && !slot->state->voted_final && slot->state->voted_notar == event->id) {
      owning_bus().publish<BroadcastVote>(FinalizeVote{event->id});
      slot->state->voted_final = true;
    }
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const LeaderWindowObserved> event) {
    auto& bus = *owning_bus();

    td::uint32 offset = event->start_slot % slots_per_leader_window_;
    if (offset == 0) {
      if (bus.collator_schedule->is_expected_collator(bus.local_id.idx, event->start_slot)) {
        start_generation(event->base, event->start_slot).start().detach();
      }
    }

    // FIXME: Use had_timeouts in determining timeout duration.
    auto timeout_at = td::Timestamp::in(first_block_timeout_s_);
    skip_timeouts_.emplace(timeout_at, event->start_slot - offset);
    alarm_timestamp().relax(timeout_at);
  }

  void alarm() override {
    while (!skip_timeouts_.empty() && skip_timeouts_.begin()->first.is_in_past()) {
      auto [ts, slot_idx] = *skip_timeouts_.begin();
      skip_timeouts_.erase(skip_timeouts_.begin());

      auto slot = state_->slot_at(slot_idx);
      td::uint32 offset = slot_idx % slots_per_leader_window_;

      if (!slot || slot->state->voted_final) {
        skip_timeouts_.emplace(ts.in(target_rate_s_), slot_idx + 1);
        continue;
      }

      slot->window->had_timeouts = true;

      for (td::uint32 i = 0; i < slots_per_leader_window_; ++i) {
        auto affected_slot = slot->window->slots[i];
        if (!affected_slot->voted_final) {
          owning_bus().publish<BroadcastVote>(SkipVote{slot_idx - offset + i});
          affected_slot->voted_skip = true;
        }
      }
    }

    if (!skip_timeouts_.empty()) {
      alarm_timestamp().relax(skip_timeouts_.begin()->first);
    }
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const CandidateReceived> event) {
    auto slot = state_->slot_at(event->candidate->id.slot);
    if (!slot.has_value()) {
      return;
    }

    const auto& candidate = event->candidate;

    if (candidate->parent_id.has_value() && candidate->parent_id->slot >= candidate->id.slot) {
      // FIXME: report misbehavior
      return;
    }

    if (slot->state->pending_block.has_value()) {
      if (slot->state->pending_block.value()->id != candidate->id) {
        // FIXME: Report misbehavior
      }
      return;
    }

    slot->state->pending_block = candidate;

    try_notarize(*slot).start().detach();
  }

 private:
  td::actor::Task<ParentId> resolve_parent(RawParentId parent) {
    if (parent.has_value()) {
      co_return co_await owning_bus().publish<ResolveCandidate>(*parent);
    } else {
      co_return std::nullopt;
    }
  }

  td::actor::Task<> start_generation(RawParentId raw_parent, td::uint32 start_slot) {
    auto parent = co_await resolve_parent(raw_parent);
    owning_bus().publish<OurLeaderWindowStarted>(parent, start_slot, start_slot + slots_per_leader_window_);
    co_return {};
  }

  td::actor::Task<> try_notarize(State::SlotRef slot) {
    const auto& candidate = *slot.state->pending_block;

    auto maybe_misbehavior = co_await owning_bus().publish<WaitForParent>(candidate);
    if (maybe_misbehavior) {
      owning_bus().publish<MisbehaviorReport>(candidate->leader, *maybe_misbehavior);
      co_return {};
    }

    auto parent = co_await resolve_parent(candidate->parent_id);
    if (candidate->parent_id != parent) {
      // FIXME: Report misbehavior
      co_return {};
    }

    auto resolved_candidate = td::make_ref<Candidate>(parent, candidate);
    auto validation_result = co_await owning_bus().publish<ValidationRequest>(resolved_candidate).wrap();

    if (validation_result.is_error()) {
      // FIXME: Report misbehavior
      co_return {};
    }

    slot.state->voted_notar = candidate->id;

    owning_bus().publish<BroadcastVote>(NotarizeVote{candidate->id});
    co_return {};
  }

  td::uint32 slots_per_leader_window_;
  double target_rate_s_;
  double first_block_timeout_s_;
  std::optional<State> state_;

  std::multimap<td::Timestamp, td::uint32> skip_timeouts_;
};

}  // namespace

void Consensus::register_in(runtime::Runtime& runtime) {
  runtime.register_actor<ConsensusImpl>("SimplexConsensus");
}

}  // namespace ton::validator::consensus::simplex
