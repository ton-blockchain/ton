/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "td/actor/coro_utils.h"

#include "bus.h"

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

    td::uint32 slot = event->candidate->id.slot;

    if (std::holds_alternative<BlockIdExt>(event->candidate->block)) {
      co_return {};
    }
    const auto& candidate = std::get<BlockCandidate>(event->candidate->block);

    ValidateParams validate_params{
        .shard = bus.shard,
        .min_masterchain_block_id = bus.min_masterchain_block_id,
        .prev = bus.convert_id_to_blocks(event->candidate->parent_id),
        .local_validator_id = bus.local_id.short_id,
    };

    owning_bus().publish<StatsTargetReached>(StatsTargetReached::ValidateStarted, slot);

    auto maybe_candidate_reject =
        co_await td::actor::ask(bus.manager, &ManagerFacade::validate_block_candidate, candidate.clone(),
                                std::move(validate_params), td::Timestamp::in(60.0));

    owning_bus().publish<StatsTargetReached>(StatsTargetReached::ValidateFinished, slot);

    if (maybe_candidate_reject.has<CandidateReject>()) {
      auto error = td::Status::Error(0, maybe_candidate_reject.get<CandidateReject>().reason);

      if (event->candidate->leader == bus.local_id.idx) {
        LOG(ERROR) << "BUG! Candidate " << event->candidate->id << " is self-rejected: " << error;
      }

      co_return error;
    }

    co_return {};
  }
};

}  // namespace

void BlockValidator::register_in(runtime::Runtime& runtime) {
  runtime.register_actor<BlockValidatorImpl>("BlockValidator");
}

}  // namespace ton::validator::consensus
