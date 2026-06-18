/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include <tuple>

#include "auto/tl/ton_api_json.h"
#include "td/utils/deref.h"
#include "tl/tl_json.h"
#include "ton/ton-io.hpp"

#include "bus.h"

namespace ton::validator::consensus {

namespace {

std::string block_candidate_to_string(const BlockCandidate& candidate) {
  return PSTRING() << "BlockCandidate{id=" << candidate.id << ", block_size=" << candidate.data.size()
                   << ", collated_size=" << candidate.collated_data.size()
                   << ", collated_file_hash=" << candidate.collated_file_hash
                   << ", pubkey=" << candidate.pubkey.as_bits256() << "}";
}

std::string candidate_to_string(const CandidateRef& candidate) {
  auto block_fn = [](const BlockCandidate& block) { return block_candidate_to_string(block); };
  auto empty_fn = [](const BlockIdExt& id) { return PSTRING() << id << " (referenced)"; };

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

bool Start::operator==(const Start& other) const {
  return td::deref(state) == td::deref(other.state);
}

std::string Start::contents_to_string() const {
  return PSTRING() << "{state=" << state << "}";
}

bool FinalizeBlock::operator==(const FinalizeBlock& other) const {
  auto k = [](const FinalizeBlock& v) { return std::make_tuple(td::deref(v.candidate), std::ref(v.signatures)); };
  return k(*this) == k(other);
}

std::string FinalizeBlock::contents_to_string() const {
  return PSTRING() << "{candidate=" << candidate_to_string(candidate)
                   << ", signatures=" << block_signature_set_to_string(signatures) << "}";
}

bool OurLeaderWindowStarted::operator==(const OurLeaderWindowStarted& other) const {
  auto k = [](const OurLeaderWindowStarted& v) {
    return std::make_tuple(std::ref(v.base), td::deref(v.state), v.start_slot, v.end_slot, v.start_time);
  };
  return k(*this) == k(other);
}

std::string OurLeaderWindowStarted::contents_to_string() const {
  return PSTRING() << "{base=" << base << ", state=" << state << ", start_slot=" << start_slot
                   << ", end_slot=" << end_slot << ", start_time=" << start_time.at_unix() << "}";
}

bool CandidateGenerated::operator==(const CandidateGenerated& other) const {
  auto k = [](const CandidateGenerated& v) { return std::make_tuple(td::deref(v.candidate), std::ref(v.collator_id)); };
  return k(*this) == k(other);
}

std::string CandidateGenerated::contents_to_string() const {
  return PSTRING() << "{candidate=" << candidate_to_string(candidate)
                   << ", collator_id=" << (collator_id.has_value() ? (PSTRING() << *collator_id) : "none") << "}";
}

bool CandidateReceived::operator==(const CandidateReceived& other) const {
  return td::deref(candidate) == td::deref(other.candidate);
}

std::string CandidateReceived::contents_to_string() const {
  return PSTRING() << "{candidate=" << candidate_to_string(candidate) << "}";
}

bool ValidationRequest::operator==(const ValidationRequest& other) const {
  auto k = [](const ValidationRequest& v) { return std::make_tuple(td::deref(v.state), std::ref(v.candidate)); };
  return k(*this) == k(other);
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
  return PSTRING() << "{block=" << block << "}";
}

bool MisbehaviorReport::operator==(const MisbehaviorReport& other) const {
  auto k = [](const MisbehaviorReport& v) { return std::make_tuple(std::ref(v.id), td::deref(v.proof)); };
  return k(*this) == k(other);
}

std::string MisbehaviorReport::contents_to_string() const {
  return PSTRING() << "{id=" << id << "}";
}

bool TraceEvent::operator==(const TraceEvent& other) const {
  return event ? other.event && event->equals(*other.event) : !other.event;
}

std::string TraceEvent::contents_to_string() const {
  return PSTRING() << "{event=" << event->to_string() << "}";
}

std::string NoncriticalParamsUpdated::contents_to_string() const {
  td::StringBuilder sb;
#define APPEND_PARAM(_, name, value) sb << #name << "=" << params.name << ", ";
#define APPEND_DURATION(_, name, value) sb << #name << "=" << params.name.count() << "ms, ";
  ENUMERATE_NONCRITICAL_PARAMS(APPEND_PARAM, APPEND_PARAM, APPEND_DURATION)
#undef APPEND_PARAM
#undef APPEND_DURATION
  return PSTRING() << "{params={" << td::Slice{sb.as_cslice()}.remove_suffix(2) << "}}";
}

std::string PrecheckCandidateBroadcast::contents_to_string() const {
  return PSTRING() << "{slot=" << slot << ", broadcast_id=" << broadcast_id
                   << ", signature_checked=" << signature_checked << "}";
}

}  // namespace ton::validator::consensus
