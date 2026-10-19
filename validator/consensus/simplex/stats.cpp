/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include <cmath>
#include <limits>

#include "bus.h"
#include "stats.h"

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

MetricCollector::MetricCollector(metrics::BlockChain chain, Roles roles) : chain_(chain), roles_(roles) {
}

bool MetricCollector::too_old(td::uint32 slot) const {
  return newest_slot_ && *newest_slot_ > kSlotRetention && slot < *newest_slot_ - kSlotRetention;
}

void MetricCollector::remember_slot(td::uint32 slot) {
  if (!newest_slot_ || slot > *newest_slot_) {
    newest_slot_ = slot;
    prune_old_slots();
  }
}

void MetricCollector::prune_old_slots() {
  if (!newest_slot_ || *newest_slot_ <= kSlotRetention) {
    return;
  }
  const td::uint32 cutoff = *newest_slot_ - kSlotRetention;

  while (!flows_.empty() && flows_.begin()->first.slot < cutoff) {
    flows_.erase(flows_.begin());
  }
  collate_started_by_slot_.erase(collate_started_by_slot_.begin(), collate_started_by_slot_.lower_bound(cutoff));
  completed_at_by_slot_.erase(completed_at_by_slot_.begin(), completed_at_by_slot_.lower_bound(cutoff));
  completed_slots_.erase(completed_slots_.begin(), completed_slots_.lower_bound(cutoff));
  observed_handoffs_.erase(observed_handoffs_.begin(), observed_handoffs_.lower_bound(cutoff));
}

void MetricCollector::set_mark(Flow& flow, metrics::ConsensusMark mark, double timestamp) {
  if (!std::isfinite(timestamp)) {
    return;
  }
  auto& value = flow.marks[mark_index(mark)];
  if (!value) {
    value = timestamp;
    observe_ready_stages(flow);
  }
}

void MetricCollector::observe_ready_stages(Flow& flow) {
  if (!flow.round_observed) {
    return;
  }
  for (size_t index = 0; index < flow.stages_observed.size(); ++index) {
    if (flow.stages_observed.test(index) || !flow.marks[index] || !flow.marks[index + 1]) {
      continue;
    }
    const auto stage = static_cast<metrics::ConsensusStage>(index);
    if (!owns(stage)) {
      flow.stages_observed.set(index);
      continue;
    }
    // A relaunched slot keeps marks[collate_start] on the first attempt, which is where work on the
    // slot began; the stage measures the attempt that actually produced this candidate.
    const double began = stage == metrics::ConsensusStage::collate
                             ? flow.collate_attempt_start.value_or(*flow.marks[index])
                             : *flow.marks[index];
    const double duration = *flow.marks[index + 1] - began;
    if (!std::isfinite(duration)) {
      continue;
    }
    metrics_.observe_stage(chain_, stage, duration);
    flow.stages_observed.set(index);
  }
}

void MetricCollector::observe_slot_marks(Flow& flow) {
  // Slot zero exists only on the identity that collated the slot, so the whole family is collator-
  // owned: see METRICS.md on what the timeline can and cannot show in a delegating group.
  if (!roles_.collator || !flow.round_observed || !flow.slot_start) {
    return;
  }
  for (size_t i = 0; i < flow.marks.size(); ++i) {
    if (flow.marks[i] && !flow.slot_marks_observed.test(i)) {
      metrics_.observe_slot_mark(chain_, static_cast<metrics::ConsensusMark>(i), *flow.marks[i] - *flow.slot_start);
      flow.slot_marks_observed.set(i);
    }
  }
}

void MetricCollector::record_slot_completion(td::uint32 slot, double timestamp) {
  if (!std::isfinite(timestamp)) {
    return;
  }
  auto [it, inserted] = completed_at_by_slot_.try_emplace(slot, timestamp);
  if (!inserted && timestamp < it->second) {
    it->second = timestamp;
  }
  if (slot != std::numeric_limits<td::uint32>::max()) {
    maybe_observe_handoff(slot + 1);
  }
}

bool MetricCollector::complete_slot(td::uint32 slot, double timestamp, metrics::ConsensusRoundOutcome outcome) {
  if (!completed_slots_.insert(slot).second) {
    return false;
  }
  // Every identity of the group observes the round end, so only the validator counts it; the
  // completion timestamp itself still feeds this reporter's own handoff.
  if (roles_.validator) {
    metrics_.add_round(chain_, outcome);
  }
  record_slot_completion(slot, timestamp);
  return true;
}

void MetricCollector::maybe_complete_round(const CandidateId& id, Flow& flow) {
  const auto finalized_at = flow.marks[mark_index(metrics::ConsensusMark::finalize_cert)];
  if (!finalized_at || !flow.outcome) {
    return;
  }
  if (*flow.outcome == metrics::ConsensusRoundOutcome::empty) {
    // Empty rounds have no manager-apply event and no full-block latency pipeline. Count their
    // outcome at finality, but keep them out of stage and collator-timeline distributions.
    complete_slot(id.slot, *finalized_at, *flow.outcome);
    return;
  }

  if (!flow.round_observed) {
    // Finality identifies the winning full candidate. From here onward every mark belongs in the
    // timeline, including an apply that arrives after this reporter has already flushed a delta.
    flow.round_observed = true;
    observe_ready_stages(flow);
    observe_slot_marks(flow);
  }

  if (const auto applied_at = flow.marks[mark_index(metrics::ConsensusMark::apply)]) {
    // Preserve the established meaning of outcome="accepted": the manager applied the full block.
    // This timestamp is also the handoff boundary, so storage/fsync latency remains visible.
    complete_slot(id.slot, *applied_at, *flow.outcome);
  }
}

void MetricCollector::maybe_observe_handoff(td::uint32 next_slot) {
  if (!roles_.collator || next_slot == 0 || observed_handoffs_.contains(next_slot)) {
    return;
  }
  auto completed = completed_at_by_slot_.find(next_slot - 1);
  auto started = collate_started_by_slot_.find(next_slot);
  if (completed == completed_at_by_slot_.end() || started == collate_started_by_slot_.end()) {
    return;
  }

  // The first launch is the one that answers how long the handoff took, whether or not a later
  // attempt replaced it, and whether the relaunch or the completion is observed first.
  const double gap = started->second.first_started - completed->second;
  metrics_.observe_slot_gap(chain_, std::max(gap, 0.0));
  metrics_.observe_slot_lead(chain_, std::max(-gap, 0.0));
  observed_handoffs_.insert(next_slot);
}

void MetricCollector::associate_collation(td::uint32 target_slot, td::uint32 assigned_slot) {
  auto launched = collate_started_by_slot_.find(target_slot);
  if (launched == collate_started_by_slot_.end() || target_slot > assigned_slot) {
    return;
  }

  // A single future stays active while timeout slots emit empty candidates. Every timeout calls
  // this method for its own assigned slot, so attaching just that slot is both sufficient and O(1)
  // per event; rescanning target_slot..assigned_slot would make a long-running future quadratic.
  // CollateFinished performs the same attachment for the eventual successful slot.
  const Collation collation = launched->second;
  auto [assigned, inserted] = collate_started_by_slot_.try_emplace(assigned_slot, collation);
  if (!inserted) {
    assigned->second.merge(collation);
  }
  maybe_observe_handoff(assigned_slot);
}

void MetricCollector::collect_collate_started(const consensus::stats::CollateStarted& event) {
  remember_slot(event.target_slot());
  if (too_old(event.target_slot())) {
    return;
  }
  // A terminal failure can restart a new future for the same slot, and both launches matter: the
  // abandoned one is when work on this slot began, the new one is the attempt whose duration the
  // collate stage measures. A carried future does not emit another CollateStarted, so timeout-slot
  // association is unaffected.
  const double started = event.monotonic_ts();
  const Collation launch{started, started, event.slot_start_monotonic()};
  auto [it, inserted] = collate_started_by_slot_.try_emplace(event.target_slot(), launch);
  if (!inserted) {
    it->second.merge(launch);
  }
  maybe_observe_handoff(event.target_slot());
}

void MetricCollector::collect_collate_finished(const consensus::stats::CollateFinished& event) {
  remember_slot(std::max(event.target_slot(), event.id().slot));
  if (too_old(event.id().slot)) {
    return;
  }
  auto& flow = flows_[event.id()];
  set_mark(flow, metrics::ConsensusMark::collate_finish, event.finished_at_monotonic());
  if (event.slot_start_monotonic() > 0.0) {
    flow.slot_start = event.slot_start_monotonic();
  }

  auto it = collate_started_by_slot_.find(event.target_slot());
  if (it != collate_started_by_slot_.end()) {
    // Set before the mark: set_mark folds the collate stage as soon as both its ends exist.
    flow.collate_attempt_start = it->second.last_started;
    set_mark(flow, metrics::ConsensusMark::collate_start, it->second.first_started);
    if (!flow.slot_start && event.target_slot() == event.id().slot) {
      flow.slot_start = it->second.slot_start;
    }

    associate_collation(event.target_slot(), event.id().slot);
  }
  observe_slot_marks(flow);
  maybe_complete_round(event.id(), flow);
}

void MetricCollector::collect_collated_empty(const consensus::stats::CollatedEmpty& event) {
  remember_slot(std::max(event.target_slot(), event.id().slot));
  if (!too_old(event.id().slot)) {
    associate_collation(event.target_slot(), event.id().slot);
  }
}

void MetricCollector::collect_candidate_received(const consensus::stats::CandidateReceived& event) {
  remember_slot(event.id().slot);
  if (too_old(event.id().slot)) {
    return;
  }
  auto& flow = flows_[event.id()];
  set_mark(flow, metrics::ConsensusMark::candidate_received, event.monotonic_ts());
  observe_slot_marks(flow);
  maybe_complete_round(event.id(), flow);
}

void MetricCollector::collect_validation_started(const consensus::stats::ValidationStarted& event) {
  remember_slot(event.id().slot);
  if (too_old(event.id().slot)) {
    return;
  }
  auto& flow = flows_[event.id()];
  set_mark(flow, metrics::ConsensusMark::validation_start, event.monotonic_ts());
  observe_slot_marks(flow);
}

void MetricCollector::collect_validation_finished(const consensus::stats::ValidationFinished& event) {
  remember_slot(event.id().slot);
  if (too_old(event.id().slot)) {
    return;
  }
  auto& flow = flows_[event.id()];
  set_mark(flow, metrics::ConsensusMark::validation_finish, event.monotonic_ts());
  observe_slot_marks(flow);
}

void MetricCollector::collect_block_accepted(const consensus::stats::BlockAccepted& event) {
  remember_slot(event.id().slot);
  if (too_old(event.id().slot)) {
    return;
  }
  auto& flow = flows_[event.id()];
  set_mark(flow, metrics::ConsensusMark::apply, event.monotonic_ts());
  observe_slot_marks(flow);
  maybe_complete_round(event.id(), flow);
}

void MetricCollector::collect_voted(const Voted& event) {
  std::visit(
      [&]<typename T>(const T& v) {
        if constexpr (std::is_same_v<T, SkipVote>) {
          return;
        } else {
          remember_slot(v.id.slot);
          if (too_old(v.id.slot)) {
            return;
          }
          auto& flow = flows_[v.id];
          double timestamp = event.monotonic_ts();
          if constexpr (std::is_same_v<T, NotarizeVote>) {
            set_mark(flow, metrics::ConsensusMark::notarize_vote, timestamp);
          } else if constexpr (std::is_same_v<T, FinalizeVote>) {
            set_mark(flow, metrics::ConsensusMark::finalize_vote, timestamp);
          }
          observe_slot_marks(flow);
        }
      },
      event.vote().vote);
}

void MetricCollector::collect_candidate_outcome(CandidateId id, bool is_empty) {
  remember_slot(id.slot);
  if (too_old(id.slot)) {
    return;
  }
  // CandidateResolver is authoritative for candidate kind. This local event carries no timestamp,
  // so learning through recovery can never fabricate a stage or slot mark. Keep the first value to
  // make duplicate delivery harmless; a CandidateId commits to the candidate kind, so legitimate
  // observations cannot disagree.
  auto& flow = flows_[id];
  if (!flow.outcome) {
    flow.outcome = is_empty ? metrics::ConsensusRoundOutcome::empty : metrics::ConsensusRoundOutcome::accepted;
  }
  maybe_complete_round(id, flow);
}

void MetricCollector::collect_cert_observed(const CertObserved& event) {
  std::visit(
      [&]<typename T>(const T& v) {
        if constexpr (std::is_same_v<T, SkipVote>) {
          remember_slot(v.slot);
          if (!too_old(v.slot)) {
            complete_slot(v.slot, event.monotonic_ts(), metrics::ConsensusRoundOutcome::skipped);
          }
          return;
        } else {
          remember_slot(v.id.slot);
          if (too_old(v.id.slot)) {
            return;
          }
          auto& flow = flows_[v.id];
          double timestamp = event.monotonic_ts();
          if constexpr (std::is_same_v<T, NotarizeVote>) {
            set_mark(flow, metrics::ConsensusMark::notarize_cert, timestamp);
          } else if constexpr (std::is_same_v<T, FinalizeVote>) {
            set_mark(flow, metrics::ConsensusMark::finalize_cert, timestamp);
          }
          observe_slot_marks(flow);
          maybe_complete_round(v.id, flow);
        }
      },
      event.vote().vote);
}

}  // namespace ton::validator::consensus::simplex::stats

namespace ton::validator::consensus::simplex {

namespace {

class MetricReporterImpl : public td::actor::SpawnsWith<Bus>, public td::actor::ConnectsTo<Bus> {
 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  static bool should_be_spawned(const Bus& bus) {
    // An observer sees certificates and applies blocks like everyone else, so letting it report
    // would multiply every chain-level consensus total by the number of local identities.
    return bus.is_validator() || bus.is_collator;
  }

  void start_up() override {
    const auto& bus = *owning_bus();
    manager_ = bus.manager;
    collector_.emplace(bus.shard.is_masterchain() ? metrics::BlockChain::master : metrics::BlockChain::shard,
                       stats::Roles{.validator = bus.is_validator(),
                                    // A validator collates every window it does not delegate.
                                    .collator = bus.is_validator() || bus.is_collator});
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const StopRequested>) {
    stop();
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const TraceEvent> event) {
    using Base = consensus::stats::CollectibleEvent<consensus::stats::MetricCollector>;
    using Own = consensus::stats::CollectibleEvent<stats::MetricCollector>;
    if (auto* collectible = dynamic_cast<const Base*>(event->event.get())) {
      collectible->collect_to(*collector_);
    } else if (auto* collectible = dynamic_cast<const Own*>(event->event.get())) {
      collectible->collect_to(*collector_);
    }
    alarm_timestamp().relax(td::Timestamp::in(1.0));
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const CandidateOutcomeObserved> event) {
    collector_->collect_candidate_outcome(event->id, event->is_empty);
    alarm_timestamp().relax(td::Timestamp::in(1.0));
  }

  void alarm() override {
    flush();
  }

  void tear_down() override {
    flush();
  }

 private:
  void flush() {
    td::actor::send_closure(manager_, &ManagerFacade::report_consensus_metrics, collector_->take_metrics());
  }

  td::actor::ActorId<ManagerFacade> manager_;
  std::optional<stats::MetricCollector> collector_;
};

}  // namespace

void MetricReporter::register_in(td::actor::Runtime& runtime) {
  runtime.register_actor<MetricReporterImpl>("MetricReporter");
}

}  // namespace ton::validator::consensus::simplex
