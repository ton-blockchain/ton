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
#include "td/actor/actor.h"
#include "storage/db.h"
#include "tonlib/tonlib/TonlibClientWrapper.h"
#include "StorageManager.h"
#include "keyring/keyring.h"
#include "smc-util.h"
#include "storage/MicrochunkTree.h"

namespace ton {

struct ProviderParams {
  bool accept_new_contracts = false;
  td::RefInt256 rate_per_mb_day = td::zero_refint();
  td::uint32 max_span = 0;
  td::uint64 minimal_file_size = 0;
  td::uint64 maximal_file_size = 0;

  static td::Result<ProviderParams> create(const tl_object_ptr<ton_api::storage_daemon_provider_params>& obj);
  tl_object_ptr<ton_api::storage_daemon_provider_params> tl() const;
  bool to_builder(vm::CellBuilder& b) const;
};

class StorageProvider : public td::actor::Actor {
 public:
  struct Config {
    td::uint32 max_contracts = 1000;
    td::uint64 max_total_size = 128LL << 30;
    Config() = default;
    explicit Config(const tl_object_ptr<ton_api::storage_daemon_providerConfig>& obj);
    tl_object_ptr<ton_api::storage_daemon_providerConfig> tl() const;
  };

  StorageProvider(ContractAddress address, std::string db_root,
                  td::actor::ActorId<tonlib::TonlibClientWrapper> tonlib_client,
                  td::actor::ActorId<StorageManager> storage_manager, td::actor::ActorId<keyring::Keyring> keyring);

  void start_up() override;
  void alarm() override;
  void get_params(td::Promise<ProviderParams> promise);
  static void get_provider_params(td::actor::ActorId<tonlib::TonlibClientWrapper>, ContractAddress address,
                                  td::Promise<ProviderParams> promise);
  void set_params(ProviderParams params, td::Promise<td::Unit> promise);

  void get_provider_info(bool with_balances, bool with_contracts,
                         td::Promise<tl_object_ptr<ton_api::storage_daemon_providerInfo>> promise);
  void set_provider_config(Config config, td::Promise<td::Unit> promise);
  void withdraw(ContractAddress address, td::Promise<td::Unit> promise);
  void send_coins(ContractAddress dest, td::RefInt256 amount, std::string message, td::Promise<td::Unit> promise);
  void close_storage_contract(ContractAddress address, td::Promise<td::Unit> promise);

 private:
  ContractAddress main_address_;
  std::string db_root_;
  td::actor::ActorId<tonlib::TonlibClientWrapper> tonlib_client_;
  td::actor::ActorId<StorageManager> storage_manager_;
  td::actor::ActorId<keyring::Keyring> keyring_;
  td::Promise<td::Unit> init_promise_;
  Config config_;

  std::unique_ptr<td::KeyValue> db_;
  td::actor::ActorOwn<FabricContractWrapper> contract_wrapper_;
  td::uint64 last_processed_lt_ = 0;

  struct StorageContract {
    enum State { st_downloading = 0, st_downloaded = 1, st_active = 2, st_closing = 3 };
    td::Bits256 torrent_hash;
    td::Bits256 microchunk_hash;
    td::uint32 created_time;
    State state;

    td::uint64 file_size = 0;
    td::uint32 max_span = 0;
    td::RefInt256 rate = td::zero_refint();

    // TODO: Compute and store only one tree for duplicating torrents
    std::shared_ptr<MicrochunkTree> microchunk_tree;

    td::Timestamp check_next_proof_at = td::Timestamp::never();
  };
  std::map<ContractAddress, StorageContract> contracts_;
  td::uint64 contracts_total_size_ = 0;

  void process_transaction(tl_object_ptr<tonlib_api::raw_transaction> transaction);

  void db_store_state();
  void db_store_config();
  void db_update_storage_contract(const ContractAddress& address, bool update_list);
  void db_update_microchunk_tree(const ContractAddress& address);

  void on_new_storage_contract(ContractAddress address, td::Promise<td::Unit> promise, int max_retries = 10);
  void on_new_storage_contract_cont(ContractAddress address, StorageContractData data, td::Promise<td::Unit> promise);
  void init_new_storage_contract(ContractAddress address, StorageContract& contract);
  void downloaded_torrent(ContractAddress address, MicrochunkTree microchunk_tree);
  void after_contract_downloaded(ContractAddress address, td::Timestamp retry_until = td::Timestamp::in(30.0),
                                 td::Timestamp retry_false_until = td::Timestamp::never());
  void activate_contract_cont(ContractAddress address);
  void activated_storage_contract(ContractAddress address);
  void do_close_storage_contract(ContractAddress address);
  void check_storage_contract_deleted(ContractAddress address,
                                      td::Timestamp retry_false_until = td::Timestamp::never());
  void send_close_storage_contract(ContractAddress address);
  void storage_contract_deleted(ContractAddress address);

  void check_next_proof(ContractAddress address, StorageContract& contract);
  void got_next_proof_info(ContractAddress address, td::Result<StorageContractData> R);
  void got_contract_exists(ContractAddress address, td::Result<bool> R);
  void got_next_proof(ContractAddress address, td::Result<td::Ref<vm::Cell>> R);
  void sent_next_proof(ContractAddress address);
};

}  // namespace ton