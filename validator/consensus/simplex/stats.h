/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#pragma once

#include <algorithm>
#include <bitset>
#include <map>
#include <optional>
#include <set>

#include "common/stats.h"
#include "consensus/stats.h"
#include "metrics/consensus-metrics.h"

#include "votes.h"

namespace ton::validator::consensus::simplex::stats {

namespace tl {

using voted = ton_api::consensus_simplex_stats_voted;
using certObserved = ton_api::consensus_simplex_stats_certObserved;

}  // namespace tl

class MetricCollector;

class Voted : public consensus::stats::CollectibleEvent<MetricCollector> {
 public:
  static std::unique_ptr<Voted> create(Vote vote);

  consensus::stats::tl::EventRef to_tl() const override;
  std::string to_string() const override;
  void collect_to(MetricCollector& collector) const override;

  const Vote& vote() const {
    return vote_;
  }

 private:
  Voted(Vote vote);

  Vote vote_;
};

class CertObserved : public consensus::stats::CollectibleEvent<MetricCollector> {
 public:
  static std::unique_ptr<CertObserved> create(Vote vote);

  consensus::stats::tl::EventRef to_tl() const override;
  std::string to_string() const override;
  void collect_to(MetricCollector& collector) const override;

  const Vote& vote() const {
    return vote_;
  }

 private:
  CertObserved(Vote vote);

  Vote vote_;
};

// Which identities of a consensus group this reporter speaks for. One group can run a validator
// identity, a dedicated-collator identity and observer identities at the same time, all feeding the
// same chain-level counters, so every observation is owned by exactly one role. A validator collates
// every window it does not delegate and therefore holds both roles, which is what keeps the merged
// single-identity deployment reporting exactly what it reported before roles existed.
struct Roles {
  bool validator = false;
  bool collator = false;
};

// The collator owns the round until the candidate exists locally; the validator owns it from there
// on. Both identities can see certificates and the local apply, so those stages must belong to the
// validator alone or a delegating group would count them twice.
constexpr bool is_collator_stage(metrics::ConsensusStage stage) {
  return stage == metrics::ConsensusStage::collate || stage == metrics::ConsensusStage::publish;
}

struct Flow {
  metrics::ConsensusRoundMarks marks;
  std::bitset<metrics::LabelDomainOf<metrics::ConsensusStage>::size> stages_observed;
  std::bitset<metrics::LabelDomainOf<metrics::ConsensusMark>::size> slot_marks_observed;
  std::optional<double> slot_start;
  // Launch of the attempt that produced this candidate. It differs from marks[collate_start], which
  // stays on the first attempt, only when a failed generation relaunched the same slot.
  std::optional<double> collate_attempt_start;
  std::optional<metrics::ConsensusRoundOutcome> outcome;
  bool round_observed = false;
};

class MetricCollector final : public consensus::stats::MetricCollector {
 public:
  MetricCollector(metrics::BlockChain chain, Roles roles);

  void collect_collate_started(const consensus::stats::CollateStarted& event) override;
  void collect_collate_finished(const consensus::stats::CollateFinished& event) override;
  void collect_collated_empty(const consensus::stats::CollatedEmpty& event) override;
  void collect_candidate_received(const consensus::stats::CandidateReceived& event) override;
  void collect_validation_started(const consensus::stats::ValidationStarted& event) override;
  void collect_validation_finished(const consensus::stats::ValidationFinished& event) override;
  void collect_block_accepted(const consensus::stats::BlockAccepted& event) override;

  void collect_voted(const Voted& event);
  void collect_cert_observed(const CertObserved& event);
  void collect_candidate_outcome(CandidateId id, bool is_empty);

  metrics::ConsensusMetrics take_metrics() {
    return std::exchange(metrics_, {});
  }

 private:
  // A launched collation, awaiting its CollateFinished. A failed generation can relaunch the same
  // slot, so both ends are kept: the first launch is when work on the slot began and owns the slot
  // mark and the handoff, the last one is the attempt that can still win and owns the collate stage.
  // Merging is order-independent, so the same event set always yields the same numbers.
  struct Collation {
    double first_started;
    double last_started;
    double slot_start;  // of the first launch, so a reordered relaunch cannot move slot zero

    void merge(const Collation& other) {
      if (other.first_started < first_started) {
        first_started = other.first_started;
        slot_start = other.slot_start;
      }
      last_started = std::max(last_started, other.last_started);
    }
  };

  static constexpr td::uint32 kSlotRetention = 1024;

  static size_t mark_index(metrics::ConsensusMark mark) {
    return static_cast<size_t>(mark);
  }

  bool owns(metrics::ConsensusStage stage) const {
    return is_collator_stage(stage) ? roles_.collator : roles_.validator;
  }

  bool too_old(td::uint32 slot) const;
  void remember_slot(td::uint32 slot);
  void prune_old_slots();
  void set_mark(Flow& flow, metrics::ConsensusMark mark, double timestamp);
  void observe_ready_stages(Flow& flow);
  void observe_slot_marks(Flow& flow);
  void associate_collation(td::uint32 target_slot, td::uint32 assigned_slot);
  void maybe_complete_round(const CandidateId& id, Flow& flow);
  bool complete_slot(td::uint32 slot, double timestamp, metrics::ConsensusRoundOutcome outcome);
  void record_slot_completion(td::uint32 slot, double timestamp);
  void maybe_observe_handoff(td::uint32 next_slot);

  metrics::BlockChain chain_;
  Roles roles_;

  std::map<CandidateId, Flow> flows_;
  std::map<td::uint32, Collation> collate_started_by_slot_;
  std::map<td::uint32, double> completed_at_by_slot_;
  std::set<td::uint32> completed_slots_;
  std::set<td::uint32> observed_handoffs_;
  std::optional<td::uint32> newest_slot_;

  metrics::ConsensusMetrics metrics_;
};

}  // namespace ton::validator::consensus::simplex::stats
