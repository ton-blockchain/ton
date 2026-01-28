/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

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
  static std::unique_ptr<Id> create(ShardIdFull shard, td::uint32 cc_seqno, size_t idx, size_t total_validators,
                                    ValidatorWeight weight, ValidatorWeight total_weight,
                                    td::uint32 slots_per_leader_window);

  tl::EventRef to_tl() const override;
  std::string to_string() const override;

 private:
  Id(WorkchainId workchain, ShardId shard, td::uint32 cc_seqno, size_t idx, size_t total_validators,
     ValidatorWeight weight, ValidatorWeight total_weight, td::uint32 slots_per_leader_window);

  WorkchainId workchain_;
  ShardId shard_;
  td::uint32 cc_seqno_;
  size_t idx_;
  size_t total_validators_;
  ValidatorWeight weight_;
  ValidatorWeight total_weight_;
  td::uint32 slots_per_leader_window_;
};

class CollateStarted : public CollectibleEvent<MetricCollector> {
 public:
  static std::unique_ptr<CollateStarted> create(td::uint32 slot);

  tl::EventRef to_tl() const override;
  std::string to_string() const override;
  void collect_to(MetricCollector& collector) const override;

  td::uint32 target_slot() const {
    return target_slot_;
  }

 private:
  CollateStarted(td::uint32 target_slot);

  td::uint32 target_slot_;
};

class CollateFinished : public CollectibleEvent<MetricCollector> {
 public:
  static std::unique_ptr<CollateFinished> create(td::uint32 slot, CandidateId id);

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
  CollateFinished(td::uint32 target_slot, CandidateId id);

  td::uint32 target_slot_;
  CandidateId id_;
};

class CollatedEmpty : public Event {
 public:
  static std::unique_ptr<CollatedEmpty> create(CandidateId id);

  tl::EventRef to_tl() const override;
  std::string to_string() const override;

  CandidateId id() const {
    return id_;
  }

 private:
  CollatedEmpty(CandidateId id);

  CandidateId id_;
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
  ParentId parent() const {
    return parent_;
  }
  std::optional<BlockIdExt> block_id() const {
    return block_;
  }
  bool is_collator() const {
    return is_collator_;
  }

 private:
  CandidateReceived(CandidateId id, ParentId parent, std::optional<BlockIdExt> block, bool is_collator);

  CandidateId id_;
  ParentId parent_;
  std::optional<BlockIdExt> block_;
  bool is_collator_;
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
  static std::unique_ptr<BlockAccepted> create(CandidateId id);

  tl::EventRef to_tl() const override;
  std::string to_string() const override;
  void collect_to(MetricCollector& collector) const override;

  CandidateId id() const {
    return id_;
  }

 private:
  BlockAccepted(CandidateId id);

  CandidateId id_;
};

class MetricCollector {
 public:
  virtual ~MetricCollector() = default;

  virtual void collect_collate_started(const CollateStarted& event) = 0;
  virtual void collect_collate_finished(const CollateFinished& event) = 0;
  virtual void collect_candidate_received(const CandidateReceived& event) = 0;
  virtual void collect_validation_started(const ValidationStarted& event) = 0;
  virtual void collect_validation_finished(const ValidationFinished& event) = 0;
  virtual void collect_block_accepted(const BlockAccepted& event) = 0;
};

}  // namespace ton::validator::consensus::stats
