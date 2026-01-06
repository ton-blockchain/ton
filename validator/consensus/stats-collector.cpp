/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include <map>

#include "validator-session/validator-session-types.h"

#include "bus.h"

namespace ton::validator::consensus {

namespace {

class StatsCollectorImpl : public runtime::SpawnsWith<Bus>, public runtime::ConnectsTo<Bus> {
 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  void start_up() override {
    for (const auto& v : owning_bus()->validator_set) {
      total_weight_ += v.weight;
    }
    system_clock_offset_ = td::Clocks::system() - td::Clocks::monotonic();
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const StopRequested>) {
    stop();
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const StatsTargetReached> event) {
    auto& stats = stats_for_[event->slot];
    auto timestamp = event->timestamp.at() + system_clock_offset_;

    switch (event->target) {
      case StatsTargetReached::CollateStarted: {
        stats.got_submit_at = timestamp;
        break;
      }

      case StatsTargetReached::CollateFinished: {
        stats.got_block_at = timestamp;
        stats.got_block_by = validatorsession::ValidatorSessionStats::recv_collated;
        stats.collated_at = timestamp;
        break;
      }

      case StatsTargetReached::CandidateReceived: {
        stats.got_submit_at = timestamp;
        stats.got_block_at = timestamp;
        stats.got_block_by = validatorsession::ValidatorSessionStats::recv_broadcast;
        break;
      }

      case StatsTargetReached::ValidateStarted: {
        stats.validation_time = timestamp;
        break;
      }

      case StatsTargetReached::ValidateFinished: {
        stats.validated_at = timestamp;
        stats.validation_time = timestamp - stats.validated_at;
        break;
      }

      case StatsTargetReached::NotarObserved: {
        stats.approved_33pct_at = stats.approved_66pct_at = timestamp;
      }

      case StatsTargetReached::FinalObserved:
        stats.signed_33pct_at = timestamp;
        stats.block_status = validatorsession::ValidatorSessionStats::status_approved;
        break;
    }
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const CandidateGenerated> event) {
    auto& stats = stats_for_[event->candidate->id.slot];

    stats.is_ours = true;
    stats.self_collated = !event->collator_id.has_value();
    if (event->collator_id.has_value()) {
      stats.collator_node_id = event->collator_id.value().bits256_value();
    }
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const BlockFinalized> event) {
    auto id = event->candidate->id;
    auto& stats = stats_for_[id.slot];

    stats.block_id = id.block;
    stats.is_accepted = true;
    stats.signed_66pct_at = td::Clocks::system();
    send_stats_for_block(id.slot);

    first_nonfinalized_slot_ = id.slot + 1;
    while (!stats_for_.empty() && stats_for_.begin()->first <= id.slot) {
      stats_for_.erase(stats_for_.begin());
    }
  }

 private:
  void send_stats_for_block(td::uint32 slot) {
    auto& producer_stats = stats_for_[slot];
    auto& bus = *owning_bus();

    validatorsession::ValidatorSessionStats stats;
    stats.session_id = bus.session_id;
    stats.self = bus.local_id.short_id;
    stats.block_id = producer_stats.block_id;
    stats.success = true;
    stats.timestamp = td::Clocks::system();
    stats.creator = producer_stats.validator_id;
    stats.total_validators = static_cast<td::uint32>(bus.validator_set.size());
    stats.total_weight = total_weight_;

    validatorsession::ValidatorSessionStats::Round round;
    round.started_at = producer_stats.got_submit_at;
    round.producers = {producer_stats};

    // Remove
    if (bus.shard.is_masterchain()) {
      stats.first_round = slot % 4 != 1;
    } else {
      stats.first_round = slot % 4 != 0;
    }
    stats.rounds.push_back(std::move(round));

    stats.fix_block_ids();

    td::actor::send_closure(bus.manager, &ManagerFacade::log_validator_session_stats, std::move(stats));
  }

  ValidatorWeight total_weight_ = 0;
  double system_clock_offset_ = 0;

  td::uint32 first_nonfinalized_slot_ = 0;
  std::map<td::uint32, validatorsession::ValidatorSessionStats::Producer> stats_for_;
};

}  // namespace

void StatsCollector::register_in(runtime::Runtime& runtime) {
  runtime.register_actor<StatsCollectorImpl>("StatsCollector");
}

}  // namespace ton::validator::consensus
