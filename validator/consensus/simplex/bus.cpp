/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include <tuple>

#include "td/utils/deref.h"

#include "bus.h"

namespace ton::validator::consensus::simplex {

namespace {

std::string certificate_to_string(const CertificateRef<Vote>& cert) {
  std::string ids;
  for (const auto& signature : cert->signatures) {
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

bool NotarizationObserved::operator==(const NotarizationObserved& other) const {
  auto k = [](const NotarizationObserved& v) { return std::make_tuple(std::ref(v.id), td::deref(v.certificate)); };
  return k(*this) == k(other);
}

std::string NotarizationObserved::contents_to_string() const {
  return PSTRING() << "{id=" << id << "}";
}

bool FinalizationObserved::operator==(const FinalizationObserved& other) const {
  auto k = [](const FinalizationObserved& v) { return std::make_tuple(std::ref(v.id), td::deref(v.certificate)); };
  return k(*this) == k(other);
}

std::string FinalizationObserved::contents_to_string() const {
  return PSTRING() << "{id=" << id << "}";
}

std::string LeaderWindowObserved::contents_to_string() const {
  return PSTRING() << "{start_slot=" << start_slot << ", base=" << base << "}";
}

bool WaitForParent::operator==(const WaitForParent& other) const {
  return td::deref(candidate) == td::deref(other.candidate);
}

std::string WaitForParent::contents_to_string() const {
  return PSTRING() << "{id=" << candidate->id << ", parent=" << candidate->parent_id << "}";
}

std::string ResolveCandidate::contents_to_string() const {
  return PSTRING() << "{id=" << id << "}";
}

bool StoreCandidate::operator==(const StoreCandidate& other) const {
  return td::deref(candidate) == td::deref(other.candidate);
}

std::string StoreCandidate::contents_to_string() const {
  return PSTRING() << "{id=" << candidate->id << "}";
}

std::string ResolveState::contents_to_string() const {
  return PSTRING() << "{id=" << id << "}";
}

std::string ResolveState::response_to_string(const ReturnType& result) {
  return PSTRING() << "ResolvedState{state=" << *result.state << ", gen_utime_exact="
                   << (result.gen_utime_exact ? (PSTRING() << *result.gen_utime_exact) : "nullopt") << "}";
}

bool SaveCertificate::operator==(const SaveCertificate& other) const {
  return td::deref(cert) == td::deref(other.cert);
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
  collator_schedule = td::make_ref<SimplexCollatorSchedule>(config.slots_per_leader_window, validators);
}

}  // namespace ton::validator::consensus::simplex
