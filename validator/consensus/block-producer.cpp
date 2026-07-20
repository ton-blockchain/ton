/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "td/actor/coro_task.h"
#include "td/utils/CancellationToken.h"

#include "bus.h"
#include "window-producer.h"

namespace ton::validator::consensus {

namespace {

class BlockProducerImpl : public td::actor::SpawnsWith<Bus>, public td::actor::ConnectsTo<Bus> {
 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  static bool should_be_spawned(const Bus& bus) {
    return bus.is_validator();
  }

  void start_up() {
    target_rate_ = owning_bus()->config.noncritical_params.target_rate;
    no_empty_blocks_on_error_timeout_ = owning_bus()->config.noncritical_params.no_empty_blocks_on_error_timeout;
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const NoncriticalParamsUpdated> event) {
    target_rate_ = event->params.target_rate;
    no_empty_blocks_on_error_timeout_ = event->params.no_empty_blocks_on_error_timeout;
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const Start> event) {
    empty_block_policy_.observe_session_start(event->state->next_seqno() - 1);
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const StopRequested>) {
    current_leader_window_ = std::nullopt;
    cancellation_source_.cancel();
    stop();
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const FinalizeBlock> event) {
    if (event->signatures->is_final()) {
      empty_block_policy_.observe_consensus_finalized(event->candidate->block_id().seqno());
    }
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const OurLeaderWindowStarted> event) {
    CHECK(current_leader_window_ < event->start_slot);

    current_leader_window_ = event->start_slot;
    cancellation_source_ = td::CancellationTokenSource();
    generate_candidates(event).start().detach();
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const BlockFinalizedInMasterchain> event) {
    empty_block_policy_.observe_mc_finalized(event->block.seqno());
  }

 private:
  td::actor::Task<> generate_candidates(std::shared_ptr<const OurLeaderWindowStarted> event) {
    auto& bus = *owning_bus();

    auto window = current_leader_window_;
    if (window == std::nullopt) {
      co_return {};
    }

    ProduceWindowContext ctx{
        .base = event->base,
        .state = event->state,
        .start_slot = event->start_slot,
        .end_slot = event->end_slot,
        .start_time = event->start_time,
        .leader = *bus.local_id,
        .signing_key = bus.local_id->short_id,
        .delegation = std::nullopt,
        .target_rate = target_rate_,
        .cancellation_token = cancellation_source_.get_cancellation_token(),
        .is_superseded = [&, window] { return current_leader_window_ != window; },
        .should_generate_empty_block =
            [&](const ChainStateRef& state) {
              return empty_block_policy_.should_generate_empty_block(owning_bus()->shard.is_masterchain(), state);
            },
        .allow_empty_on_generation_failure =
            [&] { return empty_block_policy_.allow_empty_on_generation_failure(no_empty_blocks_on_error_timeout_); },
    };
    co_await produce_window(owning_bus(), std::move(ctx));

    if (current_leader_window_ == window) {
      current_leader_window_ = std::nullopt;
    }

    co_return {};
  }

  std::optional<td::uint32> current_leader_window_;
  td::CancellationTokenSource cancellation_source_;

  EmptyBlockPolicy empty_block_policy_;
  std::chrono::milliseconds target_rate_;
  std::chrono::milliseconds no_empty_blocks_on_error_timeout_;
};

}  // namespace

void BlockProducer::register_in(td::actor::Runtime& runtime) {
  runtime.register_actor<BlockProducerImpl>("BlockProducer");
}

}  // namespace ton::validator::consensus
