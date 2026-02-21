/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "consensus/simplex/state.h"
#include "consensus/utils.h"
#include "td/actor/SharedFuture.h"
#include "td/actor/coro_utils.h"

#include "bus.h"

namespace ton::validator::consensus::simplex {

namespace {

struct SlotState {
  SlotState(td::Unit) {
  }

  std::optional<CandidateRef> pending_block;
  std::optional<CandidateId> voted_notar;
  bool voted_skip = false;
  bool voted_final = false;
};

class ConsensusImpl : public runtime::SpawnsWith<Bus>, public runtime::ConnectsTo<Bus> {
  using State = ConsensusState<SlotState, td::Unit>;

 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  void start_up() override {
    auto [awaiter, promise] = td::actor::StartedTask<StartEvent>::make_bridge();
    genesis_promise_ = std::move(promise);
    genesis_ = std::move(awaiter);

    auto& bus = *owning_bus();

    slots_per_leader_window_ = bus.simplex_config.slots_per_leader_window;
    max_leader_window_desync_ = bus.simplex_config.max_leader_window_desync;
    target_rate_s_ = bus.config.target_rate_ms / 1000.;
    default_first_block_timeout_s_ = bus.simplex_config.first_block_timeout_ms / 1000.;
    first_block_timeout_s_ = default_first_block_timeout_s_;
    state_.emplace(State({}));
    load_from_db();

    for (const auto& vote : bus.bootstrap_votes) {
      if (vote.validator != bus.local_id.idx) {
        continue;
      }

      auto slot = state_->slot_at(vote.vote.referenced_slot());
      if (!slot.has_value()) {
        continue;
      }

      auto notar_fn = [&](const NotarizeVote& notar_vote) { slot->state->voted_notar = notar_vote.id; };
      auto final_fn = [&](const FinalizeVote& final_vote) { slot->state->voted_final = true; };
      auto skip_fn = [&](const SkipVote& skip_vote) { slot->state->voted_skip = true; };
      std::visit(td::overloaded(notar_fn, final_fn, skip_fn), vote.vote.vote);
    }

    if (auto window = bus.first_nonannounced_window) {
      auto start_slot = (window - 1) * slots_per_leader_window_;
      auto end_slot = window * slots_per_leader_window_;
      for (td::uint32 i = start_slot; i < end_slot; ++i) {
        auto slot = state_->slot_at(i);
        if (slot.has_value() && !slot->state->voted_final) {
          slot->state->voted_skip = true;
          owning_bus().publish<BroadcastVote>(SkipVote{i});
        }
      }
    }
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const Start> event) {
    genesis_promise_.set_value(std::move(event));
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const StopRequested>) {
    stop();
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const FinalizationObserved> event) {
    state_->notify_finalized(event->id.slot);
    finalize_blocks(event->id, event->certificate).start().detach();
  }

  template <>
  void handle(BusHandle bus, std::shared_ptr<const NotarizationObserved> event) {
    process_notarization_observed(bus, event).start().detach();
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const LeaderWindowObserved> event) {
    auto& bus = *owning_bus();
    td::uint32 new_window = event->start_slot / slots_per_leader_window_;

    if (previous_window_had_skip_) {
      first_block_timeout_s_ =
          std::min(first_block_timeout_s_ * bus.first_block_timeout_multipler, bus.first_block_max_timeout_s);
    } else {
      first_block_timeout_s_ = default_first_block_timeout_s_;
    }

    td::uint32 offset = event->start_slot % slots_per_leader_window_;
    if (offset == 0) {
      previous_window_had_skip_ = false;

      if (bus.collator_schedule->is_expected_collator(bus.local_id.idx, event->start_slot)) {
        start_generation(event->base, event->start_slot).start().detach();
      }
    }
    current_window_ = new_window;

    if (timeout_slot_ <= event->start_slot) {
      timeout_slot_ = event->start_slot + 1;
      timeout_base_ = td::Timestamp::in(first_block_timeout_s_);
      alarm_timestamp() = td::Timestamp::in(target_rate_s_, timeout_base_);
    }
  }

  void alarm() override {
    td::uint32 range_start = timeout_slot_ - 1;
    td::uint32 window_start = range_start - range_start % slots_per_leader_window_;
    td::uint32 window_end = window_start + slots_per_leader_window_;
    for (td::uint32 i = range_start; i < window_end; ++i) {
      auto slot = state_->slot_at(i);
      if (slot && !slot->state->voted_final) {
        owning_bus().publish<BroadcastVote>(SkipVote{i});
        slot->state->voted_skip = true;
        previous_window_had_skip_ = true;
      }
    }
    timeout_slot_ = window_end;
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const CandidateReceived> event) {
    td::uint32 slot_idx = event->candidate->id.slot;
    td::uint32 first_too_new_slot = (current_window_ + max_leader_window_desync_ + 1) * slots_per_leader_window_;
    if (slot_idx >= first_too_new_slot) {
      LOG(WARNING) << "Dropping too new candidate from " << event->candidate->leader << " : slot=" << slot_idx
                   << ", current_window=" << current_window_ * slots_per_leader_window_;
      return;
    }
    auto slot = state_->slot_at(slot_idx);
    if (!slot.has_value()) {
      return;
    }
    if (slot->state->voted_notar) {
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
  td::actor::Task<> start_generation(ParentId base, td::uint32 start_slot) {
    auto parent = co_await get_resolved_candidate(base);
    td::Timestamp start_time = td::Timestamp::now();
    if (parent.gen_utime_exact.has_value()) {
      start_time = std::max(start_time, td::Timestamp::at_unix(*parent.gen_utime_exact + target_rate_s_));
      start_time = std::min(start_time, td::Timestamp::in(target_rate_s_));
    }
    owning_bus().publish<OurLeaderWindowStarted>(parent.id, parent.state, start_slot,
                                                 start_slot + slots_per_leader_window_, start_time);
    co_return {};
  }

  td::actor::Task<> try_notarize(State::SlotRef slot) {
    const auto& candidate = *slot.state->pending_block;
    auto store_candidate = owning_bus().publish<StoreCandidate>(candidate).start();

    auto maybe_misbehavior = co_await owning_bus().publish<WaitForParent>(candidate);
    if (maybe_misbehavior) {
      owning_bus().publish<MisbehaviorReport>(candidate->leader, *maybe_misbehavior);
      co_return {};
    }

    auto parent = co_await get_resolved_candidate(candidate->parent_id);
    CHECK(candidate->parent_id == parent.id);

    auto validation_result = co_await owning_bus().publish<ValidationRequest>(parent.state, candidate);

    if (validation_result.has<CandidateReject>()) {
      LOG(WARNING) << "Candidate " << candidate->id
                   << " is rejected: " << validation_result.get<CandidateReject>().reason;
      // FIXME: Report misbehavior
      co_return {};
    }
    co_await std::move(store_candidate);

    slot.state->voted_notar = candidate->id;

    owning_bus().publish<BroadcastVote>(NotarizeVote{candidate->id});
    co_return {};
  }

  td::actor::Task<> process_notarization_observed(BusHandle, std::shared_ptr<const NotarizationObserved> event) {
    auto slot = state_->slot_at(event->id.slot);
    if (!slot.has_value()) {
      co_return {};
    }

    co_await owning_bus().publish<WaitNotarCertStored>(event->id);

    if (timeout_slot_ <= event->id.slot + 1) {
      if ((event->id.slot + 1) % slots_per_leader_window_ == 0) {
        // If we are at the end of the window, we defer setting timeout to LeaderWindowObserved.
        // Note that the condition `timeout_slot_ <= event->id.slot` can't be true if
        // LeaderWindowObserver for the next slot already ran.
        timeout_slot_ = event->id.slot + 1;
      } else {
        // Otherwise, we want to set timeout for notarization of the slot following this one.
        timeout_slot_ = event->id.slot + 2;
      }

      // In the first case above, alarm_timestamp() is very likely to be at this position via
      // NotarCert of the previous slot but in case we missed the certificate let's give the
      // certificate as much time as protocol allows to arrive.
      alarm_timestamp() = td::Timestamp::in(
          (timeout_slot_ - current_window_ * slots_per_leader_window_) * target_rate_s_, timeout_base_);
    }

    if (!slot->state->voted_skip && !slot->state->voted_final && slot->state->voted_notar == event->id) {
      owning_bus().publish<BroadcastVote>(FinalizeVote{event->id});
      slot->state->voted_final = true;
    }
    co_return {};
  }

  td::uint32 slots_per_leader_window_;
  td::uint32 max_leader_window_desync_;
  td::Timestamp timeout_base_;
  td::uint32 timeout_slot_ = 0;  // By alarm_timestamp(), slots < timeout_slot_ should be notarized.
  double target_rate_s_;
  double default_first_block_timeout_s_;
  double first_block_timeout_s_;
  bool previous_window_had_skip_ = false;
  std::optional<State> state_;
  td::uint32 current_window_ = 0;

  struct ResolvedCandidate {
    ParentId id;
    ChainStateRef state;
    std::optional<double> gen_utime_exact = std::nullopt;
  };
  struct ResolvedCandidateEntry {
    std::optional<ResolvedCandidate> result;
    bool started = false;
    std::vector<td::Promise<ResolvedCandidate>> promises;
  };
  std::map<ParentId, ResolvedCandidateEntry> block_data_state_cache_;

  td::Promise<StartEvent> genesis_promise_;
  td::actor::SharedFuture<StartEvent> genesis_;

  td::actor::Task<ResolvedCandidate> get_resolved_candidate(ParentId id) {
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

  td::actor::Task<ResolvedCandidate> get_resolved_candidate_inner(ParentId id) {
    if (!id.has_value() || finalized_blocks_.contains(*id)) {
      std::vector<BlockIdExt> block;
      auto genesis = co_await genesis_.get();
      if (id.has_value()) {
        auto candidate = (co_await owning_bus().publish<ResolveCandidate>(*id)).candidate;
        block = {candidate->block_id()};
      } else {
        block = genesis->state->block_ids();
      }
      auto state = co_await ChainState::from_manager(owning_bus()->manager, owning_bus()->shard, block,
                                                     genesis->state->min_mc_block_id());
      co_return ResolvedCandidate{
          .id = id,
          .state = state,
      };
    }

    auto candidate = (co_await owning_bus().publish<ResolveCandidate>(*id)).candidate;
    auto prev_data_state = co_await get_resolved_candidate(candidate->parent_id);
    if (std::holds_alternative<BlockIdExt>(candidate->block)) {
      prev_data_state.id = candidate->id;
      co_return prev_data_state;
    }
    const auto& block_candidate = std::get<BlockCandidate>(candidate->block);
    ResolvedCandidate result{
        .id = candidate->id,
        .state = prev_data_state.state->apply(block_candidate),
    };
    auto r_gen_utime_exact = get_candidate_gen_utime_exact(block_candidate);
    if (r_gen_utime_exact.is_error()) {
      LOG(WARNING) << "Cannot get exact timestamp of the previous block: " << r_gen_utime_exact.move_as_error();
    } else {
      result.gen_utime_exact = r_gen_utime_exact.move_as_ok();
    }
    co_return result;
  }

  struct FinalizedBlock {
    std::optional<CandidateId> done = std::nullopt;
    bool started = false;
    std::vector<td::Promise<CandidateId>> waiters;
  };
  std::map<CandidateId, FinalizedBlock> finalized_blocks_;

  td::actor::Task<CandidateId> finalize_blocks(CandidateId id, FinalCertRef maybe_final_cert,
                                               CandidateRef maybe_final_candidate = {}) {
    FinalizedBlock& state = finalized_blocks_[id];
    if (state.done.has_value()) {
      co_return state.done.value();
    }
    auto [task, promise] = td::actor::StartedTask<CandidateId>::make_bridge();
    state.waiters.push_back(std::move(promise));
    if (!state.started) {
      state.started = true;
      auto result = co_await finalize_blocks_inner(id, maybe_final_cert, maybe_final_candidate).wrap();
      for (auto& p : state.waiters) {
        p.set_result(result.clone());
      }
      state.waiters.clear();
      if (result.is_ok()) {
        state.done = result.move_as_ok();
      } else {
        finalized_blocks_.erase(id);
      }
    }
    co_return co_await std::move(task);
  }

  td::actor::Task<CandidateId> finalize_blocks_inner(CandidateId id, FinalCertRef maybe_final_cert,
                                                     CandidateRef maybe_final_candidate = {}) {
    auto& bus = owning_bus();
    auto [candidate, notar_cert] = co_await bus.publish<ResolveCandidate>(id);
    if (maybe_final_cert.is_null() && owning_bus()->shard.is_masterchain()) {
      co_return candidate->id;
    }
    if (maybe_final_cert.not_null() && maybe_final_candidate.is_null()) {
      maybe_final_candidate = candidate;
    }
    bool is_empty = std::holds_alternative<BlockIdExt>(candidate->block);
    ParentId parent_id;
    if (candidate->parent_id.has_value()) {
      if (is_empty) {
        parent_id = co_await finalize_blocks(candidate->parent_id.value(), maybe_final_cert, maybe_final_candidate);
      } else {
        parent_id = co_await finalize_blocks(candidate->parent_id.value(), {});
      }
    } else {
      parent_id = std::nullopt;
    }
    if (!is_empty) {
      td::Ref<block::BlockSignatureSet> sig_set;
      std::vector<BlockSignature> signatures;
      if (maybe_final_cert.not_null()) {
        for (const auto& s : maybe_final_cert->signatures) {
          signatures.emplace_back(bus->validator_set[s.validator.value()].short_id.tl(), s.signature.clone());
        }
        sig_set = block::BlockSignatureSet::create_simplex(
            std::move(signatures), bus->cc_seqno, bus->validator_set_hash, bus->session_id,
            maybe_final_candidate->id.slot, maybe_final_candidate->hash_data().to_tl());
      } else {
        std::vector<BlockSignature> signatures;
        for (const auto& s : notar_cert->signatures) {
          signatures.emplace_back(bus->validator_set[s.validator.value()].short_id.tl(), s.signature.clone());
        }
        sig_set = block::BlockSignatureSet::create_simplex_approve(std::move(signatures), bus->cc_seqno,
                                                                   bus->validator_set_hash, bus->session_id,
                                                                   candidate->id.slot, candidate->hash_data().to_tl());
      }
      do_finalize_block(id, candidate, parent_id, sig_set).start().detach();
    } else if (!owning_bus()->shard.is_masterchain()) {
      owning_bus()
          ->db
          ->set(create_serialize_tl_object<ton_api::consensus_simplex_db_key_finalizedBlock>(id.to_tl()),
                create_serialize_tl_object<ton_api::consensus_simplex_db_finalizedBlock>(
                    create_tl_block_id(candidate->block_id()), CandidateId::parent_id_to_tl(candidate->parent_id),
                    maybe_final_cert.not_null()))
          .start()
          .detach();
    }
    co_return candidate->id;
  }

  td::actor::Task<> do_finalize_block(CandidateId id, CandidateRef candidate, ParentId parent_id,
                                      td::Ref<block::BlockSignatureSet> sig_set) {
    co_await owning_bus().publish<FinalizeBlock>(candidate, sig_set);
    co_await owning_bus()->db->set(
        create_serialize_tl_object<ton_api::consensus_simplex_db_key_finalizedBlock>(id.to_tl()),
        create_serialize_tl_object<ton_api::consensus_simplex_db_finalizedBlock>(
            create_tl_block_id(candidate->block_id()), CandidateId::parent_id_to_tl(candidate->parent_id),
            sig_set->is_final()));
    co_return td::Unit{};
  }

  void load_from_db() {
    auto data = owning_bus()->db->get_by_prefix(ton_api::consensus_simplex_db_key_finalizedBlock::ID);
    std::vector<std::pair<CandidateId, std::pair<ParentId, bool>>> blocks;
    for (auto& [key_str, value_str] : data) {
      auto key = fetch_tl_object<ton_api::consensus_simplex_db_key_finalizedBlock>(key_str, true).ensure().move_as_ok();
      auto value = fetch_tl_object<ton_api::consensus_simplex_db_finalizedBlock>(value_str, true).ensure().move_as_ok();
      blocks.emplace_back(CandidateId::from_tl(key->candidateId_),
                          std::make_pair(CandidateId::tl_to_parent_id(value->parent_), value->is_final_));
    }
    std::sort(blocks.begin(), blocks.end(), [](const auto& x, const auto& y) { return x.first.slot < y.first.slot; });
    size_t cnt = 0;
    for (size_t i = 0; i < blocks.size(); ++i) {
      auto& [id, p] = blocks[i];
      auto& [parent_id, is_final] = p;
      ParentId expected_parent_id;
      if (i == 0) {
        expected_parent_id = std::nullopt;
      } else {
        expected_parent_id = blocks[i - 1].first;
      }
      if (expected_parent_id != parent_id && !owning_bus()->shard.is_masterchain()) {
        continue;
      }
      ++cnt;
      finalized_blocks_[id].done = id;
    }
    LOG(INFO) << "Loaded " << cnt << " finalized blocks from DB";
  }
};

}  // namespace

void Consensus::register_in(runtime::Runtime& runtime) {
  runtime.register_actor<ConsensusImpl>("SimplexConsensus");
}

}  // namespace ton::validator::consensus::simplex
