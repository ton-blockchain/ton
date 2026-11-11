/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#pragma once
#include "adnl/adnl.h"
#include "interfaces/shard.h"
#include "interfaces/validator-manager.h"

namespace ton::validator {

using td::Ref;

class ValidatorRegistryWatcher : public td::actor::Actor {
 public:
  virtual void add_validator_key(PublicKeyHash key_hash) = 0;
  virtual void del_validator_key(PublicKeyHash key_hash) = 0;
  virtual void update(Ref<MasterchainState> mc_state, Ref<ValidatorManagerOptions> opts) = 0;

  static td::actor::ActorOwn<ValidatorRegistryWatcher> create(td::actor::ActorId<ValidatorManager> manager,
                                                              td::actor::ActorId<keyring::Keyring> keyring);
  static std::set<adnl::AdnlNodeIdShort> get_all_collators(Ref<MasterchainState> mc_state);
};

}  // namespace ton::validator
