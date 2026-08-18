/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "consensus/simplex/stats.h"
#include "metrics/collectors.h"
#include "td/utils/port/sleep.h"
#include "td/utils/tests.h"

namespace ton::validator::consensus::simplex::stats::test {
namespace {

// Today's common deployment: one identity validates and collates its own windows, so it owns every
// stage and must keep reporting exactly what the collector reported before roles existed.
constexpr Roles kMerged{.validator = true, .collator = true};
constexpr Roles kValidatorOnly{.validator = true, .collator = false};
constexpr Roles kCollatorOnly{.validator = false, .collator = true};

std::string render(metrics::ConsensusMetrics value) {
  metrics::Sink sink;
  metrics::Context(sink).collect(value, "ton");
  return std::move(sink).build().render();
}

bool has_line(const std::string& output, const std::string& line) {
  return output.find(line + '\n') != std::string::npos;
}

// Every candidate below is led by this identity, the way a leader window's candidates are.
const PeerValidatorId kLocal{0};

// A window this validator delegated: the certificate it signed for the collator travels with the
// candidate. Only its presence matters here — nothing on the metrics path inspects it.
std::optional<Delegation> delegation_if(bool delegated) {
  if (!delegated) {
    return std::nullopt;
  }
  return Delegation{PublicKey{pubkeys::Ed25519{td::Bits256::zero()}}, td::BufferSlice{}};
}

CandidateId candidate_id(td::uint32 slot) {
  return CandidateId{slot, td::Bits256::zero()};
}

CandidateRef empty_candidate(td::uint32 slot, bool delegated = false) {
  CHECK(slot > 0);
  auto id = candidate_id(slot);
  ParentId parent = candidate_id(slot - 1);
  std::variant<BlockIdExt, BlockCandidate> block{BlockIdExt{}};
  return td::make_ref<Candidate>(id, parent, kLocal, std::move(block), td::BufferSlice{}, delegation_if(delegated));
}

CandidateRef full_candidate(td::uint32 slot, bool delegated = false) {
  auto id = candidate_id(slot);
  ParentId parent = slot == 0 ? std::nullopt : ParentId{candidate_id(slot - 1)};
  BlockCandidate candidate;
  candidate.id = BlockIdExt{};
  std::variant<BlockIdExt, BlockCandidate> block{std::move(candidate)};
  return td::make_ref<Candidate>(id, parent, kLocal, std::move(block), td::BufferSlice{}, delegation_if(delegated));
}

// The rule simplex/consensus.cpp applies when a candidate reaches the slot state: trace whatever
// this node did not itself collate (`candidate->collated_by(bus) != bus.local_adnl_id`). Every
// candidate in this fixture is led by the local identity, so that reduces to "delegated": the
// window ran on the collator's bus and nothing else on this node ever traces it, while a
// self-produced candidate was already traced by the producer when it published it.
std::unique_ptr<consensus::stats::CandidateReceived> traced_on_receipt(const CandidateRef& candidate) {
  if (!candidate->delegation.has_value()) {
    return nullptr;
  }
  return consensus::stats::CandidateReceived::create(candidate, /* is_collator = */ false);
}

void collect_candidate(MetricCollector& collector, const CandidateRef& candidate, bool is_collator = false) {
  auto event = consensus::stats::CandidateReceived::create(candidate, is_collator);
  collector.collect_candidate_received(*event);
  collector.collect_candidate_outcome(candidate->id, candidate->is_empty());
}

void collect_outcome(MetricCollector& collector, const CandidateRef& candidate) {
  collector.collect_candidate_outcome(candidate->id, candidate->is_empty());
}

void collect_finalize(MetricCollector& collector, CandidateId id) {
  auto event = CertObserved::create(FinalizeVote{id});
  collector.collect_cert_observed(*event);
}

void collect_skip(MetricCollector& collector, td::uint32 slot) {
  auto event = CertObserved::create(SkipVote{slot});
  collector.collect_cert_observed(*event);
}

TEST(ConsensusMetricCollector, FinalCertificateCompletesEmptyRoundWithoutBlockAccepted) {
  MetricCollector collector(metrics::BlockChain::shard, kMerged);
  auto candidate = empty_candidate(7);
  collect_candidate(collector, candidate);
  collect_finalize(collector, candidate->id);
  collect_finalize(collector, candidate->id);  // duplicate certificate

  auto output = render(collector.take_metrics());
  ASSERT_TRUE(has_line(output, "ton_consensus_rounds_total{chain=\"shard\",outcome=\"empty\"} 1.000000"));
  ASSERT_TRUE(has_line(output, "ton_consensus_rounds_total{chain=\"shard\",outcome=\"accepted\"} 0.000000"));
  ASSERT_TRUE(has_line(output, "ton_consensus_stage_seconds_count{chain=\"shard\",stage=\"apply\"} 0.000000"));
}

TEST(ConsensusMetricCollector, ReorderedFinalCertificateAndApplyAreCountedExactlyOnce) {
  MetricCollector collector(metrics::BlockChain::master, kMerged);
  auto candidate = full_candidate(9);

  collect_finalize(collector, candidate->id);  // certificate can reach this subscriber first
  collect_candidate(collector, candidate);
  auto accepted = consensus::stats::BlockAccepted::create(candidate->id);
  collector.collect_block_accepted(*accepted);
  collector.collect_block_accepted(*accepted);  // duplicate manager notification

  auto output = render(collector.take_metrics());
  ASSERT_TRUE(has_line(output, "ton_consensus_rounds_total{chain=\"master\",outcome=\"accepted\"} 1.000000"));
  ASSERT_TRUE(has_line(output, "ton_consensus_stage_seconds_count{chain=\"master\",stage=\"apply\"} 1.000000"));
}

TEST(ConsensusMetricCollector, OutcomeAfterFinalityAndApplyCompletesAcceptedRoundExactlyOnce) {
  MetricCollector collector(metrics::BlockChain::master, kMerged);
  auto candidate = full_candidate(12);

  collect_finalize(collector, candidate->id);
  auto accepted = consensus::stats::BlockAccepted::create(candidate->id);
  collector.collect_block_accepted(*accepted);
  collector.collect_block_accepted(*accepted);

  // Timing is retained across a reporter flush, but without the authoritative candidate-kind event
  // neither the outcome nor any stage is emitted yet.
  auto before_outcome = render(collector.take_metrics());
  ASSERT_TRUE(has_line(before_outcome, "ton_consensus_rounds_total{chain=\"master\",outcome=\"accepted\"} 0.000000"));
  ASSERT_TRUE(has_line(before_outcome, "ton_consensus_stage_seconds_count{chain=\"master\",stage=\"apply\"} 0.000000"));

  collect_outcome(collector, candidate);
  collect_outcome(collector, candidate);

  auto output = render(collector.take_metrics());
  ASSERT_TRUE(has_line(output, "ton_consensus_rounds_total{chain=\"master\",outcome=\"accepted\"} 1.000000"));
  ASSERT_TRUE(has_line(output, "ton_consensus_stage_seconds_count{chain=\"master\",stage=\"apply\"} 1.000000"));

  collector.collect_block_accepted(*accepted);
  collect_outcome(collector, candidate);
  auto duplicates = render(collector.take_metrics());
  ASSERT_TRUE(has_line(duplicates, "ton_consensus_rounds_total{chain=\"master\",outcome=\"accepted\"} 0.000000"));
  ASSERT_TRUE(has_line(duplicates, "ton_consensus_stage_seconds_count{chain=\"master\",stage=\"apply\"} 0.000000"));
}

TEST(ConsensusMetricCollector, ReceiptAndApplyTimingDoNotInventRoundOutcomes) {
  MetricCollector received_only(metrics::BlockChain::shard, kValidatorOnly);
  auto empty = empty_candidate(13);
  auto received = consensus::stats::CandidateReceived::create(empty, false);
  received_only.collect_candidate_received(*received);
  collect_finalize(received_only, empty->id);

  auto received_output = render(received_only.take_metrics());
  for (auto outcome : {"empty", "accepted", "skipped"}) {
    ASSERT_TRUE(has_line(received_output, PSTRING() << "ton_consensus_rounds_total{chain=\"shard\",outcome=\""
                                                    << outcome << "\"} 0.000000"));
  }

  MetricCollector applied_only(metrics::BlockChain::master, kValidatorOnly);
  auto full = full_candidate(14);
  collect_finalize(applied_only, full->id);
  auto accepted = consensus::stats::BlockAccepted::create(full->id);
  applied_only.collect_block_accepted(*accepted);

  auto applied_output = render(applied_only.take_metrics());
  ASSERT_TRUE(has_line(applied_output, "ton_consensus_rounds_total{chain=\"master\",outcome=\"accepted\"} 0.000000"));
  ASSERT_TRUE(has_line(applied_output, "ton_consensus_stage_seconds_count{chain=\"master\",stage=\"apply\"} 0.000000"));
}

TEST(ConsensusMetricCollector, CandidateOutcomeIsIdempotentBeforeOrAfterFinalityAndAddsNoTiming) {
  auto candidate = empty_candidate(15);

  auto replay = [&](bool outcome_first) {
    MetricCollector collector(metrics::BlockChain::shard, kValidatorOnly);
    if (outcome_first) {
      collect_outcome(collector, candidate);
      collect_outcome(collector, candidate);
      collect_finalize(collector, candidate->id);
    } else {
      collect_finalize(collector, candidate->id);
      collect_outcome(collector, candidate);
      collect_outcome(collector, candidate);
    }
    return render(collector.take_metrics());
  };

  auto output = replay(true);
  ASSERT_EQ(output, replay(false));
  ASSERT_TRUE(has_line(output, "ton_consensus_rounds_total{chain=\"shard\",outcome=\"empty\"} 1.000000"));
  for (auto stage : {"collate", "publish", "validate_wait", "validate", "notarize_vote", "notarize_cert",
                     "finalize_vote", "finalize_cert", "apply"}) {
    ASSERT_TRUE(has_line(
        output, PSTRING() << "ton_consensus_stage_seconds_count{chain=\"shard\",stage=\"" << stage << "\"} 0.000000"));
  }
}

TEST(ConsensusMetricCollector, RecoveredEmptyOutcomeKeepsFinalityAsTheHandoffBoundary) {
  MetricCollector collector(metrics::BlockChain::shard, kMerged);
  auto candidate = empty_candidate(16);

  collect_finalize(collector, candidate->id);
  td::usleep_for(10'000);
  auto next_started = consensus::stats::CollateStarted::create(17, td::Timestamp::now());
  collector.collect_collate_started(*next_started);
  td::usleep_for(10'000);

  // CandidateResolver learns the kind only after the next collation is underway. The counter lands
  // now, but the completed-slot timestamp must remain the earlier final certificate.
  collect_outcome(collector, candidate);
  auto output = render(collector.take_metrics());

  ASSERT_TRUE(has_line(output, "ton_consensus_rounds_total{chain=\"shard\",outcome=\"empty\"} 1.000000"));
  ASSERT_TRUE(has_line(output, "ton_consensus_slot_gap_seconds_bucket{chain=\"shard\",le=\"0\"} 0.000000"));
  ASSERT_TRUE(has_line(output, "ton_consensus_slot_lead_seconds_bucket{chain=\"shard\",le=\"0\"} 1.000000"));
}

TEST(ConsensusMetricCollector, StagesArrivingAfterFinalityAndFlushAreCountedExactlyOnce) {
  MetricCollector collector(metrics::BlockChain::shard, kMerged);
  auto candidate = full_candidate(10);
  collect_candidate(collector, candidate);

  auto validation_started = consensus::stats::ValidationStarted::create(candidate->id);
  collector.collect_validation_started(*validation_started);
  collect_finalize(collector, candidate->id);

  // Finality closes the outcome, and the reporter may flush before this node's slower local path
  // finishes. The already-ready stage must not be replayed into the next delta.
  auto first_delta = render(collector.take_metrics());
  ASSERT_TRUE(
      has_line(first_delta, "ton_consensus_stage_seconds_count{chain=\"shard\",stage=\"validate_wait\"} 1.000000"));
  ASSERT_TRUE(has_line(first_delta, "ton_consensus_stage_seconds_count{chain=\"shard\",stage=\"validate\"} 0.000000"));

  auto validation_finished = consensus::stats::ValidationFinished::create(candidate->id);
  collector.collect_validation_finished(*validation_finished);
  auto notarize_vote = Voted::create(NotarizeVote{candidate->id});
  collector.collect_voted(*notarize_vote);
  auto notarize_cert = CertObserved::create(NotarizeVote{candidate->id});
  collector.collect_cert_observed(*notarize_cert);
  auto finalize_vote = Voted::create(FinalizeVote{candidate->id});
  collector.collect_voted(*finalize_vote);
  auto accepted = consensus::stats::BlockAccepted::create(candidate->id);
  collector.collect_block_accepted(*accepted);

  // Duplicate delivery must neither move a first-seen mark nor repeat a stage.
  collector.collect_validation_finished(*validation_finished);
  collector.collect_voted(*notarize_vote);
  collector.collect_cert_observed(*notarize_cert);
  collector.collect_voted(*finalize_vote);
  collector.collect_block_accepted(*accepted);

  auto late_delta = render(collector.take_metrics());
  ASSERT_TRUE(
      has_line(late_delta, "ton_consensus_stage_seconds_count{chain=\"shard\",stage=\"validate_wait\"} 0.000000"));
  for (auto stage : {"validate", "notarize_vote", "notarize_cert", "finalize_vote", "finalize_cert", "apply"}) {
    ASSERT_TRUE(has_line(late_delta, PSTRING() << "ton_consensus_stage_seconds_count{chain=\"shard\",stage=\"" << stage
                                               << "\"} 1.000000"));
  }
}

TEST(ConsensusMetricCollector, SkipCertificatesAreDeduplicatedAndOldDuplicatesStayPruned) {
  MetricCollector collector(metrics::BlockChain::shard, kMerged);
  for (td::uint32 slot = 0; slot <= 1100; ++slot) {
    collect_skip(collector, slot);
  }
  collect_skip(collector, 0);
  collect_skip(collector, 1100);

  auto output = render(collector.take_metrics());
  ASSERT_TRUE(has_line(output, "ton_consensus_rounds_total{chain=\"shard\",outcome=\"skipped\"} 1101.000000"));
}

TEST(ConsensusMetricCollector, SlowCollationKeepsLaunchSlotAndUsesAssignedSlotZero) {
  MetricCollector collector(metrics::BlockChain::shard, kMerged);
  auto started = consensus::stats::CollateStarted::create(10, td::Timestamp::now());
  const double assigned_slot_start = started->monotonic_ts() + 0.5;
  collector.collect_collate_started(*started);

  // Slot 10 completes while the same collation future is still running. The finish event below
  // assigns that future to slot 11 and must retroactively close the 10 -> 11 handoff.
  auto empty = empty_candidate(10);
  collect_candidate(collector, empty);
  collect_finalize(collector, empty->id);

  auto id = candidate_id(11);
  auto finished = consensus::stats::CollateFinished::create(10, td::Timestamp::at(assigned_slot_start), id,
                                                            started->monotonic_ts() + 0.25);
  collector.collect_collate_finished(*finished);
  auto candidate = full_candidate(11);
  collect_candidate(collector, candidate);
  collect_finalize(collector, candidate->id);

  auto output = render(collector.take_metrics());
  ASSERT_TRUE(has_line(output,
                       "ton_consensus_collator_slot_mark_seconds_sum{chain=\"shard\",mark=\"collate_start\","
                       "side=\"before\"} 0.500000"));
  ASSERT_TRUE(has_line(output,
                       "ton_consensus_collator_slot_mark_seconds_sum{chain=\"shard\",mark=\"collate_finish\","
                       "side=\"before\"} 0.250000"));
  ASSERT_TRUE(has_line(output,
                       "ton_consensus_collator_slot_mark_seconds_count{chain=\"shard\",mark=\"collate_finish\","
                       "side=\"after\"} 1.000000"));
  ASSERT_TRUE(has_line(output, "ton_consensus_slot_gap_seconds_count{chain=\"shard\"} 1.000000"));
  ASSERT_TRUE(has_line(output, "ton_consensus_slot_lead_seconds_count{chain=\"shard\"} 1.000000"));
}

TEST(ConsensusMetricCollector, SameSlotRestartUsesLatestLaunchAndPreservesWireSlot) {
  MetricCollector collector(metrics::BlockChain::shard, kMerged);
  auto abandoned = consensus::stats::CollateStarted::create(30, td::Timestamp::now());
  ASSERT_EQ(abandoned->to_string(), "CollateStarted{target_slot=30}");
  collector.collect_collate_started(*abandoned);
  td::usleep_for(10'000);

  auto restarted = consensus::stats::CollateStarted::create(30, td::Timestamp::now());
  collector.collect_collate_started(*restarted);

  auto id = candidate_id(31);
  auto finished = consensus::stats::CollateFinished::create(30, td::Timestamp::at(restarted->monotonic_ts() + 0.5), id,
                                                            restarted->monotonic_ts() + 0.1);

  // Launch-slot tracking is private telemetry. The existing trace field must retain its legacy
  // meaning: the slot to which the candidate was assigned, not the earlier launch slot.
  auto wire_event = finished->to_tl();
  auto* wire_finished = static_cast<consensus::stats::tl::collateFinished*>(wire_event.get());
  ASSERT_EQ(wire_finished->target_slot_, 31);
  ASSERT_EQ(finished->to_string(), PSTRING() << "CollateFinished{target_slot=31, id=" << id << "}");

  collector.collect_collate_finished(*finished);
  auto candidate = full_candidate(31);
  collect_candidate(collector, candidate);
  collect_finalize(collector, candidate->id);

  auto output = render(collector.take_metrics());
  ASSERT_TRUE(has_line(output, "ton_consensus_stage_seconds_sum{chain=\"shard\",stage=\"collate\"} 0.100000"));
}

TEST(ConsensusMetricCollector, NeverFinishingCollationStillExposesCarriedSlotHandoff) {
  MetricCollector collector(metrics::BlockChain::shard, kMerged);
  auto started = consensus::stats::CollateStarted::create(20, td::Timestamp::now());
  collector.collect_collate_started(*started);

  auto slot_20 = empty_candidate(20);
  auto timeout_20 = consensus::stats::CollatedEmpty::create(20, slot_20->id);
  collector.collect_collated_empty(*timeout_20);
  collect_candidate(collector, slot_20);
  collect_finalize(collector, slot_20->id);

  // The future times out again in slot 21 and never emits CollateFinished. The timeout itself must
  // associate the original start with slot 21 so the 20 -> 21 handoff is not lost.
  auto slot_21 = empty_candidate(21);
  auto timeout_21 = consensus::stats::CollatedEmpty::create(20, slot_21->id);
  ASSERT_EQ(timeout_21->to_string(), PSTRING() << "CollatedEmpty{id=" << slot_21->id << "}");
  collector.collect_collated_empty(*timeout_21);

  auto output = render(collector.take_metrics());
  ASSERT_TRUE(has_line(output, "ton_consensus_slot_gap_seconds_count{chain=\"shard\"} 1.000000"));
  ASSERT_TRUE(has_line(output, "ton_consensus_slot_lead_seconds_count{chain=\"shard\"} 1.000000"));
}

TEST(ConsensusMetricCollector, HandoffPairsOnlyConsecutiveSlots) {
  MetricCollector collector(metrics::BlockChain::master, kMerged);
  auto start_11 = consensus::stats::CollateStarted::create(11, td::Timestamp::now());
  auto start_12 = consensus::stats::CollateStarted::create(12, td::Timestamp::now());
  collector.collect_collate_started(*start_11);
  collector.collect_collate_started(*start_12);

  auto candidate = full_candidate(10);
  collect_candidate(collector, candidate);
  collect_finalize(collector, candidate->id);

  // A full block's handoff boundary is local manager apply, not the earlier final certificate;
  // otherwise slow storage/fsync disappears from the gap.
  auto before_apply = render(collector.take_metrics());
  ASSERT_TRUE(has_line(before_apply, "ton_consensus_slot_lead_seconds_count{chain=\"master\"} 0.000000"));
  ASSERT_TRUE(has_line(before_apply, "ton_consensus_slot_gap_seconds_count{chain=\"master\"} 0.000000"));

  auto accepted = consensus::stats::BlockAccepted::create(candidate->id);
  collector.collect_block_accepted(*accepted);

  auto output = render(collector.take_metrics());
  ASSERT_TRUE(has_line(output, "ton_consensus_slot_lead_seconds_count{chain=\"master\"} 1.000000"));
  ASSERT_TRUE(has_line(output, "ton_consensus_slot_gap_seconds_count{chain=\"master\"} 1.000000"));
}

TEST(ConsensusMetricCollector, SplitRolesPartitionWhatOneMergedIdentityReports) {
  auto slot_start = td::Timestamp::now();
  auto candidate = full_candidate(5, /* delegated = */ true);
  auto started = consensus::stats::CollateStarted::create(5, slot_start);
  auto finished =
      consensus::stats::CollateFinished::create(5, slot_start, candidate->id, started->monotonic_ts() + 0.05);
  // The delegated validator has this event because the candidate carries a delegation: its leader is
  // that validator, but the window ran on the collator's bus, so the receipt is where the event is
  // published. The collator's own copy comes from its producer and differs only in the is_collator
  // flag, which no collector reads — so one instant is replayed into every stream below, which is
  // what makes the partition arithmetic rather than approximate.
  auto received = traced_on_receipt(candidate);
  ASSERT_TRUE(received != nullptr);
  auto validation_started = consensus::stats::ValidationStarted::create(candidate->id);
  auto validation_finished = consensus::stats::ValidationFinished::create(candidate->id);
  auto notarize_vote = Voted::create(NotarizeVote{candidate->id});
  auto notarize_cert = CertObserved::create(NotarizeVote{candidate->id});
  auto finalize_vote = Voted::create(FinalizeVote{candidate->id});
  auto finalize_cert = CertObserved::create(FinalizeVote{candidate->id});
  auto accepted = consensus::stats::BlockAccepted::create(candidate->id);

  auto replay = [&](Roles roles, const auto& stream) {
    MetricCollector collector(metrics::BlockChain::shard, roles);
    stream(collector);
    return collector.take_metrics();
  };

  // What each identity of a delegating group actually sees. The dedicated collator launches and
  // finishes the collation and publishes the candidate, then observes the certificates and applies
  // the finalized block like every other identity — it never validates and never votes. The
  // delegated validator learns of the candidate over the network, validates, votes, and sees the
  // same certificates and the same local apply — it sees no collation event at all.
  auto collator_only = replay(kCollatorOnly, [&](MetricCollector& collector) {
    collector.collect_collate_started(*started);
    collector.collect_collate_finished(*finished);
    collector.collect_candidate_received(*received);
    collect_outcome(collector, candidate);
    collector.collect_cert_observed(*notarize_cert);
    collector.collect_cert_observed(*finalize_cert);
    collector.collect_block_accepted(*accepted);
  });
  auto validator_only = replay(kValidatorOnly, [&](MetricCollector& collector) {
    collector.collect_candidate_received(*received);
    collect_outcome(collector, candidate);
    collector.collect_validation_started(*validation_started);
    collector.collect_validation_finished(*validation_finished);
    collector.collect_voted(*notarize_vote);
    collector.collect_cert_observed(*notarize_cert);
    collector.collect_voted(*finalize_vote);
    collector.collect_cert_observed(*finalize_cert);
    collector.collect_block_accepted(*accepted);
  });
  // The merged identity is one node doing both jobs, so it sees the union of those two streams.
  auto merged = replay(kMerged, [&](MetricCollector& collector) {
    collector.collect_collate_started(*started);
    collector.collect_collate_finished(*finished);
    collector.collect_candidate_received(*received);
    collect_outcome(collector, candidate);
    collector.collect_validation_started(*validation_started);
    collector.collect_validation_finished(*validation_finished);
    collector.collect_voted(*notarize_vote);
    collector.collect_cert_observed(*notarize_cert);
    collector.collect_voted(*finalize_vote);
    collector.collect_cert_observed(*finalize_cert);
    collector.collect_block_accepted(*accepted);
  });

  metrics::ConsensusMetrics split;
  split += validator_only;
  split += collator_only;
  // The two identities partition exactly what one merged identity counts alone, with each role's own
  // visibility — every family, with nothing masked out of the comparison. The delegating deployment
  // no longer misses a single exported series, because the one mark it could not place is no longer
  // exported by anyone.
  auto merged_out = render(merged);
  ASSERT_EQ(merged_out, render(split));

  auto validator_out = render(validator_only);
  ASSERT_TRUE(has_line(validator_out, "ton_consensus_rounds_total{chain=\"shard\",outcome=\"accepted\"} 1.000000"));
  for (auto stage :
       {"validate_wait", "validate", "notarize_vote", "notarize_cert", "finalize_vote", "finalize_cert", "apply"}) {
    ASSERT_TRUE(has_line(validator_out, PSTRING() << "ton_consensus_stage_seconds_count{chain=\"shard\",stage=\""
                                                  << stage << "\"} 1.000000"));
  }
  for (auto stage : {"collate", "publish"}) {
    ASSERT_TRUE(has_line(validator_out, PSTRING() << "ton_consensus_stage_seconds_count{chain=\"shard\",stage=\""
                                                  << stage << "\"} 0.000000"));
  }
  ASSERT_TRUE(has_line(validator_out,
                       "ton_consensus_collator_slot_mark_seconds_count{chain=\"shard\",mark=\"collate_start\","
                       "side=\"after\"} 0.000000"));

  auto collator_out = render(collator_only);
  ASSERT_TRUE(has_line(collator_out, "ton_consensus_rounds_total{chain=\"shard\",outcome=\"accepted\"} 0.000000"));
  ASSERT_TRUE(has_line(collator_out, "ton_consensus_stage_seconds_sum{chain=\"shard\",stage=\"collate\"} 0.050000"));
  ASSERT_TRUE(has_line(collator_out, "ton_consensus_stage_seconds_count{chain=\"shard\",stage=\"publish\"} 1.000000"));
  // The apply it sees belongs to the validator: every identity applies the block, so counting it
  // here would multiply the stage by the number of identities the node runs.
  ASSERT_TRUE(has_line(collator_out, "ton_consensus_stage_seconds_count{chain=\"shard\",stage=\"apply\"} 0.000000"));
  auto slot_mark_count = [](const char* mark, const char* side, const char* count) -> std::string {
    return PSTRING() << "ton_consensus_collator_slot_mark_seconds_count{chain=\"shard\",mark=\"" << mark << "\",side=\""
                     << side << "\"} " << count;
  };
  for (auto mark : {"collate_start", "collate_finish", "finalize_cert", "apply"}) {
    ASSERT_TRUE(has_line(collator_out, slot_mark_count(mark, "after", "1.000000")));
  }
  // The four exported marks are exactly the ones the identity holding slot zero observes itself, so
  // a delegated round places all of them. Validation is not on that path — it needs an identity that
  // both holds slot zero and validates — so no role exports it, not even the merged one that could
  // have placed it. The mark itself is still recorded, and still reaches trace and logs.
  for (const auto* out : {&merged_out, &validator_out, &collator_out}) {
    ASSERT_TRUE(out->find("mark=\"validation_finish\"") == std::string::npos);
  }
}

TEST(ConsensusMetricCollector, DelegatedEmptyRoundIsCountedByTheDelegatingValidator) {
  auto candidate = empty_candidate(7, /* delegated = */ true);
  auto received = traced_on_receipt(candidate);
  ASSERT_TRUE(received != nullptr);
  // A candidate the validator produced for itself is traced by its own producer instead, so tracing
  // it again on receipt would only duplicate it.
  ASSERT_TRUE(traced_on_receipt(empty_candidate(7)) == nullptr);

  MetricCollector collector(metrics::BlockChain::shard, kValidatorOnly);
  collector.collect_candidate_received(*received);
  collect_outcome(collector, candidate);
  collect_finalize(collector, candidate->id);

  auto output = render(collector.take_metrics());
  ASSERT_TRUE(has_line(output, "ton_consensus_rounds_total{chain=\"shard\",outcome=\"empty\"} 1.000000"));

  // A validator that missed the receipt still learns the kind through CandidateResolver. The
  // outcome is counted, but recovery must not fabricate candidate-receipt or validation timing.
  MetricCollector recovered(metrics::BlockChain::shard, kValidatorOnly);
  collect_finalize(recovered, candidate->id);
  collect_outcome(recovered, candidate);
  auto recovered_out = render(recovered.take_metrics());
  ASSERT_TRUE(has_line(recovered_out, "ton_consensus_rounds_total{chain=\"shard\",outcome=\"empty\"} 1.000000"));
  ASSERT_TRUE(
      has_line(recovered_out, "ton_consensus_stage_seconds_count{chain=\"shard\",stage=\"validate_wait\"} 0.000000"));
}

TEST(ConsensusMetricCollector, SameSlotRetryHandoffIsIndependentOfArrivalOrder) {
  auto previous_completed = CertObserved::create(SkipVote{29});
  td::usleep_for(2'000);
  auto first = consensus::stats::CollateStarted::create(30, td::Timestamp::now());
  td::usleep_for(300'000);
  auto retry = consensus::stats::CollateStarted::create(30, td::Timestamp::now());

  auto candidate = full_candidate(30);
  auto finished = consensus::stats::CollateFinished::create(30, td::Timestamp::at(first->monotonic_ts() + 0.5),
                                                            candidate->id, retry->monotonic_ts() + 0.1);
  auto received = consensus::stats::CandidateReceived::create(candidate, true);
  auto finalize_cert = CertObserved::create(FinalizeVote{candidate->id});

  auto replay = [&](bool completion_before_retry) {
    MetricCollector collector(metrics::BlockChain::shard, kMerged);
    collector.collect_collate_started(*first);
    if (completion_before_retry) {
      collector.collect_cert_observed(*previous_completed);
      collector.collect_collate_started(*retry);
    } else {
      collector.collect_collate_started(*retry);
      collector.collect_cert_observed(*previous_completed);
    }
    collector.collect_collate_finished(*finished);
    collector.collect_candidate_received(*received);
    collect_outcome(collector, candidate);
    collector.collect_cert_observed(*finalize_cert);
    return render(collector.take_metrics());
  };

  auto output = replay(true);
  ASSERT_EQ(output, replay(false));

  // The handoff and the slot mark stay on the first launch — 300 ms before the retry, so a
  // retry-based gap would miss this bucket — while the stage measures the winning attempt.
  ASSERT_TRUE(has_line(output, "ton_consensus_slot_gap_seconds_bucket{chain=\"shard\",le=\"0.25\"} 1.000000"));
  ASSERT_TRUE(has_line(output, "ton_consensus_slot_lead_seconds_count{chain=\"shard\"} 1.000000"));
  ASSERT_TRUE(has_line(output,
                       "ton_consensus_collator_slot_mark_seconds_sum{chain=\"shard\",mark=\"collate_start\","
                       "side=\"before\"} 0.500000"));
  ASSERT_TRUE(has_line(output, "ton_consensus_stage_seconds_sum{chain=\"shard\",stage=\"collate\"} 0.100000"));
}

}  // namespace
}  // namespace ton::validator::consensus::simplex::stats::test
