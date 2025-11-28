/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "auto/tl/ton_api.hpp"
#include "keys/encryptor.h"
#include "td/utils/overloaded.h"
#include "validator-session/candidate-serializer.h"

#include "bus.h"
#include "checksum.h"

namespace ton::validator::consensus {

const PeerValidator& PeerValidatorId::get_using(const Bus& bus) const {
  return bus.validator_set[idx_];
}

td::StringBuilder& operator<<(td::StringBuilder& stream, const PeerValidatorId& id) {
  return stream << "validator " << id.value();
}

bool PeerValidator::check_signature(ValidatorSessionId session, td::Slice data, td::Slice signature) const {
  auto signed_data = create_serialize_tl_object<tl::dataToSign>(session, td::BufferSlice(data));
  return key.create_encryptor().move_as_ok()->check_signature(signed_data, signature).is_ok();
}

td::StringBuilder& operator<<(td::StringBuilder& stream, const PeerValidator& peer_validator) {
  return stream << peer_validator.idx << " at " << peer_validator.short_id;
}

RawCandidateId RawCandidateId::from_tl(const tl::CandidateIdRef& tl_parent) {
  return RawCandidateId{static_cast<td::uint32>(tl_parent->slot_), tl_parent->hash_};
}

tl::CandidateIdRef RawCandidateId::to_tl() const {
  return create_tl_object<tl::candidateId>(slot, hash);
}

td::StringBuilder& operator<<(td::StringBuilder& stream, const RawCandidateId& id) {
  return stream << "{" << id.slot << ", " << id.hash << ", ?}";
}

td::StringBuilder& operator<<(td::StringBuilder& stream, const RawParentId& id) {
  if (id.has_value()) {
    return stream << *id;
  } else {
    return stream << "consensus genesis";
  }
}

namespace {

tl::CandidateParentRef parent_id_to_tl(RawParentId parent) {
  if (!parent) {
    return create_tl_object<tl::candidateWithoutParents>();
  } else {
    return create_tl_object<tl::candidateParent>(parent->to_tl());
  }
}

RawParentId tl_to_parent_id(const tl::CandidateParentRef& tl_parent) {
  RawParentId id;
  auto without_parents_fn = [&](const tl::candidateWithoutParents&) {};
  auto parent_fn = [&](const tl::candidateParent& parent) { id = RawCandidateId::from_tl(parent.id_); };
  ton_api::downcast_call(*tl_parent, td::overloaded(without_parents_fn, parent_fn));
  return id;
}

}  // namespace

tl::CandidateHashDataRef CandidateId::create_hash_data(td::uint32 slot,
                                                       const std::variant<BlockIdExt, BlockCandidate>& block,
                                                       RawParentId parent) {
  auto empty_fn = [&](const BlockIdExt& id) -> tl::CandidateHashDataRef {
    return create_tl_object<tl::candidateHashDataEmpty>(create_tl_block_id(id), parent->to_tl());
  };
  auto block_fn = [&](const BlockCandidate& candidate) -> tl::CandidateHashDataRef {
    return create_tl_object<tl::candidateHashDataOrdinary>(create_tl_block_id(candidate.id),
                                                           candidate.collated_file_hash, parent_id_to_tl(parent));
  };
  return std::visit(td::overloaded(empty_fn, block_fn), block);
}

CandidateId CandidateId::create(td::uint32 slot, const std::variant<BlockIdExt, BlockCandidate>& candidate,
                                std::optional<RawCandidateId> parent) {
  auto empty_fn = [&](const BlockIdExt& id) { return id; };
  auto block_fn = [&](const BlockCandidate& candidate) { return candidate.id; };
  auto block = std::visit(td::overloaded(empty_fn, block_fn), candidate);

  auto hash = get_tl_object_sha_bits256(create_hash_data(slot, candidate, parent));

  return CandidateId{RawCandidateId{slot, hash}, block};
}

td::StringBuilder& operator<<(td::StringBuilder& stream, const CandidateId& id) {
  return stream << "{" << id.slot << ", " << id.hash << ", " << id.block.to_str() << "}";
}

td::StringBuilder& operator<<(td::StringBuilder& stream, const ParentId& id) {
  if (id.has_value()) {
    return stream << *id;
  } else {
    return stream << "consensus genesis";
  }
}

td::Result<RawCandidateRef> RawCandidate::deserialize(td::Slice data, const PeerValidator& leader, const Bus& bus) {
  TRY_RESULT(broadcast, fetch_tl_object<tl::CandidateData>(data, true));

  struct ExtractedData {
    td::uint32 slot;
    RawParentId parent_id;
    std::variant<BlockIdExt, BlockCandidate> block;
    td::BufferSlice signature;
  };
  td::Result<ExtractedData> maybe_data;

  auto empty_fn = [](tl::empty& empty_broadcast) -> td::Result<ExtractedData> {
    return ExtractedData{
        .slot = static_cast<td::uint32>(empty_broadcast.slot_),
        .parent_id = RawCandidateId::from_tl(empty_broadcast.parent_),
        .block = create_block_id(empty_broadcast.block_),
        .signature = std::move(empty_broadcast.signature_),
    };
  };

  auto ordinary_fn = [&](tl::block& block_broadcast) -> td::Result<ExtractedData> {
    TRY_RESULT(candidate, validatorsession::deserialize_candidate(
                              block_broadcast.candidate_, true,
                              bus.config.max_block_size + bus.config.max_collated_data_size + 1024));

    if (!candidate->src_.is_zero()) {
      return td::Status::Error("src field of the candidate broadcast must be null");
    }

    if (candidate->data_.size() > bus.config.max_block_size ||
        candidate->collated_data_.size() > bus.config.max_collated_data_size) {
      return td::Status::Error(PSTRING() << "Too big candidate broadcast with data_size=" << candidate->data_.size()
                                         << ", collated_data_size=" << candidate->collated_data_.size());
    }

    BlockIdExt block_id{
        BlockId{bus.shard, static_cast<BlockSeqno>(candidate->round_)},
        candidate->root_hash_,
        td::sha256_bits256(candidate->data_.as_slice()),
    };

    auto collated_file_hash = td::sha256_bits256(candidate->collated_data_.as_slice());

    Ed25519_PublicKey creator{leader.key.ed25519_value().raw()};

    BlockCandidate block{
        creator, block_id, collated_file_hash, std::move(candidate->data_), std::move(candidate->collated_data_),
    };

    return ExtractedData{
        .slot = static_cast<td::uint32>(block_broadcast.slot_),
        .parent_id = tl_to_parent_id(block_broadcast.parent_),
        .block = std::move(block),
        .signature = std::move(block_broadcast.signature_),
    };
  };

  ton_api::downcast_call(*broadcast,
                         [&](auto& broadcast) { maybe_data = td::overloaded(empty_fn, ordinary_fn)(broadcast); });
  TRY_RESULT(parsed, std::move(maybe_data));

  auto id = CandidateId::create(parsed.slot, parsed.block, parsed.parent_id);

  auto signed_data = serialize_tl_object(id.as_raw().to_tl(), true);
  if (!leader.check_signature(bus.session_id, signed_data, parsed.signature)) {
    return td::Status::Error("Candidate broadcast signature is not valid");
  }

  return td::make_ref<RawCandidate>(id, parsed.parent_id, leader.idx, std::move(parsed.block),
                                    std::move(parsed.signature));
}

td::BufferSlice RawCandidate::serialize() const {
  auto empty_fn = [&](const BlockIdExt& referenced_block) {
    return create_serialize_tl_object<tl::empty>(id.slot, parent_id->to_tl(), create_tl_block_id(referenced_block),
                                                 signature.clone());
  };
  auto block_fn = [&](const BlockCandidate& candidate) {
    auto candidate_tl = create_tl_object<ton_api::validatorSession_candidate>(
        td::Bits256{}, candidate.id.seqno(), candidate.id.root_hash, candidate.data.clone(),
        candidate.collated_data.clone());

    return create_serialize_tl_object<tl::block>(id.slot, parent_id_to_tl(parent_id),
                                                 validatorsession::serialize_candidate(candidate_tl, true).move_as_ok(),
                                                 signature.clone());
  };
  return std::visit(td::overloaded(empty_fn, block_fn), block);
}

}  // namespace ton::validator::consensus
