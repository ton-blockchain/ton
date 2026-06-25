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
  explicit ValidatorRegistryWatcher(PublicKeyHash key_hash, td::actor::ActorId<ValidatorManager> manager,
                                    td::actor::ActorId<keyring::Keyring> keyring)
      : key_hash_(key_hash), manager_(manager), keyring_(keyring) {
  }

  void update(Ref<MasterchainState> mc_state, Ref<ValidatorManagerOptions> opts);

  static std::vector<adnl::AdnlNodeIdShort> get_all_collators(Ref<MasterchainState> mc_state);

 private:
  PublicKeyHash key_hash_;
  td::actor::ActorId<ValidatorManager> manager_;
  td::actor::ActorId<keyring::Keyring> keyring_;

  td::Timestamp update_at_ = td::Timestamp::now();

  Ref<CollatorsList> collators_list_;
  Ref<vm::Cell> new_entry_cell_;

  struct ValidatorInfo {
    UnixTime allow_update_at = 0;
    Ref<vm::Cell> entry = {};
  };

  struct Entry {
    std::vector<adnl::AdnlNodeIdShort> collators;
  };

  Ref<vm::Cell> make_entry_cell(Ref<MasterchainState> mc_state);

  td::actor::Task<> sign_and_send_request(StdSmcAddress addr, Ref<vm::Cell> request_cell);
  td::actor::Task<> send_external_message(StdSmcAddress addr, vm::CellSlice body);

  static td::Result<Ref<vm::Cell>> get_contract_data(Ref<MasterchainState> mc_state,
                                                     block::ValidatorRegistryConfig& registry_config);
  static td::Result<ValidatorInfo> get_validator_info(Ref<vm::Cell> contract_data, td::Bits256 public_key);
  static td::Result<Entry> parse_entry(Ref<vm::Cell> cell, const block::ValidatorRegistryConfig& registry_config);
  static td::Result<Entry> get_validator_entry(Ref<vm::Cell> contract_data, td::Bits256 public_key,
                                               const block::ValidatorRegistryConfig& registry_config);
  static td::Result<BlockSeqno> get_last_cleanup_key_block_seqno(Ref<vm::Cell> contract_data);
};

}  // namespace ton::validator
