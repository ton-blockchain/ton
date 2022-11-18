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

#include "StorageProvider.h"
#include "td/db/RocksDb.h"
#include "td/utils/filesystem.h"
#include "td/utils/JsonBuilder.h"
#include "auto/tl/ton_api_json.h"
#include "td/utils/port/path.h"
#include "block/block-auto.h"
#include "common/delay.h"
#include "td/actor/MultiPromise.h"

ProviderParams::ProviderParams(const tl_object_ptr<ton_api::storage_daemon_provider_params>& obj)
    : accept_new_contracts(obj->accept_new_contracts_)
    , rate_per_mb_day(true)
    , max_span(obj->max_span_)
    , minimal_file_size(obj->minimal_file_size_)
    , maximal_file_size(obj->maximal_file_size_) {
  CHECK(rate_per_mb_day.write().import_bytes(obj->rate_per_mb_day_.data(), 32, false));
}

tl_object_ptr<ton_api::storage_daemon_provider_params> ProviderParams::tl() const {
  td::Bits256 rate_per_mb_day_bits;
  CHECK(rate_per_mb_day->export_bytes(rate_per_mb_day_bits.data(), 32, false));
  return create_tl_object<ton_api::storage_daemon_provider_params>(accept_new_contracts, rate_per_mb_day_bits, max_span,
                                                                   minimal_file_size, maximal_file_size);
}

bool ProviderParams::to_builder(vm::CellBuilder& b) const {
  return b.store_long_bool(accept_new_contracts, 1) && store_coins(b, rate_per_mb_day) &&
         b.store_long_bool(max_span, 32) && b.store_long_bool(minimal_file_size, 64) &&
         b.store_long_bool(maximal_file_size, 64);
}

StorageProvider::StorageProvider(ContractAddress account_address, std::string db_root,
                                 td::actor::ActorId<tonlib::TonlibClientWrapper> tonlib_client,
                                 td::actor::ActorId<StorageManager> storage_manager,
                                 td::actor::ActorId<keyring::Keyring> keyring)

    : main_address_(account_address)
    , db_root_(std::move(db_root))
    , tonlib_client_(std::move(tonlib_client))
    , storage_manager_(std::move(storage_manager))
    , keyring_(std::move(keyring)) {
}

void StorageProvider::start_up() {
  LOG(INFO) << "Initing storage provider, account address: " << main_address_.to_string();
  td::mkdir(db_root_).ensure();
  db_ = std::make_unique<td::RocksDb>(td::RocksDb::open(db_root_).move_as_ok());

  auto r_state = db::db_get<ton_api::storage_provider_db_state>(
      *db_, create_hash_tl_object<ton_api::storage_provider_db_key_state>(), true);
  r_state.ensure();
  auto state = r_state.move_as_ok();
  if (state) {
    last_processed_lt_ = state->last_processed_lt_;
    LOG(INFO) << "Loaded storage provider state";
    LOG(INFO) << "Last processed lt: " << last_processed_lt_;
  }

  class Callback : public FabricContractWrapper::Callback {
   public:
    explicit Callback(td::actor::ActorId<StorageProvider> id) : id_(std::move(id)) {
    }
    void on_transaction(tl_object_ptr<tonlib_api::raw_transaction> transaction) override {
      td::actor::send_closure(id_, &StorageProvider::process_transaction, std::move(transaction));
    }

   private:
    td::actor::ActorId<StorageProvider> id_;
  };
  contract_wrapper_ =
      td::actor::create_actor<FabricContractWrapper>("ContractWrapper", main_address_, tonlib_client_, keyring_,
                                                     td::make_unique<Callback>(actor_id(this)), last_processed_lt_);

  auto r_contract_list = db::db_get<ton_api::storage_provider_db_contractList>(
      *db_, create_hash_tl_object<ton_api::storage_provider_db_key_contractList>(), true);
  r_contract_list.ensure();
  auto contract_list = r_contract_list.move_as_ok();
  if (contract_list) {
    LOG(INFO) << "Loading " << contract_list->contracts_.size() << " contracts from db";
    for (auto& c : contract_list->contracts_) {
      ContractAddress address(c->wc_, c->addr_);
      if (contracts_.count(address)) {
        LOG(ERROR) << "Duplicate contract in db: " << address.to_string();
        continue;
      }
      auto r_contract = db::db_get<ton_api::storage_provider_db_storageContract>(
          *db_, create_hash_tl_object<ton_api::storage_provider_db_key_storageContract>(address.wc, address.addr),
          true);
      r_contract.ensure();
      auto db_contract = r_contract.move_as_ok();
      if (!db_contract) {
        LOG(ERROR) << "Missing contract in db: " << address.to_string();
        continue;
      }
      StorageContract& contract = contracts_[address];
      contract.torrent_hash = db_contract->torrent_hash_;
      contract.created_time = db_contract->created_time_;
      contract.state = (StorageContract::State)db_contract->state_;
      contract.file_size = db_contract->file_size_;
      contract.max_span = db_contract->max_span_;
      contract.rate = db_contract->rate_;

      auto r_tree = db::db_get<ton_api::storage_provider_db_microchunkTree>(
          *db_, create_hash_tl_object<ton_api::storage_provider_db_key_microchunkTree>(address.wc, address.addr), true);
      r_tree.ensure();
      auto tree = r_tree.move_as_ok();
      if (tree) {
        contract.microchunk_tree = std::make_shared<MicrochunkTree>(vm::std_boc_deserialize(tree->data_).move_as_ok());
      }
    }
  }

  for (auto& p : contracts_) {
    const ContractAddress& address = p.first;
    StorageContract& contract = p.second;
    switch (contract.state) {
      case StorageContract::st_downloading:
        init_new_storage_contract(address, contract);
        break;
      case StorageContract::st_downloaded:
        check_contract_active(address);
        break;
      case StorageContract::st_active:
        contract.check_next_proof_at = td::Timestamp::now();
        break;
      case StorageContract::st_closing:
        check_storage_contract_deleted(address);
        break;
      default:
        LOG(FATAL) << "Invalid contract state in db";
    }
  }
  LOG(INFO) << "Loaded contracts from db";

  alarm();
}

void StorageProvider::get_params(td::Promise<ProviderParams> promise) {
  td::actor::send_closure(
      contract_wrapper_, &FabricContractWrapper::run_get_method, "get_storage_params",
      std::vector<tl_object_ptr<tonlib_api::tvm_StackEntry>>(),
      promise.wrap([](std::vector<tl_object_ptr<tonlib_api::tvm_StackEntry>> stack) -> td::Result<ProviderParams> {
        if (stack.size() != 5) {
          return td::Status::Error(PSTRING() << "Method returned " << stack.size() << " values, 5 expected");
        }
        TRY_RESULT_PREFIX(accept_new_contracts, entry_to_int<int>(stack[0]), "Invalid accept_new_contracts: ");
        TRY_RESULT_PREFIX(rate_per_mb_day, entry_to_int<td::RefInt256>(stack[1]), "Invalid rate_per_mb_day: ");
        TRY_RESULT_PREFIX(max_span, entry_to_int<td::uint32>(stack[2]), "Invalid max_span: ");
        TRY_RESULT_PREFIX(minimal_file_size, entry_to_int<td::uint64>(stack[3]), "Invalid minimal_file_size: ");
        TRY_RESULT_PREFIX(maximal_file_size, entry_to_int<td::uint64>(stack[4]), "Invalid maximal_file_size: ");
        ProviderParams params;
        params.accept_new_contracts = accept_new_contracts;
        params.rate_per_mb_day = rate_per_mb_day;
        params.max_span = max_span;
        params.minimal_file_size = minimal_file_size;
        params.maximal_file_size = maximal_file_size;
        return params;
      }));
}

void StorageProvider::set_params(ProviderParams params, td::Promise<td::Unit> promise) {
  vm::CellBuilder b;
  b.store_long(102, 32);  // const op::update_storage_params = 102;
  b.store_long(0, 64);    // query_id
  if (!params.to_builder(b)) {
    promise.set_error(td::Status::Error("Failed to store params to builder"));
    return;
  }
  LOG(INFO) << "Sending external message to update provider parameters";
  td::actor::send_closure(contract_wrapper_, &FabricContractWrapper::send_internal_message, main_address_, 100'000'000,
                          b.as_cellslice(), std::move(promise));
}

void StorageProvider::db_store_state() {
  db_->begin_transaction().ensure();
  db_->set(create_hash_tl_object<ton_api::storage_provider_db_key_state>().as_slice(),
           create_serialize_tl_object<ton_api::storage_provider_db_state>(last_processed_lt_))
      .ensure();
  db_->commit_transaction().ensure();
}

void StorageProvider::alarm() {
  for (auto& p : contracts_) {
    if (p.second.check_next_proof_at && p.second.check_next_proof_at.is_in_past()) {
      p.second.check_next_proof_at = td::Timestamp::never();
      check_next_proof(p.first, p.second);
    }
    alarm_timestamp().relax(p.second.check_next_proof_at);
  }
}

void StorageProvider::process_transaction(tl_object_ptr<tonlib_api::raw_transaction> transaction) {
  std::string new_contract_address;
  for (auto& message : transaction->out_msgs_) {
    auto data = dynamic_cast<tonlib_api::msg_dataRaw*>(message->msg_data_.get());
    if (data == nullptr) {
      continue;
    }
    auto r_body = vm::std_boc_deserialize(data->body_);
    if (r_body.is_error()) {
      LOG(ERROR) << "Invalid message body in tonlib response: " << r_body.move_as_error();
      continue;
    }
    td::Ref<vm::Cell> body = r_body.move_as_ok();
    vm::CellSlice cs = vm::load_cell_slice(body);
    // const op::new_storage_contract = 1;
    if (cs.size() >= 32 && cs.prefetch_long(32) == 1) {
      new_contract_address = message->destination_->account_address_;
    }
  }
  if (!new_contract_address.empty()) {
    auto P = td::PromiseCreator::lambda(
        [SelfId = actor_id(this), lt = (td::uint64)transaction->transaction_id_->lt_](td::Result<td::Unit> R) {
          if (R.is_error()) {
            LOG(ERROR) << "Error during processing new storage contract, skipping: " << R.move_as_error();
          }
        });
    on_new_storage_contract(ContractAddress::parse(new_contract_address).move_as_ok(), std::move(P));
  }

  last_processed_lt_ = transaction->transaction_id_->lt_;
  db_store_state();
}

void StorageProvider::on_new_storage_contract(ContractAddress address, td::Promise<td::Unit> promise, int max_retries) {
  LOG(INFO) << "Processing new storage contract: " << address.to_string();
  run_get_method(address, tonlib_client_, "get_contract_data", {},
                 [SelfId = actor_id(this), address, promise = std::move(promise),
                  max_retries](td::Result<std::vector<tl_object_ptr<tonlib_api::tvm_StackEntry>>> R) mutable {
                   if (R.is_error()) {
                     if (max_retries > 0) {
                       delay_action(
                           [SelfId = std::move(SelfId), promise = std::move(promise), address = std::move(address),
                            max_retries]() mutable {
                             td::actor::send_closure(SelfId, &StorageProvider::on_new_storage_contract,
                                                     std::move(address), std::move(promise), max_retries - 1);
                           },
                           td::Timestamp::in(5.0));
                     } else {
                       promise.set_error(R.move_as_error());
                     }
                     return;
                   }
                   auto stack = R.move_as_ok();
                   td::actor::send_closure(SelfId, &StorageProvider::on_new_storage_contract_cont, address,
                                           std::move(stack), std::move(promise));
                 });
}

void StorageProvider::on_new_storage_contract_cont(ContractAddress address,
                                                   std::vector<tl_object_ptr<tonlib_api::tvm_StackEntry>> params,
                                                   td::Promise<td::Unit> promise) {
  // Params: active, balance, provider, merkle_hash, file_size, next_proof, rate_per_mb_day, max_span, last_proof_time,
  //         client, torrent_hash
  if (params.size() != 11) {
    promise.set_error(td::Status::Error(PSTRING() << "Method returned " << params.size() << " values, 11 expected"));
    return;
  }
  TRY_RESULT_PROMISE_PREFIX(promise, hash, entry_to_bits256(params[10]), "Invalid torrent hash: ");
  TRY_RESULT_PROMISE_PREFIX(promise, file_size, entry_to_int<td::uint64>(params[4]), "Invalid file size: ");
  TRY_RESULT_PROMISE_PREFIX(promise, max_span, entry_to_int<td::uint32>(params[7]), "Invalid max_span: ");
  TRY_RESULT_PROMISE_PREFIX(promise, rate, entry_to_int<td::RefInt256>(params[5]), "Invalid rate: ");
  auto it = contracts_.emplace(address, StorageContract());
  if (!it.second) {
    promise.set_error(td::Status::Error(PSTRING() << "Storage contract already registered: " << address.to_string()));
    return;
  }
  LOG(INFO) << "New storage contract " << address.to_string() << ", torrent hash: " << hash.to_hex();
  StorageContract& contract = it.first->second;
  contract.torrent_hash = hash;
  contract.state = StorageContract::st_downloading;
  contract.created_time = (td::uint32)td::Clocks::system();
  contract.file_size = file_size;
  contract.max_span = max_span;
  contract.rate = td::min<td::RefInt256>(rate, td::make_refint(std::numeric_limits<td::int64>::max()))->to_long();
  db_update_storage_contract(address, true);
  promise.set_result(td::Unit());
  init_new_storage_contract(address, contract);
}

void StorageProvider::db_update_storage_contract(const ContractAddress& address, bool update_list) {
  db_->begin_transaction().ensure();
  if (update_list) {
    std::vector<tl_object_ptr<ton_api::storage_provider_db_contractAddress>> list;
    for (const auto& t : contracts_) {
      list.push_back(create_tl_object<ton_api::storage_provider_db_contractAddress>(t.first.wc, t.first.addr));
    }
    db_->set(create_hash_tl_object<ton_api::storage_provider_db_key_contractList>().as_slice(),
             create_serialize_tl_object<ton_api::storage_provider_db_contractList>(std::move(list)))
        .ensure();
  }
  auto key = create_hash_tl_object<ton_api::storage_provider_db_key_storageContract>(address.wc, address.addr);
  auto it = contracts_.find(address);
  if (it == contracts_.end()) {
    db_->erase(key.as_slice()).ensure();
  } else {
    const StorageContract& contract = it->second;
    db_->set(key.as_slice(), create_serialize_tl_object<ton_api::storage_provider_db_storageContract>(
                                 contract.torrent_hash, contract.created_time, (int)contract.state, contract.file_size,
                                 contract.rate, contract.max_span));
  }
  db_->commit_transaction().ensure();
}

void StorageProvider::db_update_microchunk_tree(const ContractAddress& address) {
  db_->begin_transaction().ensure();
  auto key = create_hash_tl_object<ton_api::storage_provider_db_key_microchunkTree>(address.wc, address.addr);
  auto it = contracts_.find(address);
  if (it == contracts_.end() || it->second.microchunk_tree == nullptr) {
    db_->erase(key.as_slice()).ensure();
  } else {
    db_->set(key.as_slice(), create_serialize_tl_object<ton_api::storage_provider_db_microchunkTree>(
                                 vm::std_boc_serialize(it->second.microchunk_tree->get_root()).move_as_ok()));
  }
  db_->commit_transaction().ensure();
}

void StorageProvider::init_new_storage_contract(ContractAddress address, StorageContract& contract) {
  CHECK(contract.state == StorageContract::st_downloading);
  td::actor::send_closure(storage_manager_, &StorageManager::add_torrent_by_hash, contract.torrent_hash, "", false,
                          [](td::Result<td::Unit>) {
                            // Ignore errors: error can mean that the torrent already exists, other errors will be caught later
                          });
  td::actor::send_closure(storage_manager_, &StorageManager::set_active_download, contract.torrent_hash, true,
                          [SelfId = actor_id(this), address](td::Result<td::Unit> R) {
                            if (R.is_error()) {
                              LOG(ERROR) << "Failed to init storage contract: " << R.move_as_error();
                              td::actor::send_closure(SelfId, &StorageProvider::do_close_storage_contract, address);
                              return;
                            }
                          });
  td::actor::send_closure(
      storage_manager_, &StorageManager::wait_for_completion, contract.torrent_hash,
      [SelfId = actor_id(this), address, hash = contract.torrent_hash,
       manager = storage_manager_](td::Result<td::Unit> R) {
        if (R.is_error()) {
          LOG(WARNING) << "Failed to download torrent " << hash.to_hex() << ": " << R.move_as_error();
          td::actor::send_closure(SelfId, &StorageProvider::do_close_storage_contract, address);
          return;
        }
        td::actor::send_closure(
            manager, &StorageManager::with_torrent, hash, [SelfId, address, hash](td::Result<NodeActor::NodeState> R) {
              auto r_microchunk_tree = [&]() -> td::Result<MicrochunkTree> {
                TRY_RESULT(state, std::move(R));
                Torrent& torrent = state.torrent;
                if (!torrent.is_completed() || torrent.get_included_size() != torrent.get_info().file_size) {
                  return td::Status::Error("unknown error");
                }
                return MicrochunkTree::Builder::build_for_torrent(torrent);
              }();
              if (r_microchunk_tree.is_error()) {
                LOG(WARNING) << "Failed to download torrent " << hash.to_hex() << ": " << R.move_as_error();
                td::actor::send_closure(SelfId, &StorageProvider::do_close_storage_contract, address);
              } else {
                td::actor::send_closure(SelfId, &StorageProvider::downloaded_torrent, address,
                                        r_microchunk_tree.move_as_ok());
              }
            });
      });
}

void StorageProvider::downloaded_torrent(ContractAddress address, MicrochunkTree microchunk_tree) {
  auto it = contracts_.find(address);
  if (it == contracts_.end()) {
    LOG(WARNING) << "Contract " << address.to_string() << " does not exist anymore";
    return;
  }
  auto& contract = it->second;
  LOG(INFO) << "Finished downloading torrent " << contract.torrent_hash.to_hex() << " for contract "
            << address.to_string();
  contract.state = StorageContract::st_downloaded;
  contract.microchunk_tree = std::make_shared<MicrochunkTree>(std::move(microchunk_tree));
  db_update_microchunk_tree(address);
  db_update_storage_contract(address, false);
  check_contract_active(address);
}

void StorageProvider::check_contract_active(ContractAddress address, td::Timestamp retry_until,
                                            td::Timestamp retry_false_until) {
  run_get_method(
      address, tonlib_client_, "is_active", {},
      [=, SelfId = actor_id(this)](td::Result<std::vector<tl_object_ptr<tonlib_api::tvm_StackEntry>>> R) mutable {
        if (R.is_error()) {
          LOG(WARNING) << "Failed to check that contract is active: " << R.move_as_error();
          if (retry_until && retry_until.is_in_past()) {
            delay_action(
                [=]() {
                  td::actor::send_closure(SelfId, &StorageProvider::check_contract_active, address, retry_until,
                                          retry_false_until);
                },
                td::Timestamp::in(5.0));
          }
          return;
        }
        auto stack = R.move_as_ok();
        auto active = [&]() -> td::Result<bool> {
          if (stack.size() != 1) {
            return td::Status::Error(PSTRING() << "Method returned " << stack.size() << " values, 1 expected");
          }
          TRY_RESULT(x, entry_to_int<int>(stack[0]));
          return (bool)x;
        }();
        if (active.is_error()) {
          LOG(ERROR) << "Failed to check that contract is active: " << active.move_as_error();
          td::actor::send_closure(SelfId, &StorageProvider::do_close_storage_contract, address);
        } else if (active.ok()) {
          td::actor::send_closure(SelfId, &StorageProvider::activated_storage_contract, address);
        } else if (retry_false_until && retry_false_until.is_in_past()) {
          delay_action(
              [=]() {
                td::actor::send_closure(SelfId, &StorageProvider::check_contract_active, address, retry_until,
                                        retry_false_until);
              },
              td::Timestamp::in(5.0));
        } else {
          td::actor::send_closure(SelfId, &StorageProvider::activate_contract_cont, address);
        }
      });
}

void StorageProvider::activate_contract_cont(ContractAddress address) {
  vm::CellBuilder b;
  b.store_long(9, 32);  // const op::accept_contract = 9;
  b.store_long(0, 64);  // query_id
  td::actor::send_closure(
      contract_wrapper_, &FabricContractWrapper::send_internal_message, address, 100'000'000, b.as_cellslice(),
      [SelfId = actor_id(this), address](td::Result<td::Unit> R) {
        if (R.is_error()) {
          LOG(ERROR) << "Failed to send activate message, retrying later: " << R.move_as_error();
          delay_action([=]() { td::actor::send_closure(SelfId, &StorageProvider::activate_contract_cont, address); },
                       td::Timestamp::in(10.0));
          return;
        }
        td::actor::send_closure(SelfId, &StorageProvider::check_contract_active, address, td::Timestamp::in(60.0),
                                td::Timestamp::in(40.0));
      });
}

void StorageProvider::activated_storage_contract(ContractAddress address) {
  auto it = contracts_.find(address);
  if (it == contracts_.end()) {
    LOG(WARNING) << "Contract " << address.to_string() << " does not exist anymore";
    return;
  }
  LOG(INFO) << "Storage contract " << address.to_string() << " is active";
  auto& contract = it->second;
  contract.state = StorageContract::st_active;
  db_update_storage_contract(address, false);
  alarm_timestamp().relax(contract.check_next_proof_at = td::Timestamp::in(1.0));
}

void StorageProvider::do_close_storage_contract(ContractAddress address) {
  auto it = contracts_.find(address);
  if (it == contracts_.end()) {
    LOG(WARNING) << "Contract " << address.to_string() << " does not exist anymore";
    return;
  }
  LOG(INFO) << "Closing storage contract " << address.to_string();
  auto& contract = it->second;
  contract.state = StorageContract::st_closing;
  db_update_storage_contract(address, false);
  check_storage_contract_deleted(address);
}

void StorageProvider::send_close_storage_contract(ContractAddress address) {
  vm::CellBuilder b;
  b.store_long(7, 32);  // const op::close_contract = 7;
  b.store_long(0, 64);  // query_id
  td::actor::send_closure(
      contract_wrapper_, &FabricContractWrapper::send_internal_message, address, 100'000'000, b.as_cellslice(),
      [SelfId = actor_id(this), address](td::Result<td::Unit> R) {
        if (R.is_error()) {
          LOG(ERROR) << "Failed to send close message, retrying later: " << R.move_as_error();
          delay_action([=]() { td::actor::send_closure(SelfId, &StorageProvider::activate_contract_cont, address); },
                       td::Timestamp::in(10.0));
          return;
        }
        td::actor::send_closure(SelfId, &StorageProvider::check_storage_contract_deleted, address,
                                td::Timestamp::in(40.0));
      });
}

void StorageProvider::check_storage_contract_deleted(ContractAddress address, td::Timestamp retry_false_until) {
  check_contract_exists(address, tonlib_client_, [=, SelfId = actor_id(this)](td::Result<bool> R) {
    if (R.is_error()) {
      delay_action(
          [=]() {
            td::actor::send_closure(SelfId, &StorageProvider::check_storage_contract_deleted, address,
                                    retry_false_until);
          },
          td::Timestamp::in(10.0));
      return;
    }
    if (!R.move_as_ok()) {
      td::actor::send_closure(SelfId, &StorageProvider::storage_contract_deleted, address);
    } else if (retry_false_until && !retry_false_until.is_in_past()) {
      delay_action(
          [=]() {
            td::actor::send_closure(SelfId, &StorageProvider::check_storage_contract_deleted, address,
                                    retry_false_until);
          },
          td::Timestamp::in(5.0));
    } else {
      td::actor::send_closure(SelfId, &StorageProvider::send_close_storage_contract, address);
    }
  });
}

void StorageProvider::storage_contract_deleted(ContractAddress address) {
  auto it = contracts_.find(address);
  if (it == contracts_.end()) {
    return;
  }
  LOG(INFO) << "Storage contract " << address.to_string() << " was deleted";
  td::Bits256 hash = it->second.torrent_hash;
  contracts_.erase(it);
  bool delete_torrent = true;
  for (const auto& p : contracts_) {
    if (p.second.torrent_hash == hash) {
      delete_torrent = false;
      break;
    }
  }
  if (delete_torrent) {
    LOG(INFO) << "Deleting torrent " << hash.to_hex();
    td::actor::send_closure(storage_manager_, &StorageManager::remove_torrent, hash, true,
                            [](td::Result<td::Unit> R) {});
  }
  db_update_storage_contract(address, true);
}

void StorageProvider::check_next_proof(ContractAddress address, StorageContract& contract) {
  if (contract.state != StorageContract::st_active) {
    return;
  }
  CHECK(contract.microchunk_tree != nullptr);
  run_get_method(
      address, tonlib_client_, "get_next_proof_info", {},
      [SelfId = actor_id(this), address](td::Result<std::vector<tl_object_ptr<tonlib_api::tvm_StackEntry>>> R) {
        td::actor::send_closure(SelfId, &StorageProvider::got_next_proof_info, address, std::move(R));
      });
}

void StorageProvider::got_next_proof_info(ContractAddress address,
                                          td::Result<std::vector<tl_object_ptr<tonlib_api::tvm_StackEntry>>> R) {
  auto it = contracts_.find(address);
  if (it == contracts_.end() || it->second.state != StorageContract::st_active) {
    return;
  }
  auto& contract = it->second;
  td::uint64 next_proof = 0;
  td::uint32 last_proof_time = 0, max_span = 0;
  auto S = [&]() -> td::Status {
    TRY_RESULT(stack, std::move(R));
    if (stack.size() != 3) {
      return td::Status::Error(PSTRING() << "Method returned " << stack.size() << " values, 3 expected");
    }
    TRY_RESULT_PREFIX_ASSIGN(next_proof, entry_to_int<td::uint64>(stack[0]), "Invalid next_proof: ");
    TRY_RESULT_PREFIX_ASSIGN(last_proof_time, entry_to_int<td::uint32>(stack[1]), "Invalid last_proof_time: ");
    TRY_RESULT_PREFIX_ASSIGN(max_span, entry_to_int<td::uint32>(stack[2]), "Invalid max_span: ");
    return td::Status::OK();
  }();
  if (S.is_error()) {
    LOG(ERROR) << "get_next_proof_info for " << address.to_string() << ": " << S.move_as_error();
    alarm_timestamp().relax(contract.check_next_proof_at = td::Timestamp::in(10.0));
    return;
  }
  td::uint32 send_at = last_proof_time + max_span / 2, now = (td::uint32)td::Clocks::system();
  if (now < send_at) {
    alarm_timestamp().relax(contract.check_next_proof_at = td::Timestamp::in(send_at - now + 2));
    return;
  }

  LOG(INFO) << "Sending proof for " << address.to_string() << ": next_proof=" << next_proof << ", max_span=" << max_span
            << ", last_proof_time=" << last_proof_time << " (" << now - last_proof_time << "s ago)";
  td::actor::send_closure(
      storage_manager_, &StorageManager::with_torrent, contract.torrent_hash,
      [=, SelfId = actor_id(this), tree = contract.microchunk_tree](td::Result<NodeActor::NodeState> R) {
        if (R.is_error()) {
          LOG(ERROR) << "Missing torrent for " << address.to_string();
          return;
        }
        auto state = R.move_as_ok();
        td::uint64 l = next_proof / MicrochunkTree::MICROCHUNK_SIZE * MicrochunkTree::MICROCHUNK_SIZE;
        td::uint64 r = l + MicrochunkTree::MICROCHUNK_SIZE;
        auto proof = tree->get_proof(l, r, state.torrent);
        td::actor::send_closure(SelfId, &StorageProvider::got_next_proof, address, std::move(proof));
      });
}

void StorageProvider::got_next_proof(ContractAddress address, td::Result<td::Ref<vm::Cell>> R) {
  if (R.is_error()) {
    LOG(ERROR) << "Failed to build proof: " << R.move_as_error();
    return;
  }
  LOG(INFO) << "Got proof, sending";

  vm::CellBuilder b;
  b.store_long(11, 32);  // const op::proof_storage = 11;
  b.store_long(0, 64);   // query_id
  b.store_ref(R.move_as_ok());
  td::actor::send_closure(contract_wrapper_, &FabricContractWrapper::send_internal_message, address, 100'000'000,
                          b.as_cellslice(), [SelfId = actor_id(this), address](td::Result<td::Unit> R) {
                            if (R.is_error()) {
                              LOG(ERROR) << "Failed to send proof message: " << R.move_as_error();
                            }
                            td::actor::send_closure(SelfId, &StorageProvider::sent_next_proof, address);
                          });
}

void StorageProvider::sent_next_proof(ContractAddress address) {
  auto it = contracts_.find(address);
  if (it == contracts_.end() || it->second.state != StorageContract::st_active) {
    return;
  }
  auto& contract = it->second;
  alarm_timestamp().relax(contract.check_next_proof_at = td::Timestamp::in(30.0));
}

void StorageProvider::get_provider_info(bool with_balances, bool with_contracts,
                                        td::Promise<tl_object_ptr<ton_api::storage_daemon_providerInfo>> promise) {
  auto result = std::make_shared<ton_api::storage_daemon_providerInfo>();
  td::MultiPromise mp;
  auto ig = mp.init_guard();
  ig.add_promise(promise.wrap(
      [result](td::Unit) { return create_tl_object<ton_api::storage_daemon_providerInfo>(std::move(*result)); }));
  result->address_ = main_address_.to_string();
  if (with_balances) {
    get_contract_balance(main_address_, tonlib_client_, ig.get_promise().wrap([result](td::uint64 balance) {
      result->balance_ = balance;
      return td::Unit();
    }));
  } else {
    result->balance_ = -1;
  }
  if (with_contracts) {
    for (const auto& p : contracts_) {
      auto obj = create_tl_object<ton_api::storage_daemon_contractInfo>();
      const StorageContract& contract = p.second;
      obj->address_ = p.first.to_string();
      obj->state_ = (int)contract.state;
      obj->torrent_ = contract.torrent_hash;
      obj->created_time_ = contract.created_time;
      obj->rate_ = contract.rate;
      obj->max_span_ = contract.max_span;
      obj->file_size_ = contract.file_size;
      obj->downloaded_size_ = obj->file_size_;
      obj->client_balance_ = -1;
      obj->contract_balance_ = -1;
      result->contracts_.push_back(std::move(obj));
    }
    size_t i = 0;
    for (const auto& p : contracts_) {
      const StorageContract& contract = p.second;
      if (contract.state == StorageContract::st_downloading) {
        td::actor::send_closure(storage_manager_, &StorageManager::with_torrent, contract.torrent_hash,
                                [i, result, promise = ig.get_promise()](td::Result<NodeActor::NodeState> R) mutable {
                                  if (R.is_error()) {
                                    result->contracts_[i]->downloaded_size_ = 0;
                                  } else {
                                    auto state = R.move_as_ok();
                                    result->contracts_[i]->downloaded_size_ = state.torrent.get_included_ready_size();
                                  }
                                  promise.set_result(td::Unit());
                                });
      }
      if (with_balances) {
        get_contract_balance(p.first, tonlib_client_,
                             [i, result, promise = ig.get_promise()](td::Result<td::uint64> R) mutable {
                               if (R.is_ok()) {
                                 result->contracts_[i]->contract_balance_ = R.move_as_ok();
                               }
                               promise.set_result(td::Unit());
                             });
        run_get_method(p.first, tonlib_client_, "get_contract_data", {},
                       [i, result, promise = ig.get_promise()](
                           td::Result<std::vector<tl_object_ptr<tonlib_api::tvm_StackEntry>>> R) mutable {
                         auto S = [&]() -> td::Status {
                           TRY_RESULT(stack, std::move(R));
                           // Stack: active, balance, ...
                           if (stack.size() < 2) {
                             return td::Status::Error("Too few entries");
                           }
                           TRY_RESULT(balance, entry_to_int<td::uint64>(stack[1]));
                           result->contracts_[i]->client_balance_ = balance;
                           return td::Status::OK();
                         }();
                         promise.set_result(td::Unit());
                       });
      }
      i += 1;
    }
  }
}

void StorageProvider::withdraw(ContractAddress address, td::Promise<td::Unit> promise) {
  auto it = contracts_.find(address);
  if (it == contracts_.end() || it->second.state != StorageContract::st_active) {
    promise.set_error(td::Status::Error("No such storage contract"));
    return;
  }
  if (it->second.state != StorageContract::st_active) {
    promise.set_error(td::Status::Error("Storage contract is not active"));
    return;
  }
  vm::CellBuilder b;
  b.store_long(10, 32);  // const op::withdraw = 10;
  b.store_long(0, 64);   // query_id
  LOG(INFO) << "Sending 'withdraw' query to storage contract " << address.to_string();
  td::actor::send_closure(contract_wrapper_, &FabricContractWrapper::send_internal_message, address, 100'000'000,
                          b.as_cellslice(), std::move(promise));
}

void StorageProvider::withdraw_all(td::Promise<td::Unit> promise) {
  td::MultiPromise mp;
  auto ig = mp.init_guard();
  ig.add_promise(std::move(promise));
  for (const auto& p : contracts_) {
    if (p.second.state == StorageContract::st_active) {
      withdraw(p.first, ig.get_promise());
    }
  }
}

void StorageProvider::send_coins(ContractAddress dest, td::uint64 amount, std::string message,
                                 td::Promise<td::Unit> promise) {
  vm::CellBuilder b;
  if (!message.empty()) {
    b.store_long(0, 32);
    if (b.remaining_bits() < message.size() * 8) {
      promise.set_error(td::Status::Error("Message is too long (max 122 bytes)"));
      return;
    }
    b.store_bytes(td::Slice(message));
  }
  LOG(INFO) << "Sending " << amount << " nanoTON to " << dest.to_string();
  td::actor::send_closure(contract_wrapper_, &FabricContractWrapper::send_internal_message, dest, amount,
                          b.finalize_novm(), std::move(promise));
}

void StorageProvider::close_storage_contract(ContractAddress address, td::Promise<td::Unit> promise) {
  if (!contracts_.count(address)) {
    promise.set_error(td::Status::Error("No such storage contract"));
    return;
  }
  do_close_storage_contract(address);
  promise.set_result(td::Unit());
}
