/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "ton/ton-io.hpp"
#include "ton/ton-tl.hpp"

#include "stats.h"

namespace ton::validator::consensus::stats {

std::unique_ptr<Id> Id::create(ShardIdFull shard, td::uint32 cc_seqno, std::optional<size_t> idx,
                               size_t total_validators, ValidatorWeight weight, ValidatorWeight total_weight,
                               td::uint32 slots_per_leader_window) {
  return std::unique_ptr<Id>(new Id(shard.workchain, shard.shard, cc_seqno, idx, total_validators, weight, total_weight,
                                    slots_per_leader_window));
}

tl::EventRef Id::to_tl() const {
  auto idx = idx_.has_value() ? static_cast<int>(*idx_) : -1;
  return create_tl_object<tl::id>(workchain_, shard_, cc_seqno_, idx, static_cast<int>(total_validators_), weight_,
                                  total_weight_, slots_per_leader_window_);
}

std::string Id::to_string() const {
  return PSTRING() << "Id{workchain=" << workchain_ << ", shard=" << shard_ << ", cc_seqno=" << cc_seqno_
                   << ", idx=" << idx_ << ", total_validators=" << total_validators_ << ", weight=" << weight_
                   << ", total_weight=" << total_weight_ << ", slots_per_leader_window=" << slots_per_leader_window_
                   << "}";
}

Id::Id(WorkchainId workchain, ShardId shard, td::uint32 cc_seqno, std::optional<size_t> idx, size_t total_validators,
       ValidatorWeight weight, ValidatorWeight total_weight, td::uint32 slots_per_leader_window)
    : workchain_(workchain)
    , shard_(shard)
    , cc_seqno_(cc_seqno)
    , idx_(idx)
    , total_validators_(total_validators)
    , weight_(weight)
    , total_weight_(total_weight)
    , slots_per_leader_window_(slots_per_leader_window) {
}

std::unique_ptr<CollateStarted> CollateStarted::create(td::uint32 slot, td::Timestamp slot_start) {
  return std::unique_ptr<CollateStarted>(new CollateStarted(slot, slot_start.at()));
}

tl::EventRef CollateStarted::to_tl() const {
  // Slot timing is local-only Prometheus state. Preserve the trace-log constructor exactly.
  return create_tl_object<tl::collateStarted>(target_slot_);
}

std::string CollateStarted::to_string() const {
  return PSTRING() << "CollateStarted{target_slot=" << target_slot_ << "}";
}

void CollateStarted::collect_to(MetricCollector& collector) const {
  collector.collect_collate_started(*this);
}

CollateStarted::CollateStarted(td::uint32 target_slot, double slot_start_monotonic)
    : target_slot_(target_slot), slot_start_monotonic_(slot_start_monotonic) {
}

std::unique_ptr<CollateFinished> CollateFinished::create(td::uint32 target_slot, td::Timestamp slot_start,
                                                         CandidateId id, double finished_at_monotonic) {
  return std::unique_ptr<CollateFinished>(new CollateFinished(target_slot, slot_start.at(), id, finished_at_monotonic));
}

tl::EventRef CollateFinished::to_tl() const {
  // The steady-clock fields are in-process metrics data. Preserve both the existing trace-log
  // constructor and Event::ts(): before local launch-slot tracking existed, target_slot carried
  // the assigned candidate slot. Keep that value in logs so their meaning is unchanged.
  return create_tl_object<tl::collateFinished>(id_.slot, id_.to_tl());
}

std::string CollateFinished::to_string() const {
  return PSTRING() << "CollateFinished{target_slot=" << id_.slot << ", id=" << id_ << "}";
}

void CollateFinished::collect_to(MetricCollector& collector) const {
  collector.collect_collate_finished(*this);
}

CollateFinished::CollateFinished(td::uint32 target_slot, double slot_start_monotonic, CandidateId id,
                                 double finished_at_monotonic)
    : target_slot_(target_slot)
    , slot_start_monotonic_(slot_start_monotonic)
    , finished_at_monotonic_(finished_at_monotonic)
    , id_(id) {
}

std::unique_ptr<CollatedEmpty> CollatedEmpty::create(td::uint32 target_slot, CandidateId id) {
  return std::unique_ptr<CollatedEmpty>(new CollatedEmpty(target_slot, id));
}

tl::EventRef CollatedEmpty::to_tl() const {
  return create_tl_object<tl::collatedEmpty>(id_.to_tl());
}

std::string CollatedEmpty::to_string() const {
  return PSTRING() << "CollatedEmpty{id=" << id_ << "}";
}

void CollatedEmpty::collect_to(MetricCollector& collector) const {
  collector.collect_collated_empty(*this);
}

CollatedEmpty::CollatedEmpty(td::uint32 target_slot, CandidateId id) : target_slot_(target_slot), id_(id) {
}

std::unique_ptr<ReceivedDelegation> ReceivedDelegation::create(td::uint32 start_slot) {
  return std::unique_ptr<ReceivedDelegation>(new ReceivedDelegation(start_slot));
}

tl::EventRef ReceivedDelegation::to_tl() const {
  return create_tl_object<tl::receivedDelegation>(start_slot_);
}

std::string ReceivedDelegation::to_string() const {
  return PSTRING() << "ReceivedDelegation{start_slot=" << start_slot_ << "}";
}

ReceivedDelegation::ReceivedDelegation(td::uint32 start_slot) : start_slot_(start_slot) {
}

std::unique_ptr<SentDelegation> SentDelegation::create(td::uint32 start_slot, adnl::AdnlNodeIdShort collator_node_id) {
  return std::unique_ptr<SentDelegation>(new SentDelegation(start_slot, collator_node_id));
}

tl::EventRef SentDelegation::to_tl() const {
  return create_tl_object<tl::sentDelegation>(start_slot_, collator_node_id_.bits256_value());
}

std::string SentDelegation::to_string() const {
  return PSTRING() << "SentDelegation{start_slot=" << start_slot_ << ", collator_node_id=" << collator_node_id_ << "}";
}

SentDelegation::SentDelegation(td::uint32 start_slot, adnl::AdnlNodeIdShort collator_node_id)
    : start_slot_(start_slot), collator_node_id_(collator_node_id) {
}

std::unique_ptr<ConcludedDelegation> ConcludedDelegation::create(td::uint32 start_slot,
                                                                 adnl::AdnlNodeIdShort collator_node_id, bool success) {
  return std::unique_ptr<ConcludedDelegation>(new ConcludedDelegation(start_slot, collator_node_id, success));
}

tl::EventRef ConcludedDelegation::to_tl() const {
  return create_tl_object<tl::concludedDelegation>(start_slot_, collator_node_id_.bits256_value(), success_);
}

std::string ConcludedDelegation::to_string() const {
  return PSTRING() << "ConcludedDelegation{start_slot=" << start_slot_ << ", collator_node_id=" << collator_node_id_
                   << ", success=" << success_ << "}";
}

ConcludedDelegation::ConcludedDelegation(td::uint32 start_slot, adnl::AdnlNodeIdShort collator_node_id, bool success)
    : start_slot_(start_slot), collator_node_id_(collator_node_id), success_(success) {
}

std::unique_ptr<CandidateReceived> CandidateReceived::create(const CandidateRef& candidate, bool is_collator) {
  auto empty_fn = [&](const BlockIdExt&) { return std::optional<BlockIdExt>{}; };
  auto candidate_fn = [&](const BlockCandidate& candidate_block) { return std::optional{candidate_block.id}; };
  auto block = std::visit(td::overloaded(empty_fn, candidate_fn), candidate->block);
  std::optional<adnl::AdnlNodeIdShort> collator_node;
  if (candidate->delegation) {
    collator_node = adnl::AdnlNodeIdShort{candidate->delegation->collator_key.compute_short_id()};
  }

  return std::unique_ptr<CandidateReceived>(
      new CandidateReceived(candidate->id, candidate->parent_id, block, is_collator, collator_node));
}

tl::EventRef CandidateReceived::to_tl() const {
  tl::CandidateBlockRef block;
  if (block_.has_value()) {
    block = create_tl_object<tl::block>(create_tl_block_id(*block_));
  } else {
    block = create_tl_object<tl::empty>();
  }
  return create_tl_object<tl::candidateReceived>(
      id_.to_tl(), CandidateId::parent_id_to_tl(parent_), std::move(block), is_collator_,
      collator_node_ ? collator_node_->bits256_value() : td::Bits256::zero());
}

std::string CandidateReceived::to_string() const {
  std::string block_str = "empty";
  if (block_.has_value()) {
    block_str = block_->to_str();
  }
  return PSTRING() << "CandidateReceived{id=" << id_ << ", parent=" << parent_ << ", block_id=" << block_str
                   << ", collator_node=" << collator_node_ << "}";
}

void CandidateReceived::collect_to(MetricCollector& collector) const {
  collector.collect_candidate_received(*this);
}

CandidateReceived::CandidateReceived(CandidateId id, ParentId parent, std::optional<BlockIdExt> block, bool is_collator,
                                     std::optional<adnl::AdnlNodeIdShort> collator_node)
    : id_(id), parent_(parent), block_(block), is_collator_(is_collator), collator_node_(collator_node) {
}

std::unique_ptr<ValidationStarted> ValidationStarted::create(CandidateId id) {
  return std::unique_ptr<ValidationStarted>(new ValidationStarted(id));
}

tl::EventRef ValidationStarted::to_tl() const {
  return create_tl_object<tl::validationStarted>(id_.to_tl());
}

std::string ValidationStarted::to_string() const {
  return PSTRING() << "ValidationStarted{id=" << id_ << "}";
}

void ValidationStarted::collect_to(MetricCollector& collector) const {
  collector.collect_validation_started(*this);
}

ValidationStarted::ValidationStarted(CandidateId id) : id_(id) {
}

std::unique_ptr<ValidationFinished> ValidationFinished::create(CandidateId id) {
  return std::unique_ptr<ValidationFinished>(new ValidationFinished(id));
}

tl::EventRef ValidationFinished::to_tl() const {
  return create_tl_object<tl::validationFinished>(id_.to_tl());
}

std::string ValidationFinished::to_string() const {
  return PSTRING() << "ValidationFinished{id=" << id_ << "}";
}

void ValidationFinished::collect_to(MetricCollector& collector) const {
  collector.collect_validation_finished(*this);
}

ValidationFinished::ValidationFinished(CandidateId id) : id_(id) {
}

std::unique_ptr<BlockAccepted> BlockAccepted::create(const CandidateRef& candidate) {
  std::optional<adnl::AdnlNodeIdShort> collator_node_id;
  if (candidate->delegation) {
    collator_node_id = adnl::AdnlNodeIdShort{candidate->delegation->collator_key.compute_short_id()};
  }
  return std::unique_ptr<BlockAccepted>(new BlockAccepted(candidate->id, candidate->block_id(), collator_node_id));
}

tl::EventRef BlockAccepted::to_tl() const {
  return create_tl_object<tl::blockAccepted>(
      id_.to_tl(), create_tl_block_id(block_id_),
      collator_node_id_ ? collator_node_id_->bits256_value() : td::Bits256::zero());
}

std::string BlockAccepted::to_string() const {
  return PSTRING() << "BlockAccepted{id=" << id_ << ", block_id=" << block_id_
                   << ", collator_node_id=" << collator_node_id_ << "}";
}

void BlockAccepted::collect_to(MetricCollector& collector) const {
  collector.collect_block_accepted(*this);
}

BlockAccepted::BlockAccepted(CandidateId id, BlockIdExt block_id, std::optional<adnl::AdnlNodeIdShort> collator_node_id)
    : id_(id), block_id_(block_id), collator_node_id_(collator_node_id) {
}

}  // namespace ton::validator::consensus::stats
