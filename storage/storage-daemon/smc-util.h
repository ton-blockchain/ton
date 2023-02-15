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
#include "ton/ton-types.h"
#include "crypto/vm/cellslice.h"
#include "block/block-parse.h"
#include "td/actor/actor.h"
#include "tonlib/tonlib/TonlibClientWrapper.h"
#include <queue>
#include "keyring/keyring.h"

namespace ton {

struct ContractAddress {
  WorkchainId wc = workchainIdNotYet;
  td::Bits256 addr = td::Bits256::zero();

  ContractAddress() = default;
  ContractAddress(WorkchainId wc, td::Bits256 addr) : wc(wc), addr(addr) {
  }

  std::string to_string() const {
    return PSTRING() << wc << ":" << addr.to_hex();
  }
  td::Ref<vm::CellSlice> to_cellslice() const {
    return block::tlb::t_MsgAddressInt.pack_std_address(wc, addr);
  }

  static td::Result<ContractAddress> parse(td::Slice s) {
    TRY_RESULT(x, block::StdAddress::parse(s));
    return ContractAddress(x.workchain, x.addr);
  }

  bool operator==(const ContractAddress& other) const {
    return wc == other.wc && addr == other.addr;
  }
  bool operator!=(const ContractAddress& other) const {
    return !(*this == other);
  }
  bool operator<(const ContractAddress& other) const {
    return wc == other.wc ? addr < other.addr : wc < other.wc;
  }
};

void run_get_method(ContractAddress address, td::actor::ActorId<tonlib::TonlibClientWrapper> client, std::string method,
                    std::vector<tl_object_ptr<tonlib_api::tvm_StackEntry>> args,
                    td::Promise<std::vector<tl_object_ptr<tonlib_api::tvm_StackEntry>>> promise);
void check_contract_exists(ContractAddress address, td::actor::ActorId<tonlib::TonlibClientWrapper> client,
                           td::Promise<bool> promise);
void get_contract_balance(ContractAddress address, td::actor::ActorId<tonlib::TonlibClientWrapper> client,
                          td::Promise<td::RefInt256> promise);

class FabricContractWrapper : public td::actor::Actor {
 public:
  class Callback {
   public:
    virtual ~Callback() = default;
    virtual void on_transaction(tl_object_ptr<tonlib_api::raw_transaction> transaction) = 0;
  };

  explicit FabricContractWrapper(ContractAddress address, td::actor::ActorId<tonlib::TonlibClientWrapper> client,
                                 td::actor::ActorId<keyring::Keyring> keyring, td::unique_ptr<Callback> callback,
                                 td::uint64 last_processed_lt);

  void start_up() override;
  void alarm() override;

  void run_get_method(std::string method, std::vector<tl_object_ptr<tonlib_api::tvm_StackEntry>> args,
                      td::Promise<std::vector<tl_object_ptr<tonlib_api::tvm_StackEntry>>> promise);
  void send_internal_message(ContractAddress dest, td::RefInt256 coins, vm::CellSlice body,
                             td::Promise<td::Unit> promise);

 private:
  ContractAddress address_;
  td::actor::ActorId<tonlib::TonlibClientWrapper> client_;
  td::actor::ActorId<keyring::Keyring> keyring_;
  td::unique_ptr<Callback> callback_;

  td::Timestamp process_transactions_at_ = td::Timestamp::now();
  td::uint64 last_processed_lt_ = 0;

  struct PendingMessage {
    ContractAddress dest;
    td::RefInt256 value;
    vm::CellSlice body;
    td::Bits256 body_hash;
    td::Promise<td::Unit> promise;
  };
  struct CurrentExtMessage {
    std::vector<PendingMessage> int_msgs;
    td::uint32 seqno = 0;
    bool sent = false;
    td::Bits256 ext_msg_body_hash = td::Bits256::zero();
    td::uint32 timeout = 0;
  };
  std::queue<PendingMessage> pending_messages_;
  td::Timestamp send_message_at_ = td::Timestamp::never();
  td::optional<CurrentExtMessage> current_ext_message_;

  void load_transactions();
  void load_last_transactions(std::vector<tl_object_ptr<tonlib_api::raw_transaction>> transactions,
                              tl_object_ptr<tonlib_api::internal_transactionId> next_id, td::uint32 utime);
  void loaded_last_transactions(
      td::Result<std::pair<std::vector<tl_object_ptr<tonlib_api::raw_transaction>>, td::uint32>> R);

  void do_send_external_message();
  void do_send_external_message_cont(td::uint32 seqno, td::uint32 subwallet_id, td::Bits256 public_key);
  void do_send_external_message_cont2(td::Ref<vm::Cell> ext_msg_body);
  void do_send_external_message_finish(td::Result<const std::vector<tl_object_ptr<tonlib_api::raw_message>>*> R);
};

template <typename T>
inline td::Result<T> entry_to_int(const tl_object_ptr<tonlib_api::tvm_StackEntry>& entry) {
  auto num = dynamic_cast<tonlib_api::tvm_stackEntryNumber*>(entry.get());
  if (num == nullptr) {
    return td::Status::Error("Unexpected value type");
  }
  return td::to_integer_safe<T>(num->number_->number_);
}

template <>
inline td::Result<td::RefInt256> entry_to_int<td::RefInt256>(const tl_object_ptr<tonlib_api::tvm_StackEntry>& entry) {
  auto num = dynamic_cast<tonlib_api::tvm_stackEntryNumber*>(entry.get());
  if (num == nullptr) {
    return td::Status::Error("Unexpected value type");
  }
  auto x = td::dec_string_to_int256(num->number_->number_);
  if (x.is_null()) {
    return td::Status::Error("Invalid integer value");
  }
  return x;
}

inline td::Result<td::Bits256> entry_to_bits256(const tl_object_ptr<tonlib_api::tvm_StackEntry>& entry) {
  TRY_RESULT(x, entry_to_int<td::RefInt256>(entry));
  td::Bits256 bits;
  if (!x->export_bytes(bits.data(), 32, false)) {
    return td::Status::Error("Invalid int256");
  }
  return bits;
}

bool store_coins(vm::CellBuilder& b, const td::RefInt256& x);
bool store_coins(vm::CellBuilder& b, td::uint64 x);

struct FabricContractInit {
  ContractAddress address;
  td::Ref<vm::Cell> state_init;
  td::Ref<vm::Cell> msg_body;
};
td::Result<FabricContractInit> generate_fabric_contract(td::actor::ActorId<keyring::Keyring> keyring);

td::Ref<vm::Cell> create_new_contract_message_body(td::Ref<vm::Cell> info, td::Bits256 microchunk_hash,
                                                   td::uint64 query_id, td::RefInt256 rate, td::uint32 max_span);

struct StorageContractData {
  bool active;
  td::RefInt256 balance;
  td::Bits256 microchunk_hash;
  td::uint64 file_size;
  td::uint64 next_proof;
  td::RefInt256 rate_per_mb_day;
  td::uint32 max_span;
  td::uint32 last_proof_time;
  td::Bits256 torrent_hash;
};

void get_storage_contract_data(ContractAddress address, td::actor::ActorId<tonlib::TonlibClientWrapper> client,
                               td::Promise<StorageContractData> promise);

}  // namespace ton