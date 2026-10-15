/*
* Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#pragma once
#include <functional>
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

// Compute one historical masterchain snapshot without a running validator manager.
// shard_states must contain exactly the top shard states referenced by mc_state
// (excluding the masterchain state), paired with their corresponding block roots.
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
