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

using namespace ton;

struct ProviderParams {
  bool accept_new_contracts = false;
  td::RefInt256 rate_per_mb_day = td::zero_refint();
  td::uint32 max_span = 0;
  td::uint64 minimal_file_size = 0;
  td::uint64 maximal_file_size = 0;

  ProviderParams() = default;
  ProviderParams(const tl_object_ptr<ton_api::storage_daemon_provider_params>& obj);
  tl_object_ptr<ton_api::storage_daemon_provider_params> tl() const;
  bool to_builder(vm::CellBuilder& b) const;
};

class StorageProvider : public td::actor::Actor {
 public:
  StorageProvider(ContractAddress address, std::string db_root, std::string global_config,
                  td::actor::ActorId<StorageManager> storage_manager, td::actor::ActorId<keyring::Keyring> keyring);

  void start_up() override;
  void alarm() override;
  void get_params(td::Promise<ProviderParams> promise);
  void set_params(ProviderParams params, td::Promise<td::Unit> promise);

 private:
  ContractAddress main_address_;
  std::string db_root_;
  std::string global_config_;
  td::actor::ActorId<StorageManager> storage_manager_;
  td::actor::ActorId<keyring::Keyring> keyring_;
  td::Promise<td::Unit> init_promise_;

  std::unique_ptr<td::KeyValue> db_;
  td::actor::ActorOwn<tonlib::TonlibClientWrapper> tonlib_client_;

  PublicKeyHash public_key_hash_ = PublicKeyHash::zero();
  td::uint64 last_processed_lt_ = 0;
  bool public_key_query_active_ = false;
  std::vector<td::Promise<td::Unit>> public_key_waiting_;

  td::Timestamp next_load_transactions_at_ = td::Timestamp::now();
  std::vector<tl_object_ptr<tonlib_api::raw_transaction>> unprocessed_transactions_;

  struct StorageContract {
    enum State { st_downloading = 0, st_downloaded = 1, st_active = 2, st_closing = 3 };
    td::Bits256 torrent_hash;
    td::uint32 created_time;
    State state;

    // TODO: Compute and store only one tree for duplicating torrents
    std::shared_ptr<MicrochunkTree> microchunk_tree;

    td::Timestamp check_next_proof_at = td::Timestamp::never();
  };
  std::map<ContractAddress, StorageContract> contracts_;

  void db_store_state();
  void db_update_storage_contract(const ContractAddress& address, bool update_list);
  void db_update_microchunk_tree(const ContractAddress& address);

  void run_get_method(ContractAddress address, std::string method,
                      std::vector<tl_object_ptr<tonlib_api::tvm_StackEntry>> args,
                      td::Promise<tl_object_ptr<tonlib_api::smc_runResult>> promise);
  void send_internal_message(ContractAddress dest, td::uint64 coins, vm::CellSlice body, td::Promise<td::Unit> promise);
  void send_internal_message_cont(td::Ref<vm::Cell> int_msg, td::uint32 seqno, td::Promise<td::Unit> promise);
  void wait_public_key(td::Promise<td::Unit> promise);
  void wait_public_key_finish(td::Result<tl_object_ptr<tonlib_api::smc_runResult>> R);
  void get_seqno(td::Promise<td::uint32> promise);

  void load_last_transactions();
  void load_last_transactions_cont(std::vector<tl_object_ptr<tonlib_api::raw_transaction>> transactions,
                                   tl_object_ptr<tonlib_api::internal_transactionId> next_id);
  void loaded_last_transactions(td::Result<std::vector<tl_object_ptr<tonlib_api::raw_transaction>>> R);
  void process_last_transactions();
  void processed_transaction(td::uint64 lt);

  void on_new_storage_contract(ContractAddress address, td::Promise<td::Unit> promise, int max_retries = 3);
  void on_new_storage_contract_cont(ContractAddress address, td::Bits256 hash, td::Promise<td::Unit> promise);
  void init_new_storage_contract(ContractAddress address, StorageContract& contract);
  void downloaded_torrent(ContractAddress address, MicrochunkTree microchunk_tree);
  void activate_contract(ContractAddress address);
  void activate_contract_cont(ContractAddress address);
  void activated_storage_contract(ContractAddress address);
  void close_storage_contract(ContractAddress address);

  void check_next_proof(ContractAddress address, StorageContract& contract);
  void got_next_proof_info(ContractAddress address, td::Result<tl_object_ptr<tonlib_api::smc_runResult>> R);
  void got_next_proof(ContractAddress address, td::Result<td::Ref<vm::Cell>> R);
};
