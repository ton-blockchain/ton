/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "td/actor/coro_utils.h"

#include "bus.h"
#include "stats.h"

namespace ton::validator::consensus {

namespace {

class BlockValidatorImpl : public runtime::SpawnsWith<Bus>, public runtime::ConnectsTo<Bus> {
 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  template <>
  void handle(BusHandle, std::shared_ptr<const StopRequested>) {
    stop();
  }

  template <>
  td::actor::Task<> process(BusHandle, std::shared_ptr<ValidationRequest> event) {
    auto& bus = *owning_bus();

    if (std::holds_alternative<BlockIdExt>(event->candidate->block)) {
      co_return {};
    }
    const auto& candidate = std::get<BlockCandidate>(event->candidate->block);

    ValidateParams validate_params{
        .shard = bus.shard,
        .min_masterchain_block_id = event->state->min_mc_block_id(),
        .prev = event->state->block_ids(),
        .local_validator_id = bus.local_id.short_id,
        .is_new_consensus = true,
        .prev_block_state_roots = event->state->state(),
    };

    owning_bus().publish<TraceEvent>(stats::ValidationStarted::create(event->candidate->id));

    auto validation_result =
        co_await td::actor::ask(bus.manager, &ManagerFacade::validate_block_candidate, candidate.clone(),
                                std::move(validate_params), td::Timestamp::in(60.0));

    owning_bus().publish<TraceEvent>(stats::ValidationFinished::create(event->candidate->id));

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
};

}  // namespace

void BlockValidator::register_in(runtime::Runtime& runtime) {
  runtime.register_actor<BlockValidatorImpl>("BlockValidator");
}

}  // namespace ton::validator::consensus
