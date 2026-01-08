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

  tl::CandidateIdRef to_tl() const;
  std::strong_ordering operator<=>(const RawCandidateId&) const = default;

  td::uint32 slot{0};
  Bits256 hash{};
};

using RawParentId = std::optional<RawCandidateId>;

td::StringBuilder& operator<<(td::StringBuilder& stream, const RawCandidateId& id);
td::StringBuilder& operator<<(td::StringBuilder& stream, const RawParentId& id);

struct CandidateHashData;

struct CandidateId {
  static CandidateId create(td::uint32 slot, const CandidateHashData& builder);

  CandidateId() = default;

  CandidateId(RawCandidateId id, BlockIdExt block) : slot(id.slot), hash(id.hash), block(block) {
  }

  RawCandidateId as_raw() {
    return RawCandidateId{slot, hash};
  }

  operator RawCandidateId() const {
    return RawCandidateId{slot, hash};
  }

  std::strong_ordering operator<=>(const CandidateId&) const = default;

  td::uint32 slot{0};
  Bits256 hash{};
  BlockIdExt block;
};

using ParentId = std::optional<CandidateId>;

td::StringBuilder& operator<<(td::StringBuilder& stream, const CandidateId& id);
td::StringBuilder& operator<<(td::StringBuilder& stream, const ParentId& id);

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
  tl::CandidateHashDataRef to_tl() const;
  Bits256 hash() const;

  [[nodiscard]] bool check(BlockIdExt block, Bits256 candidate_hash) const;

  std::variant<EmptyCandidate, FullCandidate> candidate;
  RawParentId parent;
};

struct RawCandidate : td::CntObject {
  static td::Result<td::Ref<RawCandidate>> deserialize(td::Slice data, const PeerValidator& leader, const Bus& bus);

  RawCandidate(CandidateId id, RawParentId parent_id, PeerValidatorId leader,
               std::variant<BlockIdExt, BlockCandidate> block, td::BufferSlice signature)
      : id(id)
      , parent_id(std::move(parent_id))
      , leader(leader)
      , block(std::move(block))
      , signature(std::move(signature)) {
    CHECK(std::holds_alternative<BlockCandidate>(this->block) || this->parent_id.has_value());
  }

  CandidateHashData hash_data() const;
  td::BufferSlice serialize() const;

  CandidateId id;
  RawParentId parent_id;
  PeerValidatorId leader;
  std::variant<BlockIdExt, BlockCandidate> block;
  td::BufferSlice signature;
};

using RawCandidateRef = td::Ref<RawCandidate>;

struct Candidate : td::CntObject {
  Candidate(ParentId parent_id, RawCandidateRef raw)
      : id(raw->id)
      , parent_id(parent_id)
      , leader(raw->leader)
      , block(raw->block)
      , signature(raw->signature)
      , raw(std::move(raw)) {
    CHECK(parent_id == this->raw->parent_id);

    if (auto* id = std::get_if<BlockIdExt>(&block)) {
      CHECK(parent_id->block == *id);
    }
  }

  CandidateId id;
  ParentId parent_id;
  PeerValidatorId leader;
  const std::variant<BlockIdExt, BlockCandidate>& block;
  const td::BufferSlice& signature;

  RawCandidateRef raw;
};

using CandidateRef = td::Ref<Candidate>;

}  // namespace ton::validator::consensus
