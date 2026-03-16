/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "bus.h"

namespace ton::validator::consensus::simplex {

namespace {

std::string certificate_to_string(const CertificateRef<Vote> &cert) {
  std::string ids;
  for (const auto &signature : cert->signatures) {
    if (!ids.empty()) {
      ids += ",";
    }
    ids += std::to_string(signature.validator.value());
  }

  return PSTRING() << "Certificate{vote=" << cert->vote << ", signatures=[" << ids << "]}";
}

}  // namespace

std::string BroadcastVote::contents_to_string() const {
  return PSTRING() << "{vote=" << vote << "}";
}

std::string NotarizationObserved::contents_to_string() const {
  return PSTRING() << "{id=" << id << "}";
}

std::string FinalizationObserved::contents_to_string() const {
  return PSTRING() << "{id=" << id << "}";
}

std::string LeaderWindowObserved::contents_to_string() const {
  return PSTRING() << "{start_slot=" << start_slot << ", base=" << base << "}";
}

std::string WaitForParent::contents_to_string() const {
  return PSTRING() << "{id=" << candidate->id << ", parent=" << candidate->parent_id << "}";
}

std::string ResolveCandidate::contents_to_string() const {
  return PSTRING() << "{id=" << id << "}";
}

std::string StoreCandidate::contents_to_string() const {
  return PSTRING() << "{id=" << candidate->id << "}";
}

std::string ResolveState::contents_to_string() const {
  return PSTRING() << "{id=" << id << "}";
}

std::string ResolveState::response_to_string(const ReturnType &result) {
  return PSTRING() << "ResolvedState{state=" << *result.state << ", gen_utime_exact="
                   << (result.gen_utime_exact ? (PSTRING() << *result.gen_utime_exact) : "nullopt") << "}";
}

std::string SaveCertificate::contents_to_string() const {
  return PSTRING() << "{cert=" << certificate_to_string(cert) << "}";
}

namespace {

class SimplexCollatorSchedule : public CollatorSchedule {
 public:
  SimplexCollatorSchedule(td::uint32 slots_per_leader_window, td::uint32 leaders_count)
      : slots_per_leader_window_(slots_per_leader_window), leaders_count_(leaders_count) {
  }

  PeerValidatorId expected_collator_for(td::uint32 slot) const override {
    return PeerValidatorId{slot / slots_per_leader_window_ % leaders_count_};
  }

 private:
  td::uint32 slots_per_leader_window_;
  td::uint32 leaders_count_;
};

}  // namespace

void Bus::populate_collator_schedule() {
  auto validators = static_cast<td::uint32>(validator_set.size());
  collator_schedule = td::make_ref<SimplexCollatorSchedule>(simplex_config.slots_per_leader_window, validators);
}

}  // namespace ton::validator::consensus::simplex
