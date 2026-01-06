/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#pragma once

#include <variant>

#include "consensus/types.h"

namespace ton::validator::consensus::simplex {

namespace tl {

using notarizeVote = ton_api::consensus_simplex_notarizeVote;
using finalizeVote = ton_api::consensus_simplex_finalizeVote;
using skipVote = ton_api::consensus_simplex_skipVote;
using UnsignedVote = ton_api::consensus_simplex_UnsignedVote;
using UnsignedVoteRef = tl_object_ptr<UnsignedVote>;

using vote = ton_api::consensus_simplex_vote;
using VoteRef = tl_object_ptr<vote>;

}  // namespace tl

struct NotarizeVote {
  static NotarizeVote from_tl(tl::notarizeVote&& vote);
  tl::UnsignedVoteRef to_tl() const;

  td::uint32 referenced_slot() const {
    return id.slot;
  }

  bool operator==(const NotarizeVote&) const = default;

  RawCandidateId id;
};

td::StringBuilder& operator<<(td::StringBuilder& sb, const NotarizeVote& vote);

struct FinalizeVote {
  static FinalizeVote from_tl(tl::finalizeVote&& vote);
  tl::UnsignedVoteRef to_tl() const;

  td::uint32 referenced_slot() const {
    return id.slot;
  }

  bool operator==(const FinalizeVote&) const = default;

  RawCandidateId id;
};

td::StringBuilder& operator<<(td::StringBuilder& sb, const FinalizeVote& vote);

struct SkipVote {
  static SkipVote from_tl(tl::skipVote&& vote);
  tl::UnsignedVoteRef to_tl() const;

  td::uint32 referenced_slot() const {
    return slot;
  }

  bool operator==(const SkipVote&) const = default;

  td::uint32 slot;
};

td::StringBuilder& operator<<(td::StringBuilder& sb, const SkipVote& vote);

struct Vote {
  static Vote from_tl(tl::UnsignedVote&& vote);

  Vote(td::OneOf<NotarizeVote, FinalizeVote, SkipVote> auto vote) : vote(std::move(vote)) {
  }

  tl::UnsignedVoteRef to_tl() const;

  std::variant<NotarizeVote, FinalizeVote, SkipVote> vote;
};

td::StringBuilder& operator<<(td::StringBuilder& sb, const Vote& vote);

template <typename T>
concept ValidVote = td::OneOf<T, NotarizeVote, FinalizeVote, SkipVote, Vote>;

template <ValidVote T>
struct Signed {
  td::BufferSlice serialize() const;

  static td::Result<Signed<Vote>> deserialize(td::Slice data, PeerValidatorId validator, const Bus& bus)
    requires std::same_as<T, Vote>;

  bool operator==(const Signed&) const = delete;  // Ed25519 signatures are not unique

  auto consume_and_downcast(auto&& func) &&
    requires std::same_as<T, Vote>
  {
    auto visitor = [&]<typename U>(const U& vote) { return func(Signed<U>{validator, vote, std::move(signature)}); };
    return std::visit(visitor, vote.vote);
  }

  PeerValidatorId validator;
  T vote;
  td::BufferSlice signature;
};

}  // namespace ton::validator::consensus::simplex
