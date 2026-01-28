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

CandidateId CandidateId::from_tl(const tl::CandidateIdRef& tl_parent) {
  return CandidateId{static_cast<td::uint32>(tl_parent->slot_), tl_parent->hash_};
}

tl::CandidateIdRef CandidateId::to_tl() const {
  return create_tl_object<tl::candidateId>(slot, hash);
}

td::StringBuilder& operator<<(td::StringBuilder& stream, const CandidateId& id) {
  return stream << "{" << id.slot << ", " << id.hash << ", ?}";
}

td::StringBuilder& operator<<(td::StringBuilder& stream, const ParentId& id) {
  if (id.has_value()) {
    return stream << *id;
  } else {
    return stream << "consensus genesis";
  }
}

tl::CandidateParentRef CandidateId::parent_id_to_tl(ParentId parent) {
  if (!parent) {
    return create_tl_object<tl::candidateWithoutParents>();
  } else {
    return create_tl_object<tl::candidateParent>(parent->to_tl());
  }
}

ParentId CandidateId::tl_to_parent_id(const tl::CandidateParentRef& tl_parent) {
  ParentId id;
  auto without_parents_fn = [&](const tl::candidateWithoutParents&) {};
  auto parent_fn = [&](const tl::candidateParent& parent) { id = CandidateId::from_tl(parent.id_); };
  ton_api::downcast_call(*tl_parent, td::overloaded(without_parents_fn, parent_fn));
  return id;
}

CandidateHashData CandidateHashData::from_tl(tl::CandidateHashData&& data) {
  CandidateHashData builder;

  auto empty_fn = [&](const tl::candidateHashDataEmpty& empty) {
    builder = CandidateHashData::create_empty(create_block_id(empty.block_), CandidateId::from_tl(empty.parent_));
  };
  auto ordinary_fn = [&](const tl::candidateHashDataOrdinary& full) {
    FullCandidate candidate{create_block_id(full.block_), full.collated_file_hash_};
    builder = CandidateHashData::create_full(candidate, CandidateId::tl_to_parent_id(full.parent_));
  };
  ton_api::downcast_call(data, td::overloaded(empty_fn, ordinary_fn));

  return builder;
}

BlockIdExt CandidateHashData::block() const {
  auto empty_fn = [&](const EmptyCandidate& empty) { return empty.reference; };
  auto full_fn = [&](const FullCandidate& full) { return full.id; };
  return std::visit(td::overloaded(empty_fn, full_fn), candidate);
}

CandidateId CandidateHashData::build_id_with(td::uint32 slot) const {
  return CandidateId{slot, get_tl_object_sha_bits256(to_tl())};
}

tl::CandidateHashDataRef CandidateHashData::to_tl() const {
  auto empty_fn = [&](const EmptyCandidate& empty) -> tl::CandidateHashDataRef {
    return create_tl_object<tl::candidateHashDataEmpty>(create_tl_block_id(empty.reference), parent->to_tl());
  };
  auto full_fn = [&](const FullCandidate& full) -> tl::CandidateHashDataRef {
    return create_tl_object<tl::candidateHashDataOrdinary>(create_tl_block_id(full.id), full.collated_file_hash,
                                                           CandidateId::parent_id_to_tl(parent));
  };
  return std::visit(td::overloaded(empty_fn, full_fn), candidate);
}

td::Result<CandidateRef> Candidate::deserialize(td::Slice data, const Bus& bus, std::optional<PeerValidatorId> src) {
  TRY_RESULT(broadcast, fetch_tl_object<tl::CandidateData>(data, true));

  struct ExtractedData {
    td::uint32 slot;
    ParentId parent_id;
    std::variant<BlockIdExt, BlockCandidate> block;
    td::BufferSlice signature;
    CandidateHashData hash_builder;
  };
  td::Result<ExtractedData> maybe_data;

  PeerValidator leader;
  auto set_check_leader = [&](td::uint32 slot) -> td::Status {
    leader = bus.collator_schedule->expected_collator_for(slot).get_using(bus);
    if (leader.idx != src.value_or(leader.idx)) {
      return td::Status::Error("Candidate broadcast source does not match expected leader");
    }
    return td::Status::OK();
  };

  auto empty_fn = [&](tl::empty& empty_broadcast) -> td::Result<ExtractedData> {
    auto slot = static_cast<td::uint32>(empty_broadcast.slot_);
    TRY_STATUS(set_check_leader(slot));

    auto block = create_block_id(empty_broadcast.block_);
    auto parent = CandidateId::from_tl(empty_broadcast.parent_);
    return ExtractedData{
        .slot = slot,
        .parent_id = parent,
        .block = block,
        .signature = std::move(empty_broadcast.signature_),
        .hash_builder = CandidateHashData::create_empty(block, parent),
    };
  };

  auto ordinary_fn = [&](tl::block& block_broadcast) -> td::Result<ExtractedData> {
    auto slot = static_cast<td::uint32>(block_broadcast.slot_);
    TRY_STATUS(set_check_leader(slot));

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

    auto parent = CandidateId::tl_to_parent_id(block_broadcast.parent_);
    auto hash_builder = CandidateHashData::create_full(block, parent);
    return ExtractedData{
        .slot = slot,
        .parent_id = parent,
        .block = std::move(block),
        .signature = std::move(block_broadcast.signature_),
        .hash_builder = hash_builder,
    };
  };

  ton_api::downcast_call(*broadcast,
                         [&](auto& broadcast) { maybe_data = td::overloaded(empty_fn, ordinary_fn)(broadcast); });
  TRY_RESULT(parsed, std::move(maybe_data));

  auto id = parsed.hash_builder.build_id_with(parsed.slot);

  auto signed_data = serialize_tl_object(id.to_tl(), true);
  if (!leader.check_signature(bus.session_id, signed_data, parsed.signature)) {
    return td::Status::Error("Candidate broadcast signature is not valid");
  }

  return td::make_ref<Candidate>(id, parsed.parent_id, leader.idx, std::move(parsed.block),
                                 std::move(parsed.signature));
}

BlockIdExt Candidate::block_id() const {
  auto empty_fn = [&](const BlockIdExt& referenced_block) { return referenced_block; };
  auto full_fn = [&](const BlockCandidate& candidate) { return candidate.id; };
  return std::visit(td::overloaded(empty_fn, full_fn), block);
}

CandidateHashData Candidate::hash_data() const {
  auto empty_fn = [&](const BlockIdExt& referenced_block) {
    return CandidateHashData::create_empty(referenced_block, parent_id.value());
  };
  auto block_fn = [&](const BlockCandidate& candidate) { return CandidateHashData::create_full(candidate, parent_id); };
  return std::visit(td::overloaded(empty_fn, block_fn), block);
}

td::BufferSlice Candidate::serialize() const {
  auto empty_fn = [&](const BlockIdExt& referenced_block) {
    return create_serialize_tl_object<tl::empty>(id.slot, parent_id->to_tl(), create_tl_block_id(referenced_block),
                                                 signature.clone());
  };
  auto block_fn = [&](const BlockCandidate& candidate) {
    auto candidate_tl = create_tl_object<ton_api::validatorSession_candidate>(
        td::Bits256{}, candidate.id.seqno(), candidate.id.root_hash, candidate.data.clone(),
        candidate.collated_data.clone());

    return create_serialize_tl_object<tl::block>(id.slot, CandidateId::parent_id_to_tl(parent_id),
                                                 validatorsession::serialize_candidate(candidate_tl, true).move_as_ok(),
                                                 signature.clone());
  };
  return std::visit(td::overloaded(empty_fn, block_fn), block);
}

bool Candidate::is_empty() const {
  return std::holds_alternative<BlockIdExt>(block);
}

stats::Event::Event() : ts_(td::Clocks::system()) {
}

}  // namespace ton::validator::consensus
