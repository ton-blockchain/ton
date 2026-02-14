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

std::string candidate_to_string(const CandidateRef& candidate) {
  auto block_fn = [](const BlockCandidate& block) { return block_candidate_to_string(block); };
  auto empty_fn = [](const BlockIdExt& id) { return PSTRING() << id.to_str() << " (referenced)"; };

  return PSTRING() << "Candidate{id=" << candidate->id << ", parent=" << candidate->parent_id
                   << ", leader=" << candidate->leader
                   << ", block=" << std::visit(td::overloaded(block_fn, empty_fn), candidate->block) << "}";
}

std::string message_to_string(const ProtocolMessage& message) {
  constexpr size_t max_size_for_json = 1024;
  constexpr size_t max_size_for_hex_dump = 256;

  td::Slice data = message.data;

  if (data.size() <= max_size_for_json) {
    auto maybe_decoded = fetch_tl_object<ton_api::Object>(data, true);
    if (maybe_decoded.is_ok()) {
      return td::json_encode<std::string>(td::ToJson(maybe_decoded.ok()));
    }
  }

  if (data.size() <= max_size_for_hex_dump) {
    return PSTRING() << td::format::as_hex_dump<0>(data);
  } else {
    return PSTRING() << td::format::as_hex_dump<0>(data.substr(0, max_size_for_json)) << "... (truncated "
                     << (data.size() - max_size_for_json) << " bytes)";
  }
}

std::string block_signature_set_to_string(const td::Ref<block::BlockSignatureSet>& set) {
  return PSTRING() << "<BlockSignatureSet size=" << set->get_size() << " final=" << set->is_final()
                   << " ordinary=" << set->is_ordinary() << ">";
}

}  // namespace

std::string Start::contents_to_string() const {
  return PSTRING() << "{state=" << state << "}";
}

std::string FinalizeBlock::contents_to_string() const {
  return PSTRING() << "{candidate=" << candidate_to_string(candidate)
                   << ", signatures=" << block_signature_set_to_string(signatures) << "}";
}

std::string OurLeaderWindowStarted::contents_to_string() const {
  return PSTRING() << "{base=" << base << ", state=" << state << ", start_slot=" << start_slot
                   << ", end_slot=" << end_slot << ", start_time=" << start_time.at_unix() << "}";
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
  return PSTRING() << "{state=" << state << ", candidate=" << candidate_to_string(candidate) << "}";
}

std::string ValidationRequest::response_to_string(const ReturnType& result) {
  std::string str;
  auto accept_fn = [&](const CandidateAccept& accept) { str = PSTRING() << "CandidateAccept{}"; };
  auto reject_fn = [&](const CandidateReject& reject) {
    str = PSTRING() << "CandidateReject{reason=" << reject.reason << "}";
  };
  result.visit(td::overloaded(accept_fn, reject_fn));
  return str;
}

std::string IncomingProtocolMessage::contents_to_string() const {
  return PSTRING() << "{source=" << source << ", message=" << message_to_string(message) << "}";
}

std::string OutgoingProtocolMessage::contents_to_string() const {
  return PSTRING() << "{recipient=" << (recipient.has_value() ? (PSTRING() << *recipient) : "broadcast")
                   << ", message=" << message_to_string(message) << "}";
}

std::string IncomingOverlayRequest::contents_to_string() const {
  return PSTRING() << "{source=" << source << ", request=" << message_to_string(request) << "}";
}

std::string IncomingOverlayRequest::response_to_string(const ReturnType& response) {
  return PSTRING() << message_to_string(response);
}

std::string OutgoingOverlayRequest::contents_to_string() const {
  return PSTRING() << "{destination=" << destination << ", timeout=" << timeout.in()
                   << " remaining, request=" << message_to_string(request) << "}";
}

std::string OutgoingOverlayRequest::response_to_string(const ReturnType& response) {
  return PSTRING() << message_to_string(response);
}

std::string BlockFinalizedInMasterchain::contents_to_string() const {
  return PSTRING() << "{block=" << block.to_str() << "}";
}

std::string MisbehaviorReport::contents_to_string() const {
  return PSTRING() << "{id=" << id << "}";
}

std::string TraceEvent::contents_to_string() const {
  return PSTRING() << "{event=" << event->to_string() << "}";
}

}  // namespace ton::validator::consensus
