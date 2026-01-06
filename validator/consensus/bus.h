/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#pragma once

#include "consensus/misbehavior.h"
#include "keyring/keyring.hpp"
#include "overlay/overlays.h"
#include "ton/ton-types.h"

#include "manager-facade.h"
#include "runtime.h"
#include "types.h"

namespace ton::validator::consensus {

struct StopRequested {};

struct BlockFinalized {
  using ReturnType = td::Unit;

  CandidateRef candidate;
  td::Ref<block::BlockSignatureSet> signatures;

  std::string contents_to_string() const;
};

struct OurLeaderWindowStarted {
  ParentId base;
  td::uint32 start_slot;
  td::uint32 end_slot;

  std::string contents_to_string() const;
};

struct OurLeaderWindowAborted {
  td::uint32 start_slot;

  std::string contents_to_string() const;
};

struct CandidateGenerated {
  RawCandidateRef candidate;
  std::optional<adnl::AdnlNodeIdShort> collator_id;

  std::string contents_to_string() const;
};

// The only guarantee is that the candidate has a valid signature from `candidate->leader`.
struct CandidateReceived {
  RawCandidateRef candidate;

  std::string contents_to_string() const;
};

// Checks that if candidate contains a block, then BlockCandidate is a valid block built on top of
// the parent. Note that empty blocks are always (locally) valid because of an assert in Candidate
// constructor.
struct ValidationRequest {
  using ReturnType = td::Unit;

  CandidateRef candidate;

  std::string contents_to_string() const;
};

struct IncomingProtocolMessage {
  PeerValidatorId source;
  ProtocolMessage message;

  std::string contents_to_string() const;
};

struct OutgoingProtocolMessage {
  std::optional<PeerValidatorId> recipient;
  ProtocolMessage message;

  std::string contents_to_string() const;
};

struct BlockFinalizedInMasterchain {
  BlockIdExt block;

  std::string contents_to_string() const;
};

struct MisbehaviorReport {
  PeerValidatorId id;
  MisbehaviorRef proof;
};

struct StatsTargetReached {
  enum Target {
    CollateStarted,
    CollateFinished,
    CandidateReceived,
    ValidateStarted,
    ValidateFinished,
    NotarObserved,
    FinalObserved,
  };

  StatsTargetReached(Target target, td::uint32 slot) : target(target), slot(slot), timestamp(td::Timestamp::now()) {
  }

  Target target;
  td::uint32 slot;
  td::Timestamp timestamp;

  std::string contents_to_string() const;
};

class Bus : public runtime::Bus {
 public:
  using Events =
      td::TypeList<StopRequested, BlockFinalized, OurLeaderWindowStarted, OurLeaderWindowAborted, CandidateGenerated,
                   CandidateReceived, ValidationRequest, IncomingProtocolMessage, OutgoingProtocolMessage,
                   BlockFinalizedInMasterchain, MisbehaviorReport, StatsTargetReached>;

  Bus() = default;

  std::vector<BlockIdExt> convert_id_to_blocks(ParentId parent) const;

  ValidatorSessionId session_id;

  ShardIdFull shard;
  td::actor::ActorId<ManagerFacade> manager;
  td::actor::ActorId<keyring::Keyring> keyring;
  td::Ref<ValidatorManagerOptions> validator_opts;

  std::vector<PeerValidator> validator_set;
  ValidatorWeight total_weight;
  ton::CatchainSeqno cc_seqno;
  td::uint32 validator_set_hash;
  PeerValidator local_id;

  NewConsensusConfig config;
  BlockIdExt min_masterchain_block_id;

  td::actor::ActorId<overlay::Overlays> overlays;

  std::vector<BlockIdExt> first_block_parents;
};

using BusHandle = runtime::BusHandle<Bus>;

struct BlockAccepter {
  static void register_in(runtime::Runtime&);
};

struct BlockProducer {
  static void register_in(runtime::Runtime&);
};

struct BlockValidator {
  static void register_in(runtime::Runtime&);
};

struct PrivateOverlay {
  static void register_in(runtime::Runtime&);
};

struct StatsCollector {
  static void register_in(runtime::Runtime&);
};

}  // namespace ton::validator::consensus
