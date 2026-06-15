/*
This file is part of TON Blockchain Library.

    TON Blockchain Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    TON Blockchain Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with TON Blockchain Library.  If not, see <http://www.gnu.org/licenses/>.
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
