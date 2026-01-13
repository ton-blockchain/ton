/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "auto/tl/ton_api_json.h"
#include "tl/tl_json.h"

#include "bus.h"

namespace ton::validator::consensus {

namespace {

std::string block_candidate_to_string(const BlockCandidate& candidate) {
  return PSTRING() << "BlockCandidate{id=" << candidate.id.to_str() << ", block_size=" << candidate.data.size()
                   << ", collated_size=" << candidate.collated_data.size()
                   << ", collated_file_hash=" << candidate.collated_file_hash
                   << ", pubkey=" << candidate.pubkey.as_bits256() << "}";
}

std::string candidate_to_string(const td::OneOf<RawCandidateRef, CandidateRef> auto& candidate) {
  auto block_fn = [](const BlockCandidate& block) { return block_candidate_to_string(block); };
  auto empty_fn = [](const BlockIdExt& id) { return PSTRING() << id.to_str() << " (referenced)"; };

  return PSTRING() << "Candidate{id=" << candidate->id << ", parent=" << candidate->parent_id
                   << ", leader=" << candidate->leader
                   << ", block=" << std::visit(td::overloaded(block_fn, empty_fn), candidate->block) << "}";
}

std::string message_to_string(td::Slice message) {
  auto maybe_decoded = fetch_tl_object<ton_api::Object>(message, true);
  if (maybe_decoded.is_error()) {
    return PSTRING() << "<message of size " << message.size() << ">";
  }

  return td::json_encode<std::string>(td::ToJson(maybe_decoded.ok()));
}

std::string block_signature_set_to_string(const td::Ref<block::BlockSignatureSet>& set) {
  return PSTRING() << "<BlockSignatureSet size=" << set->get_size() << " final=" << set->is_final()
                   << " ordinary=" << set->is_ordinary() << ">";
}

}  // namespace

std::string BlockFinalized::contents_to_string() const {
  return PSTRING() << "{candidate=" << candidate_to_string(candidate)
                   << ", signatures=" << block_signature_set_to_string(signatures) << "}";
}

std::string OurLeaderWindowStarted::contents_to_string() const {
  return PSTRING() << "{base=" << base << ", start_slot=" << start_slot << ", end_slot=" << end_slot << "}";
}

std::string OurLeaderWindowAborted::contents_to_string() const {
  return PSTRING() << "{start_slot=" << start_slot << "}";
}

std::string CandidateGenerated::contents_to_string() const {
  return PSTRING() << "{candidate=" << candidate_to_string(candidate)
                   << ", collator_id=" << (collator_id.has_value() ? (PSTRING() << *collator_id) : "none") << "}";
}

std::string CandidateReceived::contents_to_string() const {
  return PSTRING() << "{candidate=" << candidate_to_string(candidate) << "}";
}

std::string ValidationRequest::contents_to_string() const {
  return PSTRING() << "{candidate=" << candidate_to_string(candidate) << "}";
}

std::string IncomingProtocolMessage::contents_to_string() const {
  return PSTRING() << "{source=" << source << ", message=" << message_to_string(message.data) << "}";
}

std::string OutgoingProtocolMessage::contents_to_string() const {
  return PSTRING() << "{recipient=" << (recipient.has_value() ? (PSTRING() << *recipient) : "broadcast")
                   << ", message=" << message_to_string(message.data) << "}";
}

std::string BlockFinalizedInMasterchain::contents_to_string() const {
  return PSTRING() << "{block=" << block.to_str() << "}";
}

std::string StatsTargetReached::contents_to_string() const {
  auto targets = std::to_array<const char*>({
      "CollateStarted",
      "CollateFinished",
      "CandidateReceived",
      "ValidateStarted",
      "ValidateFinished",
      "NotarObserved",
      "FinalObserved",
  });
  return PSTRING() << "{target=" << targets[target] << ", slot=" << slot << ", timestamp=" << timestamp.at() << "}";
}

std::vector<BlockIdExt> Bus::convert_id_to_blocks(ParentId parent) const {
  if (parent.has_value()) {
    return {parent->block};
  } else {
    return first_block_parents;
  }
}

std::optional<td::BufferSlice> Bus::db_get(td::Slice key) const {
  std::string value;
  auto result = db_reader->get(key, value).ensure().move_as_ok();
  if (result == td::KeyValueReader::GetStatus::Ok) {
    return td::BufferSlice(value);
  }
  return std::nullopt;
}

std::vector<std::pair<td::BufferSlice, td::BufferSlice>> Bus::db_get_by_prefix(td::uint32 prefix) const {
  td::uint32 prefix2 = prefix + 1;
  td::Slice begin{(const char*)&prefix, 4};
  td::Slice end{(const char*)&prefix2, 4};
  std::vector<std::pair<td::BufferSlice, td::BufferSlice>> result;
  db_reader
      ->for_each_in_range(begin, end,
                          [&](td::Slice key, td::Slice value) -> td::Status {
                            result.emplace_back(key, value);
                            return td::Status::OK();
                          })
      .ensure();
  return result;
}

}  // namespace ton::validator::consensus
