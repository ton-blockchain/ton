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
  static td::actor::ActorOwn<GlobalBalanceCalculator> create(BlockIdExt start_mc_block,
                                                             td::actor::ActorId<ValidatorManager> manager);
};

}  // namespace ton::validator
