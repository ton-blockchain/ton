/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include <map>

#include "validator-session/validator-session-types.h"

#include "checksum.h"
#include "consensus-bus.h"

namespace ton::validator {

namespace {

class StatsCollectorImpl : public runtime::SpawnsWith<ConsensusBus>, public runtime::ConnectsTo<ConsensusBus> {
 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  void start_up() override {
    total_weight_ = 0;
    for (const auto& v : owning_bus()->validator_set) {
      total_weight_ += v.weight;
    }
  }

  template <>
  void handle(runtime::BusHandle<ConsensusBus>, std::shared_ptr<const ConsensusBus::StopRequested>) {
    stop();
  }

  template <>
  void handle(runtime::BusHandle<ConsensusBus>, std::shared_ptr<const ConsensusBus::CandidateGenerated> event) {
    BlockSeqno seqno = event->candidate.id().seqno();

    auto& producer = create_stats_for(seqno, owning_bus()->local_id.short_id);
    producer.is_ours = true;
    producer.block_status = validatorsession::ValidatorSessionStats::status_received;
    producer.block_id = event->candidate.id();
    producer.collated_data_hash = sha256_bits256(event->candidate.block->collated_data.as_slice());
    producer.got_block_at = td::Clocks::system();
    producer.got_block_by = validatorsession::ValidatorSessionStats::recv_collated;
    producer.collated_at = td::Clocks::system();
    producer.self_collated = !event->collator_id.has_value();
    if (event->collator_id.has_value()) {
      producer.collator_node_id = event->collator_id.value();
    }
  }

  template <>
  void handle(runtime::BusHandle<ConsensusBus>, std::shared_ptr<const ConsensusBus::CandidateReceived> event) {
    BlockSeqno seqno = event->candidate.id().seqno();

    auto& producer = create_stats_for(seqno, event->candidate.leader.short_id);
    producer.block_status = validatorsession::ValidatorSessionStats::status_received;
    producer.block_id = event->candidate.id();
    producer.collated_data_hash = sha256_bits256(event->candidate.block->collated_data.as_slice());
    producer.got_block_at = td::Clocks::system();
    producer.got_block_by = validatorsession::ValidatorSessionStats::recv_broadcast;
  }

  template <>
  void handle(runtime::BusHandle<ConsensusBus>, std::shared_ptr<const ConsensusBus::CandidateValidated> event) {
    BlockSeqno seqno = event->candidate.id().seqno();

    auto& producer = create_stats_for(seqno, event->candidate.leader.short_id);
    if (event->verdict.is_ok()) {
      producer.validated_at = td::Clocks::system();
    }
  }

  template <>
  void handle(runtime::BusHandle<ConsensusBus>, std::shared_ptr<const ConsensusBus::BlockFinalized> event) {
    BlockSeqno seqno = event->id.seqno();

    auto& producer = create_stats_for(seqno, event->leader.short_id);
    producer.is_accepted = true;
    producer.block_status = validatorsession::ValidatorSessionStats::status_approved;
    producer.block_id = event->id;

    auto now = td::Clocks::system();
    producer.approved_33pct_at = now;
    producer.approved_66pct_at = now;
    producer.signed_33pct_at = now;
    producer.signed_66pct_at = now;

    auto it = rounds_.find(seqno);
    if (it != rounds_.end()) {
      send_stats_for_block(seqno, event->id, true);
      rounds_.erase(it);
    }
  }

 private:
  struct RoundStats {
    double started_at = -1.0;
    validatorsession::ValidatorSessionStats::Producer producer;
  };

  validatorsession::ValidatorSessionStats::Producer& create_stats_for(BlockSeqno seqno, PublicKeyHash validator_id) {
    auto& round = rounds_[seqno];

    if (round.started_at == -1) {
      round.started_at = td::Clocks::system();
    }

    round.producer.validator_id = validator_id;

    return round.producer;
  }

  void send_stats_for_block(BlockSeqno seqno, BlockIdExt block_id, bool success) {
    auto it = rounds_.find(seqno);
    if (it == rounds_.end()) {
      return;
    }

    auto& bus = *owning_bus();

    validatorsession::ValidatorSessionStats stats;
    stats.session_id = bus.session_id;
    stats.self = bus.local_id.short_id;
    stats.block_id = block_id;
    stats.cc_seqno = bus.validator_set_for_external_code->get_catchain_seqno();
    stats.success = success;
    stats.timestamp = td::Clocks::system();
    stats.creator = it->second.producer.validator_id;
    stats.total_validators = static_cast<td::uint32>(bus.validator_set.size());
    stats.total_weight = total_weight_;

    validatorsession::ValidatorSessionStats::Round round;
    round.started_at = it->second.started_at;
    round.producers = {it->second.producer};

    stats.first_round = 0;
    stats.rounds.push_back(std::move(round));

    stats.fix_block_ids();

    td::actor::send_closure(bus.manager, &ManagerFacade::log_validator_session_stats, std::move(stats));
  }

  ValidatorWeight total_weight_ = 0;

  std::map<BlockSeqno, RoundStats> rounds_;
};

}  // namespace

void StatsCollector::register_in(runtime::Runtime& runtime) {
  runtime.register_actor<StatsCollectorImpl>("StatsCollector");
}

}  // namespace ton::validator
