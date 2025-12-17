/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#pragma once

#include "consensus/bus.h"

#include "votes.h"

namespace ton::validator::consensus::simplex {

struct BroadcastVote {
  Vote vote;

  std::string contents_to_string() const;
};

struct NotarizationObserved {
  RawCandidateId id;
  NotarCertRef certificate;

  std::string contents_to_string() const;
};

struct FinalizationObserved {
  RawCandidateId id;
  FinalCertRef certificate;

  std::string contents_to_string() const;
};

struct LeaderWindowObserved {
  td::uint32 start_slot;
  RawParentId base;

  std::string contents_to_string() const;
};

struct WaitForParent {
  using ReturnType = std::optional<MisbehaviorRef>;

  RawCandidateRef candidate;

  std::string contents_to_string() const;
};

struct ResolveCandidate {
  using ReturnType = CandidateId;

  RawCandidateId id;

  std::string contents_to_string() const;
};

class Bus : public consensus::Bus {
 public:
  using Parent = consensus::Bus;
  using Events = td::TypeList<BroadcastVote, NotarizationObserved, FinalizationObserved, LeaderWindowObserved,
                              WaitForParent, ResolveCandidate>;

  Bus() = default;

  NewConsensusConfig::Simplex simplex_config;

  // FIXME: These should come from validator options
  double max_backoff_delay_s = 100;
  double timeout_increase_factor = 1.05;
  double standstill_timeout_s = 10;
};

using BusHandle = runtime::BusHandle<Bus>;

struct Pool {
  static void register_in(runtime::Runtime&);
};

struct Consensus {
  static void register_in(runtime::Runtime&);
};

struct CandidateResolver {
  static void register_in(runtime::Runtime&);
};

}  // namespace ton::validator::consensus::simplex
