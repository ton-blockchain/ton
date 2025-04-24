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

    Copyright 2017-2020 Telegram Systems LLP
*/
#pragma once

#include "td/utils/common.h"
#include "Ed25519.h"
#include "block/block.h"
#include "block/block-parse.h"
#include "vm/cells/CellString.h"

#include "SmartContract.h"
#include "SmartContractCode.h"
#include "GenericAccount.h"

#include <algorithm>

namespace ton {
class WalletInterface : public SmartContract {
 public:
  static constexpr uint32_t EncryptedCommentOp = 0x2167da4b;
  struct Gift {
    block::StdAddress destination;
    td::int64 gramms;
    td::Ref<vm::Cell> extra_currencies;
    td::int32 send_mode{-1};

    bool is_encrypted{false};
    std::string message;

    td::Ref<vm::Cell> body;
    td::Ref<vm::Cell> init_state;
  };
  struct DefaultInitData {
    td::SecureString public_key;
    td::uint32 wallet_id{0};
    td::uint32 seqno{0};
    DefaultInitData() = default;
    DefaultInitData(td::Slice key, td::uint32 wallet_id) : public_key(key), wallet_id(wallet_id) {
    }
  };

  WalletInterface(State state) : SmartContract(std::move(state)) {
  }

  virtual ~WalletInterface() {
  }

  virtual size_t get_max_gifts_size() const = 0;
  virtual size_t get_max_message_size() const = 0;
  virtual td::Result<td::Ref<vm::Cell>> make_a_gift_message(const td::Ed25519::PrivateKey &private_key,
                                                            td::uint32 valid_until, td::Span<Gift> gifts) const = 0;

  virtual td::Result<td::uint32> get_seqno() const;
  virtual td::Result<td::uint32> get_wallet_id() const;
  virtual td::Result<td::uint64> get_balance(td::uint64 account_balance, td::uint32 now) const;
  virtual td::Result<td::Ed25519::PublicKey> get_public_key() const;

  td::Result<td::Ref<vm::Cell>> get_init_message(const td::Ed25519::PrivateKey &private_key,
                                                 td::uint32 valid_until = std::numeric_limits<td::uint32>::max()) const;

  static td::Ref<vm::Cell> create_int_message(const Gift &gift);

 private:
  static void store_gift_message(vm::CellBuilder &cb, const Gift &gift);
};

template <class WalletT, class TraitsT>
class WalletBase : public WalletInterface {
 public:
  using Traits = TraitsT;
  using InitData = typename Traits::InitData;

  explicit WalletBase(State state) : WalletInterface(std::move(state)) {
  }

  size_t get_max_gifts_size() const override {
    return Traits::max_gifts_size;
  }
  size_t get_max_message_size() const override {
    return Traits::max_message_size;
  }

  static td::Ref<WalletT> create(State state) {
    return td::Ref<WalletT>(true, std::move(state));
  }
  static td::Ref<vm::Cell> get_init_code(int revision) {
    return SmartContractCode::get_code(get_code_type(), revision);
  };
  static State get_init_state(int revision, const InitData &init_data) {
    return {get_init_code(revision), WalletT::get_init_data(init_data)};
  }
  static SmartContractCode::Type get_code_type() {
    return Traits::code_type;
  }
  static td::optional<td::int32> guess_revision(const vm::Cell::Hash &code_hash) {
    for (auto revision : ton::SmartContractCode::get_revisions(get_code_type())) {
      auto code = get_init_code(revision);
      if (code->get_hash() == code_hash) {
        return revision;
      }
    }
    return {};
  }
  static td::Span<td::int32> get_revisions() {
    return ton::SmartContractCode::get_revisions(get_code_type());
  }
  static td::optional<td::int32> guess_revision(block::StdAddress &address, const InitData &init_data) {
    for (auto revision : get_revisions()) {
      if (WalletT(get_init_state(revision, init_data)).get_address(address.workchain) == address) {
        return revision;
      }
    }
    return {};
  }
  static td::Ref<WalletT> create(const InitData &init_data, int revision) {
    return td::Ref<WalletT>(true, State{get_init_code(revision), WalletT::get_init_data(init_data)});
  }
  CntObject *make_copy() const override {
    return new WalletT(get_state());
  }
};

}  // namespace ton
