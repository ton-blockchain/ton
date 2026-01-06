/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "td/utils/overloaded.h"
#include "validator/consensus/bus.h"

#include "certificate.h"

namespace ton::validator::consensus::simplex {

template <ValidVote T>
td::Result<td::Ref<Certificate<T>>> Certificate<T>::from_tl(tl::voteSignatureSet&& set, T vote, const Bus& bus) {
  auto vote_to_sign = serialize_tl_object(vote.to_tl(), true);

  std::vector<bool> voted(bus.validator_set.size(), false);
  std::vector<VoteSignature> signatures;
  ValidatorWeight voted_weight = 0;

  for (auto& signature : set.votes_) {
    auto who = static_cast<td::uint32>(signature->who_);
    if (who >= bus.validator_set.size()) {
      return td::Status::Error(PSTRING() << "Invalid validator index " << who << " in certificate");
    }
    if (voted[who]) {
      return td::Status::Error(PSTRING() << "Duplicate validator index " << who << " in certificate");
    }
    voted[who] = true;

    auto validator = PeerValidatorId{who}.get_using(bus);
    if (!validator.check_signature(bus.session_id, vote_to_sign, signature->signature_)) {
      return td::Status::Error(PSTRING() << "Invalid vote signature for " << validator);
    }
    signatures.emplace_back(VoteSignature{validator.idx, std::move(signature->signature_)});
    voted_weight += validator.weight;
  }

  if (voted_weight < (bus.total_weight * 2) / 3 + 1) {
    return td::Status::Error("Not enough signatures in certificate");
  }

  return td::make_ref<Certificate<T>>(std::move(vote), std::move(signatures));
}

template <>
td::Result<td::Ref<Certificate<Vote>>> Certificate<Vote>::from_tl(tl::certificate&& cert, const Bus& bus) {
  auto vote_to_sign = serialize_tl_object(cert.vote_, true);
  auto vote = Vote::from_tl(std::move(*cert.vote_));
  return from_tl(std::move(*cert.signatures_), std::move(vote), bus);
}

template <ValidVote T>
tl::VoteSignatureSetRef Certificate<T>::to_tl_vote_signature_set() const {
  std::vector<tl::VoteSignatureRef> tl_sigs;
  for (const auto& [validator, signature] : signatures) {
    tl_sigs.push_back(create_tl_object<tl::voteSignature>(validator.value(), signature.clone()));
  }
  return create_tl_object<tl::voteSignatureSet>(std::move(tl_sigs));
}

template <ValidVote T>
tl::CertificateRef Certificate<T>::to_tl() const {
  return create_tl_object<tl::certificate>(vote.to_tl(), to_tl_vote_signature_set());
}

template <ValidVote T>
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
template struct Certificate<Vote>;

}  // namespace ton::validator::consensus::simplex
