/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "consensus/utils.h"
#include "td/actor/SharedFuture.h"
#include "td/actor/coro_utils.h"

#include "bus.h"

namespace ton::validator::consensus::simplex {

namespace tl {

using db_key_finalizedBlock = ton_api::consensus_simplex_db_key_finalizedBlock;
using db_key_finalizedBlockRef = tl_object_ptr<db_key_finalizedBlock>;

}  // namespace tl

namespace {

class StateResolverImpl : public runtime::SpawnsWith<Bus>, public runtime::ConnectsTo<Bus> {
  using ResolvedState = ResolveState::Result;

 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  void start_up() override {
    auto [awaiter, promise] = td::actor::StartedTask<StartEvent>::make_bridge();
    genesis_promise_ = std::move(promise);
    genesis_ = std::move(awaiter);

    auto data = owning_bus()->db->get_by_prefix(ton_api::consensus_simplex_db_key_finalizedBlock::ID);
    for (auto& [key_str, _] : data) {
      auto key = fetch_tl_object<ton_api::consensus_simplex_db_key_finalizedBlock>(key_str, true).ensure().move_as_ok();
      finalized_blocks_[CandidateId::from_tl(key->candidateId_)].done = true;
    }
    LOG(INFO) << "Loaded " << data.size() << " finalized blocks from DB";
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
    finalize_blocks(event->id, event->certificate, std::nullopt).start().detach();
  }

  template <>
  td::actor::Task<ResolvedState> process(BusHandle, std::shared_ptr<ResolveState> request) {
    co_return co_await resolve_state(request->id);
  }

 private:
  // ===== State resolution =====
  struct CachedState {
    std::optional<ResolvedState> result;
    bool started = false;
    std::vector<td::Promise<ResolvedState>> promises;
  };

  td::Promise<StartEvent> genesis_promise_;
  td::actor::SharedFuture<StartEvent> genesis_;

  std::map<ParentId, CachedState> state_cache_;

  td::actor::Task<ResolvedState> resolve_state(ParentId id) {
    CachedState& entry = state_cache_[id];
    if (entry.result.has_value()) {
      co_return *entry.result;
    }
    auto [task, promise] = td::actor::StartedTask<ResolvedState>::make_bridge();
    entry.promises.push_back(std::move(promise));
    if (!entry.started) {
      entry.started = true;
      auto result = co_await resolve_state_inner(id).wrap();
      for (auto& p : entry.promises) {
        p.set_result(result.clone());
      }
      entry.promises.clear();
      if (result.is_ok()) {
        entry.result = result.move_as_ok();
      } else {
        state_cache_.erase(id);
      }
    }
    co_return co_await std::move(task);
  }

  td::actor::Task<ResolvedState> resolve_state_inner(ParentId id) {
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
      co_return ResolvedState{
          .state = state,
      };
    }

    auto candidate = (co_await owning_bus().publish<ResolveCandidate>(*id)).candidate;
    auto prev_data_state = co_await resolve_state(candidate->parent_id);

    auto empty_fn = [&](BlockIdExt) { return prev_data_state; };
    auto block_fn = [&](const BlockCandidate& candidate) {
      return ResolvedState{
          .state = prev_data_state.state->apply(candidate),
          .gen_utime_exact = get_candidate_gen_utime_exact(candidate).move_as_ok(),
      };
    };
    co_return std::visit(td::overloaded(empty_fn, block_fn), candidate->block);
  }

  // ===== Block finalization =====
  struct FinalizedBlock {
    bool done = false;
    bool started = false;
    std::vector<td::Promise<td::Unit>> waiters;
  };

  std::map<CandidateId, FinalizedBlock> finalized_blocks_;

  td::actor::Task<> finalize_blocks(CandidateId id, std::optional<FinalCertRef> final_cert,
                                    std::optional<CandidateRef> final_candidate) {
    FinalizedBlock& state = finalized_blocks_[id];
    if (state.done) {
      co_return {};
    }
    auto [task, promise] = td::actor::StartedTask<td::Unit>::make_bridge();
    state.waiters.push_back(std::move(promise));
    if (!state.started) {
      state.started = true;
      auto result = co_await finalize_blocks_inner(id, final_cert, final_candidate).wrap();
      for (auto& p : state.waiters) {
        p.set_result(result.clone());
      }
      state.waiters.clear();
      if (result.is_ok()) {
        state.done = true;
      } else {
        finalized_blocks_.erase(id);
      }
    }
    co_return co_await std::move(task);
  }

  td::actor::Task<> finalize_blocks_inner(CandidateId id, std::optional<FinalCertRef> final_cert,
                                          std::optional<CandidateRef> final_candidate) {
    auto& bus = *owning_bus();

    if (!final_cert && bus.shard.is_masterchain()) {
      co_return {};
    }

    auto [candidate, notar_cert] = co_await owning_bus().publish<ResolveCandidate>(id);
    if (final_cert && !final_candidate) {
      CHECK((*final_cert)->vote.id == id);
      final_candidate = candidate;
    }

    if (!candidate->is_empty()) {
      if (auto parent = candidate->parent_id) {
        co_await finalize_blocks(*parent, std::nullopt, std::nullopt);
      }

      td::Ref<block::BlockSignatureSet> sig_set;
      if (final_cert) {
        sig_set = (*final_cert)->to_signature_set(*final_candidate, bus);
      } else {
        sig_set = notar_cert->to_signature_set(candidate, bus);
      }
      co_await owning_bus().publish<FinalizeBlock>(candidate, sig_set);
    } else {
      if (auto parent = candidate->parent_id) {
        co_await finalize_blocks(*parent, final_cert, final_candidate);
      }
    }

    auto key = create_serialize_tl_object<tl::db_key_finalizedBlock>(id.to_tl());
    co_await bus.db->set(std::move(key), td::BufferSlice());
    co_return {};
  }
};

}  // namespace

void StateResolver::register_in(runtime::Runtime& runtime) {
  runtime.register_actor<StateResolverImpl>("StateResolver");
}

}  // namespace ton::validator::consensus::simplex
