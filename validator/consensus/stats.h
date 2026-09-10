/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#pragma once

#include "types.h"

namespace ton::validator::consensus::stats {

namespace tl {

using block = ton_api::consensus_stats_block;
using empty = ton_api::consensus_stats_empty;
using CandidateBlock = ton_api::consensus_stats_CandidateBlock;
using CandidateBlockRef = tl_object_ptr<CandidateBlock>;

using id = ton_api::consensus_stats_id;
using collateStarted = ton_api::consensus_stats_collateStarted;
using collateFinished = ton_api::consensus_stats_collateFinished;
using collatedEmpty = ton_api::consensus_stats_collatedEmpty;
using receivedDelegation = ton_api::consensus_stats_receivedDelegation;
using sentDelegation = ton_api::consensus_stats_sentDelegation;
using concludedDelegation = ton_api::consensus_stats_concludedDelegation;
using candidateReceived = ton_api::consensus_stats_candidateReceived;
using validationStarted = ton_api::consensus_stats_validationStarted;
using validationFinished = ton_api::consensus_stats_validationFinished;
using blockAccepted = ton_api::consensus_stats_blockAccepted;
using Event = ton_api::consensus_stats_Event;
using EventRef = tl_object_ptr<Event>;

using timestampedEvent = ton_api::consensus_stats_timestampedEvent;
using TimestampedEventRef = tl_object_ptr<timestampedEvent>;

using events = ton_api::consensus_stats_events;
using EventsRef = tl_object_ptr<events>;

}  // namespace tl

class MetricCollector;

class Id : public Event {
 public:
  static std::unique_ptr<Id> create(ShardIdFull shard, td::uint32 cc_seqno, std::optional<size_t> idx,
                                    size_t total_validators, ValidatorWeight weight, ValidatorWeight total_weight,
                                    td::uint32 slots_per_leader_window);

  tl::EventRef to_tl() const override;
  std::string to_string() const override;

 private:
  Id(WorkchainId workchain, ShardId shard, td::uint32 cc_seqno, std::optional<size_t> idx, size_t total_validators,
     ValidatorWeight weight, ValidatorWeight total_weight, td::uint32 slots_per_leader_window);

  WorkchainId workchain_;
  ShardId shard_;
  td::uint32 cc_seqno_;
  std::optional<size_t> idx_;
  size_t total_validators_;
  ValidatorWeight weight_;
  ValidatorWeight total_weight_;
  td::uint32 slots_per_leader_window_;
};

class CollateStarted : public CollectibleEvent<MetricCollector> {
 public:
  // slot_start identifies when the target slot begins. Only its steady-clock value is retained for
  // in-process metrics; the existing trace event and text remain unchanged. For a shard it is the
  // collation's wait-for-externals deadline; on the masterchain there is none, so it is the moment
  // collation is launched (window-producer.cpp uses start_collate_before = 0 there).
  static std::unique_ptr<CollateStarted> create(td::uint32 slot, td::Timestamp slot_start);

  tl::EventRef to_tl() const override;
  std::string to_string() const override;
  void collect_to(MetricCollector& collector) const override;

  td::uint32 target_slot() const {
    return target_slot_;
  }
  double slot_start_monotonic() const {
    return slot_start_monotonic_;
  }

 private:
  CollateStarted(td::uint32 target_slot, double slot_start_monotonic);

  td::uint32 target_slot_;
  double slot_start_monotonic_;
};

class CollateFinished : public CollectibleEvent<MetricCollector> {
 public:
  // target_slot is local-only launch-slot telemetry; id.slot and slot_start identify the slot to
  // which the completed candidate was actually assigned. They differ when a slow future survives
  // one or more empty slots. to_tl() keeps the legacy assigned-slot value in the existing
  // target_slot field. The monotonic finish is the collator's actual completion timestamp, not the
  // later moment window-producer happens to consume the future.
  static std::unique_ptr<CollateFinished> create(td::uint32 target_slot, td::Timestamp slot_start, CandidateId id,
                                                 double finished_at_monotonic);

  tl::EventRef to_tl() const override;
  std::string to_string() const override;
  void collect_to(MetricCollector& collector) const override;

  td::uint32 target_slot() const {
    return target_slot_;
  }
  double slot_start_monotonic() const {
    return slot_start_monotonic_;
  }
  double finished_at_monotonic() const {
    return finished_at_monotonic_;
  }
  CandidateId id() const {
    return id_;
  }

 private:
  CollateFinished(td::uint32 target_slot, double slot_start_monotonic, CandidateId id, double finished_at_monotonic);

  td::uint32 target_slot_;
  double slot_start_monotonic_;
  double finished_at_monotonic_;
  CandidateId id_;
};

class CollatedEmpty : public CollectibleEvent<MetricCollector> {
 public:
  // target_slot identifies the still-running collation future that forced this empty slot. It is
  // local telemetry only; to_tl() intentionally keeps the legacy on-disk constructor unchanged.
  static std::unique_ptr<CollatedEmpty> create(td::uint32 target_slot, CandidateId id);

  tl::EventRef to_tl() const override;
  std::string to_string() const override;
  void collect_to(MetricCollector& collector) const override;

  td::uint32 target_slot() const {
    return target_slot_;
  }
  CandidateId id() const {
    return id_;
  }

 private:
  CollatedEmpty(td::uint32 target_slot, CandidateId id);

  td::uint32 target_slot_;
  CandidateId id_;
};

class ReceivedDelegation : public Event {
 public:
  static std::unique_ptr<ReceivedDelegation> create(td::uint32 start_slot);

  tl::EventRef to_tl() const override;
  std::string to_string() const override;

 private:
  explicit ReceivedDelegation(td::uint32 start_slot);

  td::uint32 start_slot_;
};

class SentDelegation : public Event {
 public:
  static std::unique_ptr<SentDelegation> create(td::uint32 start_slot, adnl::AdnlNodeIdShort collator_node_id);

  tl::EventRef to_tl() const override;
  std::string to_string() const override;

 private:
  SentDelegation(td::uint32 start_slot, adnl::AdnlNodeIdShort collator_node_id);

  td::uint32 start_slot_;
  adnl::AdnlNodeIdShort collator_node_id_;
};

class ConcludedDelegation : public Event {
 public:
  static std::unique_ptr<ConcludedDelegation> create(td::uint32 start_slot, adnl::AdnlNodeIdShort collator_node_id,
                                                     bool success);

  tl::EventRef to_tl() const override;
  std::string to_string() const override;

 private:
  ConcludedDelegation(td::uint32 start_slot, adnl::AdnlNodeIdShort collator_node_id, bool success);

  td::uint32 start_slot_;
  adnl::AdnlNodeIdShort collator_node_id_;
  bool success_;
};

class CandidateReceived : public CollectibleEvent<MetricCollector> {
 public:
  static std::unique_ptr<CandidateReceived> create(const CandidateRef& candidate, bool is_collator);

  tl::EventRef to_tl() const override;
  std::string to_string() const override;
  void collect_to(MetricCollector& collector) const override;

  CandidateId id() const {
    return id_;
  }

 private:
  CandidateReceived(CandidateId id, ParentId parent, std::optional<BlockIdExt> block, bool is_collator,
                    std::optional<adnl::AdnlNodeIdShort> collator_node);

  CandidateId id_;
  ParentId parent_;
  std::optional<BlockIdExt> block_;
  bool is_collator_;
  std::optional<adnl::AdnlNodeIdShort> collator_node_;
};

class ValidationStarted : public CollectibleEvent<MetricCollector> {
 public:
  static std::unique_ptr<ValidationStarted> create(CandidateId id);

  tl::EventRef to_tl() const override;
  std::string to_string() const override;
  void collect_to(MetricCollector& collector) const override;

  CandidateId id() const {
    return id_;
  }

 private:
  ValidationStarted(CandidateId id);

  CandidateId id_;
};

class ValidationFinished : public CollectibleEvent<MetricCollector> {
 public:
  static std::unique_ptr<ValidationFinished> create(CandidateId id);

  tl::EventRef to_tl() const override;
  std::string to_string() const override;
  void collect_to(MetricCollector& collector) const override;

  CandidateId id() const {
    return id_;
  }

 private:
  ValidationFinished(CandidateId id);

  CandidateId id_;
};

class BlockAccepted : public CollectibleEvent<MetricCollector> {
 public:
  static std::unique_ptr<BlockAccepted> create(const CandidateRef& candidate);

  tl::EventRef to_tl() const override;
  std::string to_string() const override;
  void collect_to(MetricCollector& collector) const override;

  CandidateId id() const {
    return id_;
  }

 private:
  BlockAccepted(CandidateId id, BlockIdExt block_id, std::optional<adnl::AdnlNodeIdShort> collator_node_id);

  CandidateId id_;
  BlockIdExt block_id_;
  std::optional<adnl::AdnlNodeIdShort> collator_node_id_;
};

class MetricCollector {
 public:
  virtual ~MetricCollector() = default;

  virtual void collect_collate_started(const CollateStarted& event) = 0;
  virtual void collect_collate_finished(const CollateFinished& event) = 0;
  virtual void collect_collated_empty(const CollatedEmpty& event) = 0;
  virtual void collect_candidate_received(const CandidateReceived& event) = 0;
  virtual void collect_validation_started(const ValidationStarted& event) = 0;
  virtual void collect_validation_finished(const ValidationFinished& event) = 0;
  virtual void collect_block_accepted(const BlockAccepted& event) = 0;
};

}  // namespace ton::validator::consensus::stats
