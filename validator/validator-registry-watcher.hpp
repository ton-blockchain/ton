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

class ValidatorRegistryState {
 public:
  explicit ValidatorRegistryState(block::ValidatorRegistryConfig config) : config_(std::move(config)) {
  }
  virtual ~ValidatorRegistryState() = default;
  virtual std::set<adnl::AdnlNodeIdShort> get_all_collators(Ref<MasterchainState> mc_state) = 0;

  const block::ValidatorRegistryConfig& get_config() const {
    return config_;
  }

  virtual void for_each_node(std::function<void(adnl::AdnlNodeIdShort, td::uint32)> f) {
  }

  static void scan_nodes_diff(ValidatorRegistryState& prev, ValidatorRegistryState& cur,
                              std::function<void(adnl::AdnlNodeIdShort, td::uint32, td::uint32)> f);

  static std::unique_ptr<ValidatorRegistryState> fetch(Ref<MasterchainState> mc_state);
  static std::unique_ptr<ValidatorRegistryState> create_empty();

  static constexpr td::uint32 ROLE_COLLATOR = 1;
  static constexpr td::uint32 ROLE_PROTECTED_OVERLAY = 2;

 protected:
  block::ValidatorRegistryConfig config_;

  virtual vm::Dictionary get_nodes_dict() {
    return vm::Dictionary{256};
  }
};

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
