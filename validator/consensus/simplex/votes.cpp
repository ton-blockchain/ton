/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "td/utils/overloaded.h"
#include "validator/consensus/bus.h"

#include "votes.h"

namespace ton::validator::consensus::simplex {

NotarizeVote NotarizeVote::from_tl(const tl::notarizeVote& vote) {
  return {RawCandidateId::from_tl(vote.id_)};
}

tl::UnsignedVoteRef NotarizeVote::to_tl() const {
  return create_tl_object<tl::notarizeVote>(id.to_tl());
}

td::StringBuilder& operator<<(td::StringBuilder& sb, const NotarizeVote& vote) {
  return sb << "NotarizeVote{id=" << vote.id << "}";
}

FinalizeVote FinalizeVote::from_tl(const tl::finalizeVote& vote) {
  return {RawCandidateId::from_tl(vote.id_)};
}

tl::UnsignedVoteRef FinalizeVote::to_tl() const {
  return create_tl_object<tl::finalizeVote>(id.to_tl());
}

td::StringBuilder& operator<<(td::StringBuilder& sb, const FinalizeVote& vote) {
  return sb << "FinalizeVote{id=" << vote.id << "}";
}

SkipVote SkipVote::from_tl(const tl::skipVote& vote) {
  return {static_cast<td::uint32>(vote.slot_)};
}

tl::UnsignedVoteRef SkipVote::to_tl() const {
  return create_tl_object<tl::skipVote>(slot);
}

td::StringBuilder& operator<<(td::StringBuilder& sb, const SkipVote& vote) {
  return sb << "SkipVote{slot=" << vote.slot << "}";
}

template <ValidVote T>
td::BufferSlice Signed<T>::serialize() const {
  tl::UnsignedVoteRef tl_vote;
  if constexpr (std::is_same_v<T, Vote>) {
    tl_vote = std::visit([](const auto& v) { return v.to_tl(); }, vote);
  } else {
    tl_vote = vote.to_tl();
  }
  return create_serialize_tl_object<tl::Vote>(std::move(tl_vote), signature.clone());
}

template <>
td::Result<Signed<Vote>> Signed<Vote>::deserialize(td::Slice data, PeerValidatorId validator, const Bus& bus) {
  TRY_RESULT(signed_vote, fetch_tl_object<tl::Vote>(data, true));

  Vote vote;
  auto notarize_fn = [&](tl::notarizeVote& tl_vote) { vote = NotarizeVote::from_tl(tl_vote); };
  auto finalize_fn = [&](tl::finalizeVote& tl_vote) { vote = FinalizeVote::from_tl(tl_vote); };
  auto skip_fn = [&](tl::skipVote& tl_vote) { vote = SkipVote::from_tl(tl_vote); };
  auto vote_to_sign = serialize_tl_object(signed_vote->unsignedVote_, true);
  ton_api::downcast_call(*signed_vote->unsignedVote_, td::overloaded(notarize_fn, finalize_fn, skip_fn));

  Signed<Vote> result{validator, std::move(vote), std::move(signed_vote->signature_)};
  if (!validator.get_using(bus).check_signature(bus.session_id, vote_to_sign, result.signature)) {
    return td::Status::Error("Invalid vote signature");
  }

  return result;
}

template struct Signed<NotarizeVote>;
template struct Signed<SkipVote>;
template struct Signed<FinalizeVote>;
template struct Signed<Vote>;

template <td::OneOf<NotarizeVote, SkipVote, FinalizeVote> T>
td::Ref<block::BlockSignatureSet> Certificate<T>::to_signature_set(const RawCandidateRef& candidate,
                                                                   const Bus& bus) const
  requires td::OneOf<T, NotarizeVote, FinalizeVote>
{
  CHECK(candidate->id == vote.id);

  std::vector<ton::BlockSignature> block_signatures;
  for (const auto& [validator, signature] : signatures) {
    block_signatures.emplace_back(validator.get_using(bus).short_id.bits256_value(), signature.clone());
  }

  auto fn = block::BlockSignatureSet::create_simplex_approve;
  if constexpr (std::same_as<T, FinalizeVote>) {
    fn = block::BlockSignatureSet::create_simplex;
  }
  return fn(std::move(block_signatures), bus.cc_seqno, bus.validator_set_hash, bus.session_id, vote.id.slot,
            CandidateId::create_hash_data(candidate->id.slot, candidate->block, candidate->parent_id));
}

template struct Certificate<NotarizeVote>;
template struct Certificate<SkipVote>;
template struct Certificate<FinalizeVote>;

}  // namespace ton::validator::consensus::simplex
