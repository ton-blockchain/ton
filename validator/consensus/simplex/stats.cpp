/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "stats.h"
#include "validator-session-types.h"

namespace ton::validator::consensus::simplex::stats {

std::unique_ptr<Voted> Voted::create(Vote vote) {
  return std::unique_ptr<Voted>(new Voted(std::move(vote)));
}

consensus::stats::tl::EventRef Voted::to_tl() const {
  return create_tl_object<tl::voted>(vote_.to_tl());
}

std::string Voted::to_string() const {
  return PSTRING() << "Voted{vote=" << vote_ << "}";
}

void Voted::collect_to(MetricCollector& collector) const {
  collector.collect_voted(*this);
}

Voted::Voted(Vote vote) : vote_(std::move(vote)) {
}

std::unique_ptr<CertObserved> CertObserved::create(Vote vote) {
  return std::unique_ptr<CertObserved>(new CertObserved(std::move(vote)));
}

consensus::stats::tl::EventRef CertObserved::to_tl() const {
  return create_tl_object<tl::certObserved>(vote_.to_tl());
}

std::string CertObserved::to_string() const {
  return PSTRING() << "CertObserved{vote=" << vote_ << "}";
}

void CertObserved::collect_to(MetricCollector& collector) const {
  collector.collect_cert_observed(*this);
}

CertObserved::CertObserved(Vote vote) : vote_(std::move(vote)) {
}

bool Flow::is_normal() const {
  if (!(candidate_received && validation_started && validation_finished && notarize_voted && notarize_cert_observed &&
        finalize_voted && finalize_cert_observed && block_id.has_value())) {
    return false;
  }
  if (is_collator) {
    return collate_started && collate_finished;
  }
  return true;
}

MetricCollector::MetricCollector(ValidatorSessionId session_id, PublicKeyHash self_id,
                                 std::unique_ptr<ton::stats::Recorder> recorder)
    : session_id_(session_id), self_id_(self_id), recorder_(std::move(recorder)) {
}

void MetricCollector::collect_collate_started(const consensus::stats::CollateStarted& event) {
  if (event.target_slot() < first_non_accepted_slot_) {
    return;
  }
  collate_started_by_slot_[event.target_slot()] = event.ts();
}

void MetricCollector::collect_collate_finished(const consensus::stats::CollateFinished& event) {
  if (event.id().slot < first_non_accepted_slot_) {
    return;
  }
  auto& flow = flows_[event.id()];
  flow.collate_finished = event.ts();

  auto it = collate_started_by_slot_.find(event.target_slot());
  if (it != collate_started_by_slot_.end()) {
    flow.collate_started = it->second;
    collate_started_by_slot_.erase(it);
  }
}

void MetricCollector::collect_candidate_received(const consensus::stats::CandidateReceived& event) {
  if (event.id().slot < first_non_accepted_slot_) {
    return;
  }
  auto& flow = flows_[event.id()];
  flow.candidate_received = event.ts();
  flow.block_id = event.block_id();
  flow.is_collator = event.is_collator();
}

void MetricCollector::collect_validation_started(const consensus::stats::ValidationStarted& event) {
  if (event.id().slot < first_non_accepted_slot_) {
    return;
  }
  flows_[event.id()].validation_started = event.ts();
}

void MetricCollector::collect_validation_finished(const consensus::stats::ValidationFinished& event) {
  if (event.id().slot < first_non_accepted_slot_) {
    return;
  }
  flows_[event.id()].validation_finished = event.ts();
}

void MetricCollector::collect_block_accepted(const consensus::stats::BlockAccepted& event) {
  if (event.id().slot < first_non_accepted_slot_) {
    return;
  }
  flows_[event.id()].manager_accepted = event.ts();

  first_non_accepted_slot_ = event.id().slot + 1;

  while (!flows_.empty()) {
    auto it = flows_.begin();
    if (it->first.slot < first_non_accepted_slot_) {
      log_fake_catchain_stats(it->second);
      flows_.erase(it);
    } else {
      break;
    }
  }

  while (collate_started_by_slot_.begin() != collate_started_by_slot_.end()) {
    auto it = collate_started_by_slot_.begin();
    if (it->first < first_non_accepted_slot_) {
      collate_started_by_slot_.erase(it);
    } else {
      break;
    }
  }
}

void MetricCollector::collect_voted(const Voted& event) {
  std::visit(
      [&]<typename T>(const T& v) {
        if constexpr (std::is_same_v<T, SkipVote>) {
          return;
        } else {
          if (v.id.slot < first_non_accepted_slot_) {
            return;
          }
          auto& flow = flows_[v.id];
          double timestamp = event.ts();
          if constexpr (std::is_same_v<T, NotarizeVote>) {
            flow.notarize_voted = timestamp;
          } else if constexpr (std::is_same_v<T, FinalizeVote>) {
            flow.finalize_voted = timestamp;
          }
        }
      },
      event.vote().vote);
}

void MetricCollector::collect_cert_observed(const CertObserved& event) {
  std::visit(
      [&]<typename T>(const T& v) {
        if constexpr (std::is_same_v<T, SkipVote>) {
          return;
        } else {
          if (v.id.slot < first_non_accepted_slot_) {
            return;
          }
          auto& flow = flows_[v.id];
          double timestamp = event.ts();
          if constexpr (std::is_same_v<T, NotarizeVote>) {
            flow.notarize_cert_observed = timestamp;
          } else if constexpr (std::is_same_v<T, FinalizeVote>) {
            flow.finalize_cert_observed = timestamp;
          }
        }
      },
      event.vote().vote);
}

namespace {

validatorsession::ValidatorSessionStats::Producer flow_to_legacy_stats(const Flow& flow) {
  return {
      .block_status = validatorsession::ValidatorSessionStats::status_approved,
      .block_id = *flow.block_id,
      .is_accepted = true,
      .is_ours = flow.is_collator,
      .got_block_at = *flow.candidate_received,
      .got_block_by = flow.is_collator ? validatorsession::ValidatorSessionStats::recv_collated
                                       : validatorsession::ValidatorSessionStats::recv_broadcast,
      .got_submit_at = flow.is_collator ? *flow.collate_started : *flow.candidate_received,
      .comment = "",
      .collation_time = flow.is_collator ? (*flow.collate_finished - *flow.collate_started) : -1.0,
      .collated_at = flow.is_collator ? *flow.collate_finished : -1.0,
      .self_collated = flow.is_collator,
      .validation_time = *flow.validation_finished - *flow.validation_started,
      .validated_at = *flow.validation_finished,
      .approvers = {},
      .signers = {},
      .approved_33pct_at = *flow.validation_finished,
      .approved_66pct_at = *flow.notarize_cert_observed,
      .signed_33pct_at = *flow.finalize_cert_observed,
      .signed_66pct_at = *flow.manager_accepted,
  };
}

}  // namespace

void MetricCollector::log_fake_catchain_stats(const Flow& flow) {
  if (!flow.is_normal()) {
    return;
  }

  validatorsession::ValidatorSessionStats stats;
  stats.session_id = session_id_;
  stats.self = self_id_;
  stats.block_id = *flow.block_id;
  stats.success = true;
  stats.timestamp = *flow.finalize_voted;

  validatorsession::ValidatorSessionStats::Round round;
  round.started_at = *flow.candidate_received;
  round.producers = {flow_to_legacy_stats(flow)};

  stats.rounds.push_back(std::move(round));

  recorder_->add(stats.tl());
}

}  // namespace ton::validator::consensus::simplex::stats
