/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#pragma once

#include "consensus/bus.h"

#include "certificate.h"

namespace ton::validator::consensus::simplex {

struct BroadcastVote {
  using ReturnType = td::Unit;

  Vote vote;

  bool operator==(const BroadcastVote&) const = default;
  std::string contents_to_string() const;
};

struct NotarizationObserved {
  CandidateId id;
  NotarCertRef certificate;

  bool operator==(const NotarizationObserved&) const = default;
  std::string contents_to_string() const;
};

struct FinalizationObserved {
  CandidateId id;
  FinalCertRef certificate;

  bool operator==(const FinalizationObserved&) const = default;
  std::string contents_to_string() const;
};

struct LeaderWindowObserved {
  using ReturnType = td::Unit;

  td::uint32 start_slot;
  ParentId base;

  bool operator==(const LeaderWindowObserved&) const = default;
  std::string contents_to_string() const;
};

struct WaitForParent {
  using ReturnType = std::optional<MisbehaviorRef>;

  CandidateRef candidate;

  bool operator==(const WaitForParent&) const = default;
  std::string contents_to_string() const;
};

struct ResolveCandidate {
  struct Result {
    CandidateRef candidate;
    NotarCertRef notar;
  };

  using ReturnType = Result;

  CandidateId id;

  bool operator==(const ResolveCandidate&) const = default;
  std::string contents_to_string() const;
};

struct StoreCandidate {
  using ReturnType = td::Unit;

  CandidateRef candidate;

  bool operator==(const StoreCandidate&) const = default;
  std::string contents_to_string() const;
};

struct ResolveState {
  struct Result {
    ChainStateRef state;
    std::optional<double> gen_utime_exact = std::nullopt;
  };

  using ReturnType = Result;

  ParentId id;

  bool operator==(const ResolveState&) const = default;
  std::string contents_to_string() const;
  static std::string response_to_string(const ReturnType&);
};

struct SaveCertificate {
  using ReturnType = td::Unit;

  CertificateRef<Vote> cert;

  std::string contents_to_string() const;
};

class Bus : public consensus::Bus {
 public:
  using Parent = consensus::Bus;
  using Events = td::TypeList<BroadcastVote, NotarizationObserved, FinalizationObserved, LeaderWindowObserved,
                              WaitForParent, ResolveCandidate, StoreCandidate, ResolveState, SaveCertificate>;

  Bus() = default;

  void populate_collator_schedule() override;

  std::vector<CertificateRef<Vote>> bootstrap_certificates;
  std::vector<Vote> bootstrap_votes;

  td::uint32 first_nonannounced_window = 0;
};

using BusHandle = td::actor::BusHandle<Bus>;

struct Pool {
  static void register_in(td::actor::Runtime&);
};

struct Consensus {
  static void register_in(td::actor::Runtime&);
};

struct CandidateResolver {
  static void register_in(td::actor::Runtime&);
};

struct StateResolver {
  static void register_in(td::actor::Runtime&);
};

struct MetricCollector {
  static void register_in(td::actor::Runtime&);
};

struct Db {
  static void register_in(td::actor::Runtime&);
};

}  // namespace ton::validator::consensus::simplex
