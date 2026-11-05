/*
* Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#pragma once
#include <functional>
#include <memory>
#include <vector>

#include "interfaces/validator-manager.h"
#include "td/actor/actor.h"

namespace ton::validator {

struct GlobalBalanceShardState {
  Ref<ShardState> state;
  Ref<vm::Cell> block_root;
};

struct GlobalBalanceComponents {
  // All amounts are exact grams in nanotons. Only the first four add to total.
  td::RefInt256 accounts = td::zero_refint();
  td::RefInt256 dispatch_queue = td::zero_refint();
  td::RefInt256 unprocessed_messages = td::zero_refint();
  td::RefInt256 unrecovered_validator_fees = td::zero_refint();
  td::RefInt256 total = td::zero_refint();
  // ValueFlow.burned of this MC block, for validating a neighboring snapshot.
  td::RefInt256 burned = td::zero_refint();
};

using LoadAuxMasterchainState = std::function<td::actor::Task<Ref<MasterchainState>>(BlockIdExt)>;

// A reusable, immutable preparation of one shard (or masterchain) state. Copies
// share queue cells, but each calculation uses its own dictionary wrappers.
// The caller controls caching/lifetime; a handle does not retain predecessors.
class PreparedGlobalBalanceState {
 public:
  PreparedGlobalBalanceState() = default;
  bool empty() const;
  const BlockIdExt& block_id() const;  // requires !empty()
  int global_version() const;          // requires !empty()

 private:
  struct Data;
  std::shared_ptr<const Data> data_;

  explicit PreparedGlobalBalanceState(std::shared_ptr<const Data> data) : data_(std::move(data)) {
  }
  friend td::actor::Task<PreparedGlobalBalanceState> prepare_global_balance_state(
      Ref<ShardState>, Ref<vm::Cell>, int, std::vector<PreparedGlobalBalanceState>);
  friend td::actor::Task<GlobalBalanceComponents> calculate_global_balance_at_anchor(
      Ref<MasterchainState>, Ref<vm::Cell>, PreparedGlobalBalanceState, std::vector<PreparedGlobalBalanceState>,
      LoadAuxMasterchainState);
};

// Without prev, prepare a standalone state (including a full scan of legacy
// queues). Otherwise prev must match the block header's immediate predecessors,
// in header order, and use the same global_version. This reuses the validator's
// OutMsgDescr updates and legacy dispatch-queue scan_diff, including split/merge
// handling. Prepare intermediate shard blocks before their successors.
// Pass the anchor MC's global version; reprepare without prev when it changes.
// Nonzero states are checked against their block's state_update. Failures leave
// all supplied handles usable, so callers can retry or start a new anchor.
td::actor::Task<PreparedGlobalBalanceState> prepare_global_balance_state(
    Ref<ShardState> state, Ref<vm::Cell> block_root, int global_version,
    std::vector<PreparedGlobalBalanceState> prev = {});

// Reuse prepared states for this exact MC snapshot. shard_states must contain
// exactly its current shard tips (excluding MC); all preparations must use the
// anchor's global version. An unchanged tip can be reused across MC anchors.
// Unprocessed-message balances are recalculated using this snapshot's
// processed_upto, even when every shard tip is cached. No predecessor/global
// total is trusted as a substitute for any component of the current snapshot.
td::actor::Task<GlobalBalanceComponents> calculate_global_balance_at_anchor(
    Ref<MasterchainState> mc_state, Ref<vm::Cell> mc_block_root, PreparedGlobalBalanceState prepared_mc_state,
    std::vector<PreparedGlobalBalanceState> shard_states, LoadAuxMasterchainState load_aux_mc_state);

// Compute one historical masterchain snapshot without a running validator manager.
// shard_states must contain exactly the top shard states referenced by mc_state
// (excluding the masterchain state), paired with their corresponding block roots.
// For nonzero seqnos, each state root must match its block's state_update.
// The callback loads auxiliary masterchain states needed to identify messages
// that have already been processed. Missing inputs fail the calculation.
td::actor::Task<GlobalBalanceComponents> calculate_global_balance_at_anchor(
    Ref<MasterchainState> mc_state, Ref<vm::Cell> mc_block_root, std::vector<GlobalBalanceShardState> shard_states,
    LoadAuxMasterchainState load_aux_mc_state);

class GlobalBalanceCalculator : public td::actor::Actor {
 public:
  virtual td::actor::Task<td::RefInt256> validate_global_balance(Ref<MasterchainState> mc_state,
                                                                 Ref<vm::Cell> block_root,
                                                                 td::CancellationToken cancellation_token) = 0;
  virtual td::actor::Task<td::RefInt256> get_global_balance(BlockIdExt mc_block_id, td::Timestamp timeout) = 0;
  virtual void on_new_shard_block(BlockIdExt block_id) = 0;

  static td::actor::ActorOwn<GlobalBalanceCalculator> create(BlockIdExt start_mc_block,
                                                             td::actor::ActorId<ValidatorManager> manager,
                                                             std::unique_ptr<GarbageCollectorBlocker> gc_blocker);
};

}  // namespace ton::validator
