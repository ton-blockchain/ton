/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "td/actor/coro_utils.h"

#include "bus.h"
#include "utils.h"

namespace ton::validator::consensus {

namespace {

class BlockValidatorImpl : public runtime::SpawnsWith<Bus>, public runtime::ConnectsTo<Bus> {
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
  td::actor::Task<> process(BusHandle, std::shared_ptr<ValidationRequest> event) {
    auto& bus = *owning_bus();

    td::uint32 slot = event->candidate->id.slot;

    if (std::holds_alternative<BlockIdExt>(event->candidate->block)) {
      co_return {};
    }
    const auto& candidate = std::get<BlockCandidate>(event->candidate->block);

    auto genesis = co_await genesis_.get();

    ValidateParams validate_params{
        .shard = bus.shard,
        .min_masterchain_block_id = genesis->min_masterchain_block_id,
        .prev = genesis->convert_id_to_blocks(event->candidate->parent_id),
        .local_validator_id = bus.local_id.short_id,
        .is_new_consensus = true,
        .prev_block_state_roots = event->prev_block_state_roots,
    };

    owning_bus().publish<StatsTargetReached>(StatsTargetReached::ValidateStarted, slot);

    auto validation_result =
        co_await td::actor::ask(bus.manager, &ManagerFacade::validate_block_candidate, candidate.clone(),
                                std::move(validate_params), td::Timestamp::in(60.0));

    owning_bus().publish<StatsTargetReached>(StatsTargetReached::ValidateFinished, slot);

    if (validation_result.has<CandidateReject>()) {
      auto error = td::Status::Error(0, validation_result.get<CandidateReject>().reason);

      if (event->candidate->leader == bus.local_id.idx) {
        LOG(ERROR) << "BUG! Candidate " << event->candidate->id << " is self-rejected: " << error;
      }

      co_return error;
    }

    td::Timestamp ok_from = td::Timestamp::at_unix(validation_result.get<CandidateAccept>().ok_from_utime);
    if (!ok_from.is_in_past()) {
      LOG(INFO) << "Candidate " << event->candidate->id << " has timestamp in the future, wait for " << ok_from.in()
                << " s";
      co_await td::actor::coro_sleep(ok_from);
    }

    co_return {};
  }

 private:
  td::Promise<StartEvent> genesis_promise_;
  SharedFuture<StartEvent> genesis_;
};

}  // namespace

void BlockValidator::register_in(runtime::Runtime& runtime) {
  runtime.register_actor<BlockValidatorImpl>("BlockValidator");
}

}  // namespace ton::validator::consensus
