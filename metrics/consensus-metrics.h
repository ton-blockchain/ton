/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

#include "block-processing-metrics.h"
#include "collectors.h"

namespace ton::metrics {

// One stage per interval between two consecutive round marks (see ConsensusRoundMarks): the
// order here *is* the mark order, so stage i measures marks[i+1] - marks[i].
#define CONSENSUS_STAGE_LIST(F) \
  F(collate)                    \
  F(publish)                    \
  F(validate_wait)              \
  F(validate)                   \
  F(notarize_vote)              \
  F(notarize_cert)              \
  F(finalize_vote)              \
  F(finalize_cert)              \
  F(apply)
TON_METRIC_DEFINE_LABEL(ConsensusStage, "stage", CONSENSUS_STAGE_LIST)
#undef CONSENSUS_STAGE_LIST

// Every event position in the local collator's view of a slot. The order intentionally matches
// ConsensusRoundMarks, so the collector can walk both without a second mapping table: this is the
// index domain of a round's mark array, not the exported label domain.
#define CONSENSUS_MARK_LIST(F) \
  F(collate_start)             \
  F(collate_finish)            \
  F(candidate_received)        \
  F(validation_start)          \
  F(validation_finish)         \
  F(notarize_vote)             \
  F(notarize_cert)             \
  F(finalize_vote)             \
  F(finalize_cert)             \
  F(apply)
TON_METRIC_DEFINE_LABEL(ConsensusMark, "mark", CONSENSUS_MARK_LIST)
#undef CONSENSUS_MARK_LIST

// The marks whose absolute position is exported as paired non-negative histograms against slot zero:
// the timeline and finality panels read exactly these four, and each one costs 2 chains * 2 sides of
// histogram. The intervals around the other six stay visible as stages, which are far cheaper.
//
// All four are observed by the identity that holds slot zero, which is what keeps them one
// population: every collated round contributes all four, whoever collated it. validation_finish is
// deliberately not here — it needs an identity that both holds slot zero and validates, so a
// delegated round contributes the four and not it. Pooled over a fleet that mixes self-collating
// validators and dedicated collators, it would be a median of a strict subset drawn as one more
// point on a path the other four measure over everything.
#define CONSENSUS_EXPORTED_MARK_LIST(F) \
  F(collate_start)                      \
  F(collate_finish)                     \
  F(finalize_cert)                      \
  F(apply)
TON_METRIC_DEFINE_LABEL(ConsensusExportedMark, "mark", CONSENSUS_EXPORTED_MARK_LIST)

// Unexported marks are dropped here, not at the fold site: the consensus collector walks a round's
// whole mark array and has no business knowing which positions we publish.
inline constexpr std::optional<ConsensusExportedMark> exported_consensus_mark(ConsensusMark mark) {
#define TON_EXPORTED_MARK_CASE_(name) \
  case ConsensusMark::name:           \
    return ConsensusExportedMark::name;
  switch (mark) {
    CONSENSUS_EXPORTED_MARK_LIST(TON_EXPORTED_MARK_CASE_)
    default:
      return std::nullopt;
  }
#undef TON_EXPORTED_MARK_CASE_
}
#undef CONSENSUS_EXPORTED_MARK_LIST

#define SLOT_SIDE_LIST(F) \
  F(before)               \
  F(after)
TON_METRIC_DEFINE_LABEL(SlotSide, "side", SLOT_SIDE_LIST)
#undef SLOT_SIDE_LIST

// How a slot ended: a candidate carrying a block was accepted, an empty candidate was accepted,
// or the network certified a skip.
#define CONSENSUS_ROUND_OUTCOME_LIST(F) \
  F(accepted)                           \
  F(empty)                              \
  F(skipped)
TON_METRIC_DEFINE_LABEL(ConsensusRoundOutcome, "outcome", CONSENSUS_ROUND_OUTCOME_LIST)
#undef CONSENSUS_ROUND_OUTCOME_LIST

// collate started/finished, candidate received, validation started/finished, notarize voted,
// notarize cert observed, finalize voted, finalize cert observed, manager accepted. A mark this
// node never saw stays empty, and both stages touching it are simply not observed.
using ConsensusRoundMarks = std::array<std::optional<double>, LabelDomainOf<ConsensusStage>::size + 1>;
static_assert(ConsensusRoundMarks{}.size() == LabelDomainOf<ConsensusMark>::size);

// Metrics marks use one process-local steady clock. An hour is far past any real stage; cap finite
// outliers there as a corruption guard while retaining enough range for a genuinely long stall.
inline constexpr double kMaxConsensusSeconds = 3600.0;
// Several consensus histograms intentionally observe zero (the opposite handoff/slot-mark side and
// a reordered stage). A literal zero boundary prevents Prometheus from interpolating an all-zero
// quantile inside the first positive bucket and drawing phantom latency.
//
// Two regimes have to stay readable at p50/p95 with one bucket set: sub-slot positions and stages of
// a few milliseconds up to a sub-second slot, and the seconds-to-minutes tail of a starved node. Ten
// positive bounds cover both at roughly 4x steps; beyond the last one a quantile only reports "over
// two minutes" and the exact retained maxima are what to read instead.
inline constexpr std::array<double, 11> kConsensusDurationBuckets = {0.0, 0.001, 0.005, 0.025, 0.1,  0.25,
                                                                     0.5, 1.0,   5.0,   30.0,  120.0};

// Per-round timings, folded at events consensus already fires. A validator group accumulates a
// delta and hands it to the validator manager, which owns the process-lifetime total: groups are
// created and destroyed constantly, counters must not restart with them.
class ConsensusMetrics {
 public:
  void observe_round(BlockChain chain, const ConsensusRoundMarks &marks) {
    for (size_t stage = 0; stage + 1 < marks.size(); ++stage) {
      if (marks[stage] && marks[stage + 1]) {
        observe_stage(chain, static_cast<ConsensusStage>(stage), *marks[stage + 1] - *marks[stage]);
      }
    }
  }

  // A node can learn of a certificate before it votes itself, so a stage can legitimately run
  // backwards; that is zero waiting, not a negative one.
  void observe_stage(BlockChain chain, ConsensusStage stage, double seconds) {
    if (std::isfinite(seconds)) {
      seconds = std::clamp(seconds, 0.0, kMaxConsensusSeconds);
      stage_.at(chain, stage).observe(seconds);
      stage_recent_max_.at(chain, stage).observe(seconds);
    }
  }

  // The two sides of one handoff: dead time after the prior slot completed, head start before it.
  // Completion is local manager apply for a full block and the certificate for empty/skip rounds.
  // Every handoff observes both, and exactly one of them is zero.
  void observe_slot_gap(BlockChain chain, double seconds) {
    if (std::isfinite(seconds)) {
      seconds = std::clamp(seconds, 0.0, kMaxConsensusSeconds);
      slot_gap_.at(chain).observe(seconds);
      slot_gap_recent_max_.at(chain).observe(seconds);
    }
  }

  void observe_slot_lead(BlockChain chain, double seconds) {
    if (std::isfinite(seconds)) {
      seconds = std::clamp(seconds, 0.0, kMaxConsensusSeconds);
      slot_lead_.at(chain).observe(seconds);
      slot_lead_recent_max_.at(chain).observe(seconds);
    }
  }

  // Place one exported mark against slot zero without putting negative values into a Prometheus
  // histogram; a mark nothing reads is accepted and ignored.
  // Both sides receive an observation and exactly one side is zero. A signed mean is therefore
  // mean(side="after") - mean(side="before"). The same subtraction is valid for p50 because the
  // populations are identical and only one side can have a positive median; it is not valid for
  // higher quantiles. Each side also keeps its own zero-padded tail and exact recent maximum.
  void observe_slot_mark(BlockChain chain, ConsensusMark mark, double offset_seconds) {
    auto exported = exported_consensus_mark(mark);
    if (!exported || !std::isfinite(offset_seconds)) {
      return;
    }
    offset_seconds = std::clamp(offset_seconds, -kMaxConsensusSeconds, kMaxConsensusSeconds);
    const double before = std::max(-offset_seconds, 0.0);
    const double after = std::max(offset_seconds, 0.0);
    slot_mark_.at(chain, *exported, SlotSide::before).observe(before);
    slot_mark_.at(chain, *exported, SlotSide::after).observe(after);
    slot_mark_recent_max_.at(chain, *exported, SlotSide::before).observe(before);
    slot_mark_recent_max_.at(chain, *exported, SlotSide::after).observe(after);
  }

  void add_round(BlockChain chain, ConsensusRoundOutcome outcome) {
    rounds_.at(chain, outcome).inc();
  }

  ConsensusMetrics &operator+=(const ConsensusMetrics &other) {
    stage_ += other.stage_;
    slot_gap_ += other.slot_gap_;
    slot_lead_ += other.slot_lead_;
    slot_gap_recent_max_ += other.slot_gap_recent_max_;
    slot_lead_recent_max_ += other.slot_lead_recent_max_;
    slot_mark_ += other.slot_mark_;
    slot_mark_recent_max_ += other.slot_mark_recent_max_;
    stage_recent_max_ += other.stage_recent_max_;
    rounds_ += other.rounds_;
    return *this;
  }

  void collect(Context ctx) const {
    auto consensus = ctx.with_name("consensus");
    consensus.collect(stage_, "stage_seconds");
    consensus.collect(slot_gap_, "slot_gap_seconds");
    consensus.collect(slot_lead_, "slot_lead_seconds");
    consensus.collect(slot_gap_recent_max_, "slot_gap_recent_max_seconds");
    consensus.collect(slot_lead_recent_max_, "slot_lead_recent_max_seconds");
    consensus.collect(slot_mark_, "collator_slot_mark_seconds");
    consensus.collect(slot_mark_recent_max_, "collator_slot_mark_recent_max_seconds");
    consensus.collect(stage_recent_max_, "stage_recent_max_seconds");
    consensus.collect(rounds_, "rounds");
  }

 private:
  Labeled<Histogram<kConsensusDurationBuckets>, BlockChain, ConsensusStage> stage_;
  Labeled<Histogram<kConsensusDurationBuckets>, BlockChain> slot_gap_;
  Labeled<Histogram<kConsensusDurationBuckets>, BlockChain> slot_lead_;
  Labeled<RecentMax<>, BlockChain> slot_gap_recent_max_;
  Labeled<RecentMax<>, BlockChain> slot_lead_recent_max_;
  Labeled<Histogram<kConsensusDurationBuckets>, BlockChain, ConsensusExportedMark, SlotSide> slot_mark_;
  Labeled<RecentMax<>, BlockChain, ConsensusExportedMark, SlotSide> slot_mark_recent_max_;
  Labeled<RecentMax<>, BlockChain, ConsensusStage> stage_recent_max_;
  Labeled<Counter, BlockChain, ConsensusRoundOutcome> rounds_;
};

}  // namespace ton::metrics
