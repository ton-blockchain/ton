/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "consensus/simplex/state.h"
#include "consensus/utils.h"
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
    load_from_db();
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const StopRequested>) {
    stop();
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const FinalizationObserved> event) {
    state_->notify_finalized(event->id.slot);
    finalize_blocks(event).start().detach();
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
    auto timeout_at = td::Timestamp::in(first_block_timeout_s_ + target_rate_s_);
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
  td::actor::Task<> start_generation(RawParentId raw_parent, td::uint32 start_slot) {
    auto parent = co_await get_resolved_candidate(raw_parent);
    td::Timestamp start_time = td::Timestamp::now();
    if (parent.gen_utime_exact.has_value()) {
      start_time = std::max(start_time, td::Timestamp::at_unix(*parent.gen_utime_exact + target_rate_s_));
      start_time = std::min(start_time, td::Timestamp::in(target_rate_s_));
    }
    owning_bus().publish<OurLeaderWindowStarted>(parent.id, start_slot, start_slot + slots_per_leader_window_,
                                                 start_time, std::move(parent.state_roots),
                                                 std::move(parent.block_data));
    co_return {};
  }

  td::actor::Task<> try_notarize(State::SlotRef slot) {
    const auto& candidate = *slot.state->pending_block;

    auto maybe_misbehavior = co_await owning_bus().publish<WaitForParent>(candidate);
    if (maybe_misbehavior) {
      owning_bus().publish<MisbehaviorReport>(candidate->leader, *maybe_misbehavior);
      co_return {};
    }

    auto parent = co_await get_resolved_candidate(candidate->parent_id);
    if (candidate->parent_id != parent.id) {
      // FIXME: Report misbehavior
      co_return {};
    }

    auto resolved_candidate = td::make_ref<Candidate>(parent.id, candidate);
    auto validation_result =
        co_await owning_bus().publish<ValidationRequest>(resolved_candidate, std::move(parent.state_roots)).wrap();

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

  struct ResolvedCandidate {
    ParentId id;
    std::vector<td::Ref<vm::Cell>> state_roots;
    std::vector<td::Ref<BlockData>> block_data;
    std::optional<double> gen_utime_exact = std::nullopt;
  };
  struct ResolvedCandidateEntry {
    std::optional<ResolvedCandidate> result;
    bool started = false;
    std::vector<td::Promise<ResolvedCandidate>> promises;
  };
  std::map<RawParentId, ResolvedCandidateEntry> block_data_state_cache_;

  td::actor::Task<ResolvedCandidate> get_resolved_candidate(RawParentId id) {
    ResolvedCandidateEntry& entry = block_data_state_cache_[id];
    if (entry.result.has_value()) {
      co_return *entry.result;
    }
    auto [task, promise] = td::actor::StartedTask<ResolvedCandidate>::make_bridge();
    entry.promises.push_back(std::move(promise));
    if (!entry.started) {
      entry.started = true;
      auto result = co_await get_resolved_candidate_inner(id).wrap();
      for (auto& p : entry.promises) {
        p.set_result(result.clone());
      }
      entry.promises.clear();
      if (result.is_ok()) {
        entry.result = result.move_as_ok();
      } else {
        block_data_state_cache_.erase(id);
      }
    }
    co_return co_await std::move(task);
  }

  td::actor::Task<ResolvedCandidate> get_resolved_candidate_inner(RawParentId id) {
    if (!id.has_value() || finalized_blocks_.contains(*id)) {
      std::vector<BlockIdExt> block_ids;
      ParentId id_full;
      if (id.has_value()) {
        auto candidate = (co_await owning_bus().publish<ResolveCandidate>(*id)).candidate;
        block_ids = {candidate->id.block};
        id_full = candidate->id;
      } else {
        block_ids = owning_bus()->first_block_parents;
        id_full = std::nullopt;
      }
      std::vector<td::actor::StartedTask<td::Ref<vm::Cell>>> wait_state_root;
      std::vector<td::actor::StartedTask<td::Ref<BlockData>>> wait_block_data;
      for (const BlockIdExt& block_id : block_ids) {
        wait_state_root.push_back(td::actor::ask(owning_bus()->manager, &ManagerFacade::wait_block_state_root, block_id,
                                                 td::Timestamp::in(10.0)));
        if (block_id.seqno() != 0) {
          wait_block_data.push_back(td::actor::ask(owning_bus()->manager, &ManagerFacade::wait_block_data, block_id,
                                                   td::Timestamp::in(10.0)));
        }
      }
      co_return ResolvedCandidate{
          .id = id_full,
          .state_roots = co_await td::actor::all(std::move(wait_state_root)),
          .block_data = co_await td::actor::all(std::move(wait_block_data)),
      };
    }

    auto candidate = (co_await owning_bus().publish<ResolveCandidate>(*id)).candidate;
    auto prev_data_state = co_await get_resolved_candidate(candidate->parent_id);
    if (std::holds_alternative<BlockIdExt>(candidate->block)) {
      prev_data_state.id = candidate->id;
      co_return prev_data_state;
    }
    const auto& block_candidate = std::get<BlockCandidate>(candidate->block);
    auto [new_state_root, new_block_data] = co_await apply_block_to_state(prev_data_state.state_roots, block_candidate);
    ResolvedCandidate result{
        .id = candidate->id,
        .state_roots = {new_state_root},
        .block_data = {new_block_data},
    };
    auto r_gen_utime_exact = get_candidate_gen_utime_exact(block_candidate);
    if (r_gen_utime_exact.is_error()) {
      LOG(WARNING) << "Cannot get exact timestamp of the previous block: " << r_gen_utime_exact.move_as_error();
    } else {
      result.gen_utime_exact = r_gen_utime_exact.move_as_ok();
    }
    co_return result;
  }

  std::set<RawCandidateId> finalized_blocks_;

  td::actor::Task<> finalize_blocks(std::shared_ptr<const FinalizationObserved> event) {
    RawCandidateId id = event->id;
    RawCandidateRef first_candidate;
    bool is_first_block = true;
    while (!finalized_blocks_.contains(id)) {
      finalized_blocks_.insert(id);
      owning_bus()
          ->db
          ->set(create_serialize_tl_object<ton_api::consensus_simplex_db_key_finalizedBlock>(id.to_tl()),
                td::BufferSlice{})
          .start()
          .detach();
      auto candidate = co_await owning_bus().publish<ResolveCandidate>(id);
      if (first_candidate.is_null()) {
        first_candidate = candidate.candidate;
      }
      if (std::holds_alternative<BlockCandidate>(candidate.candidate->block)) {
        auto& bus = *owning_bus();
        td::Ref<block::BlockSignatureSet> sig_set;
        if (is_first_block) {
          std::vector<BlockSignature> signatures;
          for (const auto& s : event->certificate->signatures) {
            signatures.emplace_back(bus.validator_set[s.validator.value()].short_id.tl(), s.signature.clone());
          }
          sig_set = block::BlockSignatureSet::create_simplex(
              std::move(signatures), bus.cc_seqno, bus.validator_set_hash, bus.session_id, first_candidate->id.slot,
              first_candidate->hash_data().to_tl());
        } else {
          std::vector<BlockSignature> signatures;
          for (const auto& s : candidate.notar->signatures) {
            signatures.emplace_back(bus.validator_set[s.validator.value()].short_id.tl(), s.signature.clone());
          }
          sig_set = block::BlockSignatureSet::create_simplex_approve(
              std::move(signatures), bus.cc_seqno, bus.validator_set_hash, bus.session_id, candidate.candidate->id.slot,
              candidate.candidate->hash_data().to_tl());
        }
        is_first_block = false;
        ParentId parent_id = std::nullopt;
        if (candidate.candidate->parent_id.has_value()) {
          parent_id = (co_await owning_bus().publish<ResolveCandidate>(*candidate.candidate->parent_id)).candidate->id;
        }
        owning_bus().publish<BlockFinalized>(candidate.candidate, parent_id, std::move(sig_set));
        if (owning_bus()->shard.is_masterchain()) {
          break;
        }
      }
      if (!candidate.candidate->parent_id.has_value()) {
        break;
      }
      id = *candidate.candidate->parent_id;
    }
    co_return td::Unit{};
  }

  void load_from_db() {
    auto blocks = owning_bus()->db->get_by_prefix(ton_api::consensus_simplex_db_key_finalizedBlock::ID);
    for (auto& [key, _] : blocks) {
      auto f = fetch_tl_object<ton_api::consensus_simplex_db_key_finalizedBlock>(key, true).ensure().move_as_ok();
      finalized_blocks_.insert(RawCandidateId::from_tl(f->candidateId_));
    }
    LOG(INFO) << "Loaded " << blocks.size() << " finalized blocks from DB";
  }
};

}  // namespace

void Consensus::register_in(runtime::Runtime& runtime) {
  runtime.register_actor<ConsensusImpl>("SimplexConsensus");
}

}  // namespace ton::validator::consensus::simplex
