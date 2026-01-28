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

    owning_bus().publish<TraceEvent>(stats::ValidationStarted::create(event->candidate->id));

    auto empty_fn = [&](BlockIdExt block) -> td::actor::Task<ValidateCandidateResult> {
      if (block != event->state->as_normal()) {
        co_return CandidateReject{
            .reason = "Wrong referenced block in empty candidate",
            .proof = td::BufferSlice(),
        };
      }
      co_return CandidateAccept{};
    };
    auto block_fn = [&](const BlockCandidate& block) -> td::actor::Task<ValidateCandidateResult> {
      ValidateParams validate_params{
          .shard = bus.shard,
          .min_masterchain_block_id = event->state->min_mc_block_id(),
          .prev = event->state->block_ids(),
          .local_validator_id = bus.local_id.short_id,
          .is_new_consensus = true,
          .prev_block_state_roots = event->state->state(),
      };
      co_return co_await td::actor::ask(bus.manager, &ManagerFacade::validate_block_candidate, block.clone(),
                                        std::move(validate_params), td::Timestamp::in(60.0));
    };
    auto validation_result = co_await std::visit(td::overloaded(block_fn, empty_fn), event->candidate->block);

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
