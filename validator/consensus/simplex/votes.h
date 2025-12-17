/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#pragma once

#include <variant>

#include "consensus/types.h"
#include "crypto/block/signature-set.h"

namespace ton::validator::consensus::simplex {

namespace tl {

using notarizeVote = ton_api::consensus_simplex_notarizeVote;
using finalizeVote = ton_api::consensus_simplex_finalizeVote;
using skipVote = ton_api::consensus_simplex_skipVote;
using UnsignedVote = ton_api::consensus_simplex_UnsignedVote;
using UnsignedVoteRef = tl_object_ptr<UnsignedVote>;

using Vote = ton_api::consensus_simplex_vote;
using VoteRef = tl_object_ptr<Vote>;

}  // namespace tl

struct NotarizeVote {
  static NotarizeVote from_tl(const tl::notarizeVote& vote);
  tl::UnsignedVoteRef to_tl() const;

  td::uint32 referenced_slot() const {
    return id.slot;
  }

  bool operator==(const NotarizeVote&) const = default;

  RawCandidateId id;
};

td::StringBuilder& operator<<(td::StringBuilder& sb, const NotarizeVote& vote);

struct FinalizeVote {
  static FinalizeVote from_tl(const tl::finalizeVote& vote);
  tl::UnsignedVoteRef to_tl() const;

  td::uint32 referenced_slot() const {
    return id.slot;
  }

  bool operator==(const FinalizeVote&) const = default;

  RawCandidateId id;
};

td::StringBuilder& operator<<(td::StringBuilder& sb, const FinalizeVote& vote);

struct SkipVote {
  static SkipVote from_tl(const tl::skipVote& vote);
  tl::UnsignedVoteRef to_tl() const;

  td::uint32 referenced_slot() const {
    return slot;
  }

  bool operator==(const SkipVote&) const = default;

  td::uint32 slot;
};

td::StringBuilder& operator<<(td::StringBuilder& sb, const SkipVote& vote);

using Vote = std::variant<NotarizeVote, FinalizeVote, SkipVote>;

template <typename T>
concept ValidVote = td::OneOf<T, NotarizeVote, FinalizeVote, SkipVote, Vote>;

template <ValidVote T>
struct Signed {
  td::BufferSlice serialize() const;

  static td::Result<Signed<Vote>> deserialize(td::Slice data, PeerValidatorId validator, const Bus& bus)
    requires std::same_as<T, Vote>;

  bool operator==(const Signed&) const = delete ("Ed25519 signatures are not unique");

  auto consume_and_downcast(auto&& func) &&
    requires std::same_as<T, Vote>
  {
    auto visitor = [&]<typename U>(const U& vote) { return func(Signed<U>{validator, vote, std::move(signature)}); };
    return std::visit(visitor, vote);
  }

  PeerValidatorId validator;
  T vote;
  td::BufferSlice signature;
};

template <td::OneOf<NotarizeVote, SkipVote, FinalizeVote> T>
struct Certificate : td::CntObject {
  struct Signature {
    PeerValidatorId validator;
    td::BufferSlice signature;
  };

  Certificate(T vote, std::vector<Signature> signatures) : vote(vote), signatures(std::move(signatures)) {
  }

  td::Ref<block::BlockSignatureSet> to_signature_set(const RawCandidateRef& candidate, const Bus& bus) const
    requires td::OneOf<T, NotarizeVote, FinalizeVote>;

  T vote;
  std::vector<Signature> signatures;
};

template <typename T>
using CertificateRef = td::Ref<Certificate<T>>;

using NotarCert = Certificate<NotarizeVote>;
using SkipCert = Certificate<SkipVote>;
using FinalCert = Certificate<FinalizeVote>;
using NotarCertRef = CertificateRef<NotarizeVote>;
using SkipCertRef = CertificateRef<SkipVote>;
using FinalCertRef = CertificateRef<FinalizeVote>;

}  // namespace ton::validator::consensus::simplex
