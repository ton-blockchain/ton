/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#pragma once

#include <variant>

#include "adnl/adnl-node-id.hpp"
#include "keys/keys.hpp"
#include "ton/ton-types.h"

namespace ton::validator::consensus {

namespace tl {

using overlayId = ton_api::consensus_overlayId;
using OverlayIdRef = tl_object_ptr<overlayId>;

using dataToSign = ton_api::consensus_dataToSign;
using DataToSignRef = tl_object_ptr<dataToSign>;

using candidateId = ton_api::consensus_candidateId;
using CandidateIdRef = tl_object_ptr<candidateId>;

using candidateParent = ton_api::consensus_candidateParent;
using candidateWithoutParents = ton_api::consensus_candidateWithoutParents;
using CandidateParent = ton_api::consensus_CandidateParent;
using CandidateParentRef = tl_object_ptr<CandidateParent>;

using candidateHashDataOrdinary = ton_api::consensus_candidateHashDataOrdinary;
using candidateHashDataEmpty = ton_api::consensus_candidateHashDataEmpty;
using CandidateHashData = ton_api::consensus_CandidateHashData;
using CandidateHashDataRef = tl_object_ptr<CandidateHashData>;

using block = ton_api::consensus_block;
using empty = ton_api::consensus_empty;
using CandidateData = ton_api::consensus_CandidateData;
using CandidateDataRef = tl_object_ptr<CandidateData>;

}  // namespace tl

class Bus;
struct PeerValidator;

class PeerValidatorId {
 public:
  PeerValidatorId() : idx_(std::numeric_limits<size_t>::max()) {
  }

  explicit PeerValidatorId(size_t idx) : idx_(idx) {
  }

  size_t value() const {
    return idx_;
  }

  const PeerValidator& get_using(const Bus& bus) const;

  std::strong_ordering operator<=>(const PeerValidatorId& other) const = default;

 private:
  size_t idx_;
};

td::StringBuilder& operator<<(td::StringBuilder& stream, const PeerValidatorId& id);

struct PeerValidator {
  [[nodiscard]] bool check_signature(ValidatorSessionId session, td::Slice data, td::Slice signature) const;

  bool operator==(const PeerValidator& other) const = default;

  PeerValidatorId idx;
  PublicKey key;
  PublicKeyHash short_id;
  adnl::AdnlNodeIdShort adnl_id;
  ValidatorWeight weight;
};

td::StringBuilder& operator<<(td::StringBuilder& stream, const PeerValidator& peer_validator);

struct ProtocolMessage {
  ProtocolMessage(td::BufferSlice data) : data(std::move(data)) {
  }

  template <typename T>
  ProtocolMessage(const tl_object_ptr<T>& object) : data(serialize_tl_object(object, true)) {
  }

  td::BufferSlice data;
};

struct RawCandidateId {
  static RawCandidateId from_tl(const tl::CandidateIdRef& tl_parent);
  static tl::CandidateParentRef parent_id_to_tl(std::optional<RawCandidateId> parent);
  static std::optional<RawCandidateId> tl_to_parent_id(const tl::CandidateParentRef& tl_parent);

  tl::CandidateIdRef to_tl() const;
  std::strong_ordering operator<=>(const RawCandidateId&) const = default;

  td::uint32 slot{0};
  Bits256 hash{};
};

using RawParentId = std::optional<RawCandidateId>;

td::StringBuilder& operator<<(td::StringBuilder& stream, const RawCandidateId& id);
td::StringBuilder& operator<<(td::StringBuilder& stream, const RawParentId& id);

struct CandidateHashData;

struct CandidateHashData {
  struct EmptyCandidate {
    BlockIdExt reference;
  };

  struct FullCandidate {
    BlockIdExt id;
    td::Bits256 collated_file_hash;
  };

  static CandidateHashData create_empty(BlockIdExt reference, RawCandidateId parent) {
    return {EmptyCandidate{reference}, parent};
  }

  static CandidateHashData create_full(FullCandidate candidate, RawParentId parent) {
    return {candidate, parent};
  }

  static CandidateHashData create_full(const BlockCandidate& candidate, RawParentId parent) {
    return {FullCandidate{candidate.id, candidate.collated_file_hash}, parent};
  }

  static CandidateHashData from_tl(tl::CandidateHashData&& data);

  BlockIdExt block() const;
  RawCandidateId build_id_with(td::uint32 slot) const;
  tl::CandidateHashDataRef to_tl() const;

  std::variant<EmptyCandidate, FullCandidate> candidate;
  RawParentId parent;
};

struct RawCandidate : td::CntObject {
  static td::Result<td::Ref<RawCandidate>> deserialize(td::Slice data, const Bus& bus,
                                                       std::optional<PeerValidatorId> src = std::nullopt);

  RawCandidate(RawCandidateId id, RawParentId parent_id, PeerValidatorId leader,
               std::variant<BlockIdExt, BlockCandidate> block, td::BufferSlice signature)
      : id(id)
      , parent_id(std::move(parent_id))
      , leader(leader)
      , block(std::move(block))
      , signature(std::move(signature)) {
    CHECK(std::holds_alternative<BlockCandidate>(this->block) || this->parent_id.has_value());
  }

  BlockIdExt block_id() const;
  CandidateHashData hash_data() const;
  td::BufferSlice serialize() const;
  bool is_empty() const;

  RawCandidateId id;
  RawParentId parent_id;
  PeerValidatorId leader;
  std::variant<BlockIdExt, BlockCandidate> block;
  td::BufferSlice signature;
};

using RawCandidateRef = td::Ref<RawCandidate>;

class CollatorSchedule : public td::CntObject {
 public:
  virtual PeerValidatorId expected_collator_for(td::uint32 slot) const = 0;

  [[nodiscard]] bool is_expected_collator(PeerValidatorId id, td::uint32 slot) const {
    return expected_collator_for(slot) == id;
  }
};

namespace stats {

namespace tl {

using Event = ton_api::consensus_stats_Event;
using EventRef = tl_object_ptr<Event>;

}  // namespace tl

class Event {
 public:
  Event();

  virtual ~Event() = default;

  virtual tl::EventRef to_tl() const = 0;
  virtual std::string to_string() const = 0;

  double ts() const {
    return ts_;
  }

 protected:
  double ts_;
};

template <typename Collector>
class CollectibleEvent : public Event {
 public:
  virtual void collect_to(Collector& collector) const = 0;
};

}  // namespace stats

}  // namespace ton::validator::consensus
