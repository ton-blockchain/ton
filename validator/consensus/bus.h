/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#pragma once

#include "consensus/misbehavior.h"
#include "keyring/keyring.hpp"
#include "overlay/overlays.h"
#include "quic/quic-sender.h"
#include "rldp2/rldp.h"
#include "td/db/KeyValueAsync.h"
#include "ton/ton-types.h"

#include "chain-state.h"
#include "manager-facade.h"
#include "runtime.h"
#include "types.h"

namespace ton::validator::consensus {

struct Start {
  ChainStateRef state;

  std::string contents_to_string() const;
};

using StartEvent = std::shared_ptr<const Start>;

struct StopRequested {};

struct BlockFinalized {
  CandidateId candidate;
  bool final_signatures;

  std::string contents_to_string() const;
};

struct FinalizeBlock {
  using ReturnType = td::Unit;

  RawCandidateRef candidate;
  td::Ref<block::BlockSignatureSet> signatures;

  std::string contents_to_string() const;
};

struct OurLeaderWindowStarted {
  RawParentId base;
  ChainStateRef state;
  td::uint32 start_slot;
  td::uint32 end_slot;
  td::Timestamp start_time;

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

struct ValidationRequest {
  using ReturnType = td::Unit;

  ChainStateRef state;
  RawCandidateRef candidate;

  std::string contents_to_string() const;
};

struct IncomingProtocolMessage {
  using LogToDebug = std::true_type;

  PeerValidatorId source;
  ProtocolMessage message;

  std::string contents_to_string() const;
};

struct OutgoingProtocolMessage {
  using LogToDebug = std::true_type;

  std::optional<PeerValidatorId> recipient;
  ProtocolMessage message;

  std::string contents_to_string() const;
};

struct IncomingOverlayRequest {
  using LogToDebug = std::true_type;
  using ReturnType = ProtocolMessage;

  PeerValidatorId source;
  ProtocolMessage request;

  std::string contents_to_string() const;
  static std::string response_to_string(const ReturnType&);
};

struct OutgoingOverlayRequest {
  using LogToDebug = std::true_type;
  using ReturnType = ProtocolMessage;

  PeerValidatorId destination;
  td::Timestamp timeout;
  ProtocolMessage request;

  std::string contents_to_string() const;
  static std::string response_to_string(const ReturnType&);
};

struct BlockFinalizedInMasterchain {
  BlockIdExt block;

  std::string contents_to_string() const;
};

struct MisbehaviorReport {
  PeerValidatorId id;
  MisbehaviorRef proof;

  std::string contents_to_string() const;
};

struct TraceEvent {
  std::unique_ptr<const stats::Event> event;

  std::string contents_to_string() const;
};

class Db {
 public:
  virtual ~Db() = default;

  // Note: `get` and `get_by_prefix` use db snapshot from the start
  // `set` waits for syncing data to disk
  virtual std::optional<td::BufferSlice> get(td::Slice key) const = 0;
  virtual std::vector<std::pair<td::BufferSlice, td::BufferSlice>> get_by_prefix(td::uint32 prefix) const = 0;
  virtual td::actor::Task<> set(td::BufferSlice key, td::BufferSlice value) = 0;
};

class Bus : public runtime::Bus {
 public:
  using Events = td::TypeList<Start, StopRequested, BlockFinalized, FinalizeBlock, OurLeaderWindowStarted,
                              OurLeaderWindowAborted, CandidateGenerated, CandidateReceived, ValidationRequest,
                              IncomingProtocolMessage, OutgoingProtocolMessage, IncomingOverlayRequest,
                              OutgoingOverlayRequest, BlockFinalizedInMasterchain, MisbehaviorReport, TraceEvent>;

  Bus() = default;
  ~Bus() override {
    stop_promise.set_value(td::Unit());
  }

  virtual void populate_collator_schedule() = 0;

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

  td::Ref<CollatorSchedule> collator_schedule;

  td::actor::ActorId<overlay::Overlays> overlays;
  td::actor::ActorId<rldp2::Rldp> rldp2;
  td::actor::ActorId<quic::QuicSender> quic;
  std::unique_ptr<Db> db;

  td::Promise<td::Unit> stop_promise;
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

struct TraceCollector {
  static void register_in(runtime::Runtime&);
};

}  // namespace ton::validator::consensus
