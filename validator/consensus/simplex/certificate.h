/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#pragma once

#include "crypto/block/signature-set.h"

#include "votes.h"

namespace ton::validator::consensus::simplex {

namespace tl {

using voteSignature = ton_api::consensus_simplex_voteSignature;
using VoteSignatureRef = tl_object_ptr<voteSignature>;

using voteSignatureSet = ton_api::consensus_simplex_voteSignatureSet;
using VoteSignatureSetRef = tl_object_ptr<voteSignatureSet>;

using certificate = ton_api::consensus_simplex_certificate;
using CertificateRef = tl_object_ptr<certificate>;

}  // namespace tl

template <ValidVote T>
struct Certificate : td::CntObject {
  struct VoteSignature {
    PeerValidatorId validator;
    td::BufferSlice signature;
  };

  static td::Result<td::Ref<Certificate<T>>> from_tl(tl::voteSignatureSet&& set, T vote, const Bus& bus);
  static td::Result<td::Ref<Certificate<Vote>>> from_tl(tl::certificate&& cert, const Bus& bus)
    requires std::same_as<T, Vote>;

  Certificate(T vote, std::vector<VoteSignature> signatures) : vote(vote), signatures(std::move(signatures)) {
  }

  td::Ref<block::BlockSignatureSet> to_signature_set(const RawCandidateRef& candidate, const Bus& bus) const
    requires td::OneOf<T, NotarizeVote, FinalizeVote>;

  tl::VoteSignatureSetRef to_tl_vote_signature_set() const;
  tl::CertificateRef to_tl() const;

  T vote;
  std::vector<VoteSignature> signatures;
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
