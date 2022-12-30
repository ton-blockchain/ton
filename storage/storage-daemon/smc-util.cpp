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

#include "smc-util.h"
#include "td/utils/filesystem.h"
#include "keys/encryptor.h"
#include "smartcont/provider-code.h"

namespace ton {

static void smc_forget(td::actor::ActorId<tonlib::TonlibClientWrapper> client, td::int64 id) {
  auto query = create_tl_object<tonlib_api::smc_forget>(id);
  td::actor::send_closure(client, &tonlib::TonlibClientWrapper::send_request<tonlib_api::smc_forget>, std::move(query),
                          [](td::Result<tonlib_api::object_ptr<tonlib_api::ok>> R) mutable {
                            if (R.is_error()) {
                              LOG(WARNING) << "smc_forget failed: " << R.move_as_error();
                            }
                          });
}

void run_get_method(ContractAddress address, td::actor::ActorId<tonlib::TonlibClientWrapper> client, std::string method,
                    std::vector<tl_object_ptr<tonlib_api::tvm_StackEntry>> args,
                    td::Promise<std::vector<tl_object_ptr<tonlib_api::tvm_StackEntry>>> promise) {
  LOG(DEBUG) << "Running get method " << method << " on " << address.to_string();
  auto query =
      create_tl_object<tonlib_api::smc_load>(create_tl_object<tonlib_api::accountAddress>(address.to_string()));
  td::actor::send_closure(
      client, &tonlib::TonlibClientWrapper::send_request<tonlib_api::smc_load>, std::move(query),
      [client, method = std::move(method), args = std::move(args),
       promise = std::move(promise)](td::Result<tonlib_api::object_ptr<tonlib_api::smc_info>> R) mutable {
        TRY_RESULT_PROMISE(promise, obj, std::move(R));
        auto query = create_tl_object<tonlib_api::smc_runGetMethod>(
            obj->id_, create_tl_object<tonlib_api::smc_methodIdName>(std::move(method)), std::move(args));
        td::actor::send_closure(
            client, &tonlib::TonlibClientWrapper::send_request<tonlib_api::smc_runGetMethod>, std::move(query),
            [client, id = obj->id_,
             promise = std::move(promise)](td::Result<tonlib_api::object_ptr<tonlib_api::smc_runResult>> R) mutable {
              smc_forget(client, id);
              TRY_RESULT_PROMISE(promise, obj, std::move(R));
              if (obj->exit_code_ != 0 && obj->exit_code_ != 1) {
                promise.set_error(
                    td::Status::Error(PSTRING() << "Method execution finished with code " << obj->exit_code_));
                return;
              }
              promise.set_result(std::move(obj->stack_));
            });
      });
}

void check_contract_exists(ContractAddress address, td::actor::ActorId<tonlib::TonlibClientWrapper> client,
                           td::Promise<bool> promise) {
  auto query =
      create_tl_object<tonlib_api::smc_load>(create_tl_object<tonlib_api::accountAddress>(address.to_string()));
  td::actor::send_closure(
      client, &tonlib::TonlibClientWrapper::send_request<tonlib_api::smc_load>, std::move(query),
      [client, promise = std::move(promise)](td::Result<tonlib_api::object_ptr<tonlib_api::smc_info>> R) mutable {
        TRY_RESULT_PROMISE(promise, obj, std::move(R));
        auto query = create_tl_object<tonlib_api::smc_getState>(obj->id_);
        td::actor::send_closure(
            client, &tonlib::TonlibClientWrapper::send_request<tonlib_api::smc_getState>, std::move(query),
            [client, id = obj->id_,
             promise = std::move(promise)](td::Result<tonlib_api::object_ptr<tonlib_api::tvm_cell>> R) mutable {
              smc_forget(client, id);
              TRY_RESULT_PROMISE(promise, r, std::move(R));
              promise.set_result(!r->bytes_.empty());
            });
      });
}

void get_contract_balance(ContractAddress address, td::actor::ActorId<tonlib::TonlibClientWrapper> client,
                          td::Promise<td::RefInt256> promise) {
  auto query =
      create_tl_object<tonlib_api::getAccountState>(create_tl_object<tonlib_api::accountAddress>(address.to_string()));
  td::actor::send_closure(
      client, &tonlib::TonlibClientWrapper::send_request<tonlib_api::getAccountState>, std::move(query),
      promise.wrap([](tonlib_api::object_ptr<tonlib_api::fullAccountState> r) -> td::Result<td::RefInt256> {
        return td::make_refint(r->balance_);
      }));
}

FabricContractWrapper::FabricContractWrapper(ContractAddress address,
                                             td::actor::ActorId<tonlib::TonlibClientWrapper> client,
                                             td::actor::ActorId<keyring::Keyring> keyring,
                                             td::unique_ptr<Callback> callback, td::uint64 last_processed_lt)
    : address_(address)
    , client_(std::move(client))
    , keyring_(std::move(keyring))
    , callback_(std::move(callback))
    , last_processed_lt_(last_processed_lt) {
}

void FabricContractWrapper::start_up() {
  alarm();
}

void FabricContractWrapper::alarm() {
  if (process_transactions_at_ && process_transactions_at_.is_in_past()) {
    process_transactions_at_ = td::Timestamp::never();
    load_transactions();
  }
  alarm_timestamp().relax(process_transactions_at_);
  if (send_message_at_ && send_message_at_.is_in_past()) {
    send_message_at_ = td::Timestamp::never();
    do_send_external_message();
  }
  alarm_timestamp().relax(send_message_at_);
}

void FabricContractWrapper::load_transactions() {
  LOG(DEBUG) << "Loading transactions for " << address_.to_string() << ", last_lt=" << last_processed_lt_;
  auto query =
      create_tl_object<tonlib_api::getAccountState>(create_tl_object<tonlib_api::accountAddress>(address_.to_string()));
  td::actor::send_closure(
      client_, &tonlib::TonlibClientWrapper::send_request<tonlib_api::getAccountState>, std::move(query),
      [SelfId = actor_id(this)](td::Result<tonlib_api::object_ptr<tonlib_api::fullAccountState>> R) mutable {
        if (R.is_error()) {
          td::actor::send_closure(SelfId, &FabricContractWrapper::loaded_last_transactions, R.move_as_error());
          return;
        }
        auto obj = R.move_as_ok();
        td::actor::send_closure(SelfId, &FabricContractWrapper::load_last_transactions,
                                std::vector<tl_object_ptr<tonlib_api::raw_transaction>>(),
                                std::move(obj->last_transaction_id_), (td::uint32)obj->sync_utime_);
      });
}

void FabricContractWrapper::load_last_transactions(std::vector<tl_object_ptr<tonlib_api::raw_transaction>> transactions,
                                                   tl_object_ptr<tonlib_api::internal_transactionId> next_id,
                                                   td::uint32 utime) {
  if ((td::uint64)next_id->lt_ <= last_processed_lt_) {
    loaded_last_transactions(std::make_pair(std::move(transactions), utime));
    return;
  }
  auto query = create_tl_object<tonlib_api::raw_getTransactionsV2>(
      nullptr, create_tl_object<tonlib_api::accountAddress>(address_.to_string()), std::move(next_id), 10, false);
  td::actor::send_closure(
      client_, &tonlib::TonlibClientWrapper::send_request<tonlib_api::raw_getTransactionsV2>, std::move(query),
      [transactions = std::move(transactions), last_processed_lt = last_processed_lt_, SelfId = actor_id(this),
       utime](td::Result<tonlib_api::object_ptr<tonlib_api::raw_transactions>> R) mutable {
        if (R.is_error()) {
          td::actor::send_closure(SelfId, &FabricContractWrapper::loaded_last_transactions, R.move_as_error());
          return;
        }
        auto obj = R.move_as_ok();
        for (auto& transaction : obj->transactions_) {
          if ((td::uint64)transaction->transaction_id_->lt_ <= last_processed_lt ||
              (double)transaction->utime_ < td::Clocks::system() - 86400 || transactions.size() >= 1000) {
            LOG(DEBUG) << "Stopping loading transactions (too many or too old)";
            td::actor::send_closure(SelfId, &FabricContractWrapper::loaded_last_transactions,
                                    std::make_pair(std::move(transactions), utime));
            return;
          }
          LOG(DEBUG) << "Adding trtansaction, lt=" << transaction->transaction_id_->lt_;
          transactions.push_back(std::move(transaction));
        }
        td::actor::send_closure(SelfId, &FabricContractWrapper::load_last_transactions, std::move(transactions),
                                std::move(obj->previous_transaction_id_), utime);
      });
}

void FabricContractWrapper::loaded_last_transactions(
    td::Result<std::pair<std::vector<tl_object_ptr<tonlib_api::raw_transaction>>, td::uint32>> R) {
  if (R.is_error()) {
    LOG(ERROR) << "Error during loading last transactions: " << R.move_as_error();
    alarm_timestamp().relax(process_transactions_at_ = td::Timestamp::in(30.0));
    return;
  }
  auto r = R.move_as_ok();
  auto transactions = std::move(r.first);
  td::uint32 utime = r.second;
  LOG(DEBUG) << "Finished loading " << transactions.size() << " transactions. sync_utime=" << utime;
  std::reverse(transactions.begin(), transactions.end());
  for (tl_object_ptr<tonlib_api::raw_transaction>& transaction : transactions) {
    LOG(DEBUG) << "Processing transaction tl=" << transaction->transaction_id_->lt_;
    last_processed_lt_ = transaction->transaction_id_->lt_;
    // transaction->in_msg_->source_->account_address_.empty() - message is external
    if (current_ext_message_ && current_ext_message_.value().sent &&
        transaction->in_msg_->source_->account_address_.empty()) {
      auto msg_data = dynamic_cast<tonlib_api::msg_dataRaw*>(transaction->in_msg_->msg_data_.get());
      if (msg_data == nullptr) {
        continue;
      }
      auto r_body = vm::std_boc_deserialize(msg_data->body_);
      if (r_body.is_error()) {
        LOG(WARNING) << "Invalid response from tonlib: " << r_body.move_as_error();
        continue;
      }
      td::Ref<vm::Cell> body = r_body.move_as_ok();
      vm::CellSlice cs(vm::NoVm(), body);
      if (cs.size() < 512 + 96) {
        continue;
      }
      cs.skip_first(512 + 64);
      auto seqno = (td::uint32)cs.fetch_ulong(32);
      if (seqno != current_ext_message_.value().seqno) {
        continue;
      }
      if (current_ext_message_.value().ext_msg_body_hash != body->get_hash().bits()) {
        do_send_external_message_finish(td::Status::Error("Another external message with the same seqno was accepted"));
        continue;
      }
      do_send_external_message_finish(&transaction->out_msgs_);
    }
  }
  for (tl_object_ptr<tonlib_api::raw_transaction>& transaction : transactions) {
    callback_->on_transaction(std::move(transaction));
  }
  if (current_ext_message_ && current_ext_message_.value().sent && current_ext_message_.value().timeout < utime) {
    do_send_external_message_finish(td::Status::Error("Timeout"));
  }
  alarm_timestamp().relax(process_transactions_at_ = td::Timestamp::in(10.0));
}

void FabricContractWrapper::run_get_method(
    std::string method, std::vector<tl_object_ptr<tonlib_api::tvm_StackEntry>> args,
    td::Promise<std::vector<tl_object_ptr<tonlib_api::tvm_StackEntry>>> promise) {
  ton::run_get_method(address_, client_, std::move(method), std::move(args), std::move(promise));
}

void FabricContractWrapper::send_internal_message(ContractAddress dest, td::RefInt256 coins, vm::CellSlice body,
                                                  td::Promise<td::Unit> promise) {
  td::Bits256 body_hash = vm::CellBuilder().append_cellslice(body).finalize_novm()->get_hash().bits();
  LOG(DEBUG) << "send_internal_message " << address_.to_string() << " -> " << dest.to_string() << ", " << coins
             << " nanoTON, body=" << body_hash.to_hex();
  CHECK(coins->sgn() >= 0);
  pending_messages_.push(PendingMessage{dest, std::move(coins), std::move(body), body_hash, std::move(promise)});
  if (!send_message_at_ && !current_ext_message_) {
    alarm_timestamp().relax(send_message_at_ = td::Timestamp::in(1.0));
  }
}

void FabricContractWrapper::do_send_external_message() {
  CHECK(!current_ext_message_);
  LOG(DEBUG) << "do_send_external message: " << pending_messages_.size() << " messages in queue";
  if (pending_messages_.empty()) {
    return;
  }
  current_ext_message_ = CurrentExtMessage();
  while (current_ext_message_.value().int_msgs.size() < 4 && !pending_messages_.empty()) {
    PendingMessage msg = std::move(pending_messages_.front());
    current_ext_message_.value().int_msgs.push_back(std::move(msg));
    pending_messages_.pop();
  }
  run_get_method(
      "get_wallet_params", {},
      [SelfId = actor_id(this)](td::Result<std::vector<tl_object_ptr<tonlib_api::tvm_StackEntry>>> R) {
        td::uint32 seqno = 0;
        td::uint32 subwallet_id = 0;
        td::Bits256 public_key = td::Bits256::zero();
        auto S = [&]() -> td::Status {
          TRY_RESULT(stack, std::move(R));
          if (stack.size() != 3) {
            return td::Status::Error(PSTRING() << "Method returned " << stack.size() << " values, 3 expected");
          }
          TRY_RESULT_PREFIX_ASSIGN(seqno, entry_to_int<td::uint32>(stack[0]), "Invalid seqno: ");
          TRY_RESULT_PREFIX_ASSIGN(subwallet_id, entry_to_int<td::uint32>(stack[1]), "Invalid subwallet_id: ");
          TRY_RESULT_PREFIX_ASSIGN(public_key, entry_to_bits256(stack[2]), "Invalid public_key: ");
          return td::Status::OK();
        }();
        if (S.is_error()) {
          td::actor::send_closure(SelfId, &FabricContractWrapper::do_send_external_message_finish,
                                  S.move_as_error_prefix("Failed to get wallet params: "));
          return;
        }
        td::actor::send_closure(SelfId, &FabricContractWrapper::do_send_external_message_cont, seqno, subwallet_id,
                                public_key);
      });
}

void FabricContractWrapper::do_send_external_message_cont(td::uint32 seqno, td::uint32 subwallet_id,
                                                          td::Bits256 public_key) {
  LOG(DEBUG) << "Got wallet params: seqno=" << seqno << ", subwallet_id=" << subwallet_id
             << ", key=" << public_key.to_hex();
  CHECK(current_ext_message_);
  current_ext_message_.value().seqno = seqno;
  current_ext_message_.value().timeout = (td::uint32)td::Clocks::system() + 45;
  vm::CellBuilder b;
  b.store_long(subwallet_id, 32);                          // subwallet id.
  b.store_long(current_ext_message_.value().timeout, 32);  // valid until
  b.store_long(seqno, 32);                                 // seqno
  for (const PendingMessage& msg : current_ext_message_.value().int_msgs) {
    vm::CellBuilder b2;
    b2.store_long(3 << 2, 6);                      // 0 ihr_disabled:Bool bounce:Bool bounced:Bool src:MsgAddressInt
    b2.append_cellslice(msg.dest.to_cellslice());  // dest:MsgAddressInt
    store_coins(b2, msg.value);                    // grams:Grams
    b2.store_zeroes(1 + 4 + 4 + 64 + 32 + 1);      // extre, ihr_fee, fwd_fee, created_lt, created_at, init
    // body:(Either X ^X)
    if (b2.remaining_bits() >= 1 + msg.body.size() && b2.remaining_refs() >= msg.body.size_refs()) {
      b2.store_zeroes(1);
      b2.append_cellslice(msg.body);
    } else {
      b2.store_ones(1);
      b2.store_ref(vm::CellBuilder().append_cellslice(msg.body).finalize_novm());
    }
    b.store_long(3, 8);               // mode
    b.store_ref(b2.finalize_novm());  // message
  }
  td::Ref<vm::Cell> to_sign = b.finalize_novm();
  td::BufferSlice hash(to_sign->get_hash().as_slice());
  LOG(DEBUG) << "Signing external message";
  td::actor::send_closure(
      keyring_, &keyring::Keyring::sign_message, PublicKey(pubkeys::Ed25519(public_key)).compute_short_id(),
      std::move(hash), [SelfId = actor_id(this), data = std::move(to_sign)](td::Result<td::BufferSlice> R) mutable {
        if (R.is_error()) {
          td::actor::send_closure(SelfId, &FabricContractWrapper::do_send_external_message_finish,
                                  R.move_as_error_prefix("Failed to sign message: "));
          return;
        }
        auto signature = R.move_as_ok();
        CHECK(signature.size() == 64);
        vm::CellBuilder b;
        b.store_bytes(signature);
        b.append_cellslice(vm::load_cell_slice(data));
        td::actor::send_closure(SelfId, &FabricContractWrapper::do_send_external_message_cont2, b.finalize_novm());
      });
}

void FabricContractWrapper::do_send_external_message_cont2(td::Ref<vm::Cell> ext_msg_body) {
  CHECK(current_ext_message_);
  LOG(DEBUG) << "Signed external message, sending: seqno=" << current_ext_message_.value().seqno;
  current_ext_message_.value().sent = true;
  current_ext_message_.value().ext_msg_body_hash = ext_msg_body->get_hash().bits();
  auto body = vm::std_boc_serialize(ext_msg_body).move_as_ok().as_slice().str();
  auto query = create_tl_object<tonlib_api::raw_createAndSendMessage>(
      create_tl_object<tonlib_api::accountAddress>(address_.to_string()), "", std::move(body));
  td::actor::send_closure(client_, &tonlib::TonlibClientWrapper::send_request<tonlib_api::raw_createAndSendMessage>,
                          std::move(query), [SelfId = actor_id(this)](td::Result<tl_object_ptr<tonlib_api::ok>> R) {
                            if (R.is_error()) {
                              td::actor::send_closure(SelfId, &FabricContractWrapper::do_send_external_message_finish,
                                                      R.move_as_error_prefix("Failed to send message: "));
                            } else {
                              LOG(DEBUG) << "External message was sent to liteserver";
                            }
                          });
}

void FabricContractWrapper::do_send_external_message_finish(
    td::Result<const std::vector<tl_object_ptr<tonlib_api::raw_message>>*> R) {
  CHECK(current_ext_message_);
  if (R.is_error()) {
    LOG(DEBUG) << "Failed to send external message seqno=" << current_ext_message_.value().seqno << ": " << R.error();
    for (auto& msg : current_ext_message_.value().int_msgs) {
      msg.promise.set_error(R.error().clone());
    }
  } else {
    LOG(DEBUG) << "External message seqno=" << current_ext_message_.value().seqno << " was sent";
    const auto& out_msgs = *R.ok();
    auto& msgs = current_ext_message_.value().int_msgs;
    for (const auto& out_msg : out_msgs) {
      ContractAddress dest = ContractAddress::parse(out_msg->destination_->account_address_).move_as_ok();
      td::RefInt256 value = td::make_refint((td::uint64)out_msg->value_);
      td::Bits256 body_hash;
      body_hash.as_slice().copy_from(out_msg->body_hash_);
      bool found = false;
      for (size_t i = 0; i < msgs.size(); ++i) {
        if (msgs[i].dest == dest && msgs[i].value->cmp(*value) == 0 && msgs[i].body_hash == body_hash) {
          LOG(DEBUG) << "Internal message was sent dest=" << dest.to_string() << ", value=" << value
                     << ", body_hash=" << body_hash.to_hex();
          msgs[i].promise.set_result(td::Unit());
          msgs.erase(msgs.begin() + i);
          found = true;
          break;
        }
      }
      if (!found) {
        LOG(DEBUG) << "Unexpected internal message was sent: dest=" << dest.to_string() << " value=" << value
                   << " body_hash=" << body_hash;
      }
    }
    for (auto& msg : msgs) {
      LOG(DEBUG) << "Internal message WAS NOT SENT dest=" << msg.dest.to_string() << ", value=" << msg.value
                 << ", body_hash=" << msg.body_hash.to_hex();
      msg.promise.set_result(td::Status::Error("External message was accepted, but internal message was not sent"));
    }
  }
  current_ext_message_ = {};
  if (!pending_messages_.empty()) {
    do_send_external_message();
  }
}

bool store_coins(vm::CellBuilder& b, const td::RefInt256& x) {
  unsigned len = (((unsigned)x->bit_size(false) + 7) >> 3);
  if (len >= 16) {
    return false;
  }
  return b.store_long_bool(len, 4) && b.store_int256_bool(*x, len * 8, false);
}

bool store_coins(vm::CellBuilder& b, td::uint64 x) {
  return store_coins(b, td::make_refint(x));
}

td::Result<FabricContractInit> generate_fabric_contract(td::actor::ActorId<keyring::Keyring> keyring) {
  auto private_key = PrivateKey{privkeys::Ed25519::random()};
  td::Bits256 public_key = private_key.compute_public_key().ed25519_value().raw();

  td::Slice code_boc(STORAGE_PROVIDER_CODE, sizeof(STORAGE_PROVIDER_CODE));
  TRY_RESULT(code, vm::std_boc_deserialize(code_boc));

  LOG(DEBUG) << "Generating storage provider state init. code_hash=" << code->get_hash().to_hex()
             << " public_key=" << public_key.to_hex();

  vm::CellBuilder b;
  b.store_long(0, 32);                   // seqno
  b.store_long(0, 32);                   // subwallet_id
  b.store_bytes(public_key.as_slice());  // public_key
  b.store_long(0, 1);                    // accept_new_contracts (false by default)
  store_coins(b, 1'000'000);             // rate_per_mb_day
  b.store_long(86400, 32);               // max_span
  b.store_long(1 << 20, 64);             // min_file_size
  b.store_long(1 << 30, 64);             // max_file_size
  td::Ref<vm::Cell> data = b.finalize_novm();

  // _ split_depth:(Maybe (## 5)) special:(Maybe TickTock)
  //   code:(Maybe ^Cell) data:(Maybe ^Cell)
  //   library:(HashmapE 256 SimpleLib) = StateInit;
  td::Ref<vm::Cell> state_init =
      vm::CellBuilder().store_long(0b00110, 5).store_ref(std::move(code)).store_ref(std::move(data)).finalize_novm();
  ContractAddress address{basechainId, state_init->get_hash().bits()};

  // Message body
  b = vm::CellBuilder();
  b.store_long(0, 32);                                                 // subwallet_id
  b.store_long((td::uint32)td::Clocks::system() + 3600 * 24 * 7, 32);  // valid_until
  b.store_long(0, 32);                                                 // seqno
  td::Ref<vm::Cell> to_sign = b.finalize_novm();
  TRY_RESULT(decryptor, private_key.create_decryptor());
  TRY_RESULT(signature, decryptor->sign(to_sign->get_hash().as_slice()));
  CHECK(signature.size() == 64);
  td::Ref<vm::Cell> msg_body =
      vm::CellBuilder().store_bytes(signature).append_cellslice(vm::CellSlice(vm::NoVm(), to_sign)).finalize_novm();

  td::actor::send_closure(keyring, &keyring::Keyring::add_key, private_key, false,
                          [](td::Result<td::Unit> R) { R.ensure(); });
  return FabricContractInit{address, state_init, msg_body};
}

td::Ref<vm::Cell> create_new_contract_message_body(td::Ref<vm::Cell> info, td::Bits256 microchunk_hash,
                                                   td::uint64 query_id, td::RefInt256 rate, td::uint32 max_span) {
  // new_storage_contract#00000001 query_id:uint64 info:(^ TorrentInfo) microchunk_hash:uint256
  //     expected_rate:Coins expected_max_span:uint32 = NewStorageContract;
  vm::CellBuilder b;
  b.store_long(0x107c49ef, 32);  // const op::offer_storage_contract = 0x107c49ef;
  b.store_long(query_id, 64);
  b.store_ref(std::move(info));
  b.store_bytes(microchunk_hash.as_slice());
  store_coins(b, rate);
  b.store_long(max_span, 32);
  return b.finalize_novm();
}

void get_storage_contract_data(ContractAddress address, td::actor::ActorId<tonlib::TonlibClientWrapper> client,
                               td::Promise<StorageContractData> promise) {
  run_get_method(
      address, client, "get_storage_contract_data", {},
      promise.wrap([](std::vector<tl_object_ptr<tonlib_api::tvm_StackEntry>> stack) -> td::Result<StorageContractData> {
        if (stack.size() < 11) {
          return td::Status::Error("Too few entries");
        }
        // active, balance, provider, merkle_hash, file_size, next_proof, rate_per_mb_day, max_span, last_proof_time,
        // client, torrent_hash
        TRY_RESULT(active, entry_to_int<int>(stack[0]));
        TRY_RESULT(balance, entry_to_int<td::RefInt256>(stack[1]));
        TRY_RESULT(microchunk_hash, entry_to_bits256(stack[3]));
        TRY_RESULT(file_size, entry_to_int<td::uint64>(stack[4]));
        TRY_RESULT(next_proof, entry_to_int<td::uint64>(stack[5]));
        TRY_RESULT(rate_per_mb_day, entry_to_int<td::RefInt256>(stack[6]));
        TRY_RESULT(max_span, entry_to_int<td::uint32>(stack[7]));
        TRY_RESULT(last_proof_time, entry_to_int<td::uint32>(stack[8]));
        TRY_RESULT(torrent_hash, entry_to_bits256(stack[10]));
        return StorageContractData{(bool)active,    balance,  microchunk_hash, file_size,   next_proof,
                                   rate_per_mb_day, max_span, last_proof_time, torrent_hash};
      }));
}

}  // namespace ton
