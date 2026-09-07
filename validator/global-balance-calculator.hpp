/*
* Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#pragma once
#include "interfaces/validator-manager.h"
#include "td/actor/actor.h"

namespace ton::validator {

class GlobalBalanceCalculator : public td::actor::Actor {
 public:
  virtual td::actor::Task<td::RefInt256> validate_global_balance(Ref<MasterchainState> mc_state,
                                                                 Ref<vm::Cell> block_root,
                                                                 td::CancellationToken cancellation_token) = 0;

  static td::actor::ActorOwn<GlobalBalanceCalculator> create(BlockIdExt start_mc_block,
                                                             td::actor::ActorId<ValidatorManager> manager,
                                                             std::unique_ptr<GarbageCollectorBlocker> gc_blocker);
};

}  // namespace ton::validator
