/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "td/actor/coro_utils.h"

#include "bus.h"
#include "stats.h"

namespace td {

td::StringBuilder& operator<<(td::StringBuilder& sb, const std::optional<ton::BlockIdExt>& id) {
  if (id.has_value()) {
    sb << id->to_str();
  } else {
    sb << "genesis";
  }
  return sb;
}

}  // namespace td

namespace ton::validator::consensus {

namespace {

class BlockValidatorImpl : public td::actor::SpawnsWith<Bus>, public td::actor::ConnectsTo<Bus> {
 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  template <>
  void handle(BusHandle, std::shared_ptr<const StopRequested>) {
    stop();
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const Start> event) {
    on_new_accepted_block(event->state->as_normal());
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const FinalizeBlock> event) {
    on_new_accepted_block(event->candidate->block_id());
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const BlockFinalizedInMasterchain> event) {
    if (event->block.shard_full() != owning_bus()->shard || event->block.seqno() == 0) {
      return;
    }
    on_new_accepted_block(event->block);
  }

  template <>
  td::actor::Task<ValidateCandidateResult> process(BusHandle, std::shared_ptr<ValidationRequest> event) {
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
      if (bus.shard.is_masterchain()) {
        auto expected_seqno = event->state->as_normal();
        while (last_accepted_block_ < expected_seqno) {
          auto [awaiter, promise] = td::actor::StartedTask<>::make_bridge();
          next_block_promises_.push_back(std::move(promise));
          co_await std::move(awaiter);
        }
        if (expected_seqno < last_accepted_block_) {
          co_return td::Status::Error(PSTRING()
                                      << "Candidate " << event->candidate->id << " builds upon " << expected_seqno
                                      << " but we already finalized " << last_accepted_block_);
        }
      }

      ValidateParams validate_params{
          .shard = bus.shard,
          .min_masterchain_block_id = event->state->min_mc_block_id(),
          .prev = event->state->block_ids(),
          .local_validator_id = bus.local_id.short_id,
          .skip_store_candidate = true,
          .is_new_consensus = true,
          .prev_block_state_roots = event->state->state(),
      };
      co_return co_await td::actor::ask(bus.manager, &ManagerFacade::validate_block_candidate, block.clone(),
                                        std::move(validate_params), td::Timestamp::in(60.0));
    };
    auto validation_result = co_await std::visit(td::overloaded(block_fn, empty_fn), event->candidate->block);

    owning_bus().publish<TraceEvent>(stats::ValidationFinished::create(event->candidate->id));

    if (validation_result.has<CandidateReject>()) {
      if (event->candidate->leader == bus.local_id.idx) {
        LOG(ERROR) << "BUG! Candidate " << event->candidate->id
                   << " is self-rejected: " << validation_result.get<CandidateReject>().reason;
      }
      co_return validation_result;
    }

    td::Timestamp ok_from = td::Timestamp::at_unix(validation_result.get<CandidateAccept>().ok_from_utime);
    if (!ok_from.is_in_past()) {
      LOG(INFO) << "Candidate " << event->candidate->id << " has timestamp in the future, wait for " << ok_from.in()
                << " s";
      co_await td::actor::coro_sleep(ok_from);
    }
    co_return validation_result;
  }

 private:
  void on_new_accepted_block(std::optional<BlockIdExt> block) {
    if (last_accepted_block_ < block) {
      last_accepted_block_ = block;
      auto next_block_promises = std::move(next_block_promises_);
      for (auto& promise : next_block_promises) {
        promise.set_value({});
      }
    }
  }

  std::optional<BlockIdExt> last_accepted_block_;
  std::vector<td::Promise<>> next_block_promises_;
};

}  // namespace

void BlockValidator::register_in(td::actor::Runtime& runtime) {
  runtime.register_actor<BlockValidatorImpl>("BlockValidator");
}

}  // namespace ton::validator::consensus
