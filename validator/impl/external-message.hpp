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

#include "adnl/utils.hpp"
#include "auto/tl/ton_api.h"
#include "block/transaction.h"
#include "interfaces/validator-manager.h"
#include "validator/interfaces/external-message.h"

namespace ton {

namespace validator {

class ExtMessageQ : public ExtMessage {
  td::Ref<vm::Cell> root_;
  AccountIdPrefixFull addr_prefix_;
  td::BufferSlice data_;
  Hash hash_;
  Hash hash_norm_;
  ton::WorkchainId wc_;
  ton::StdSmcAddress addr_;

 public:
  AccountIdPrefixFull shard() const override {
    return addr_prefix_;
  }
  td::BufferSlice serialize() const override {
    return data_.clone();
  }
  td::Ref<vm::Cell> root_cell() const override {
    return root_;
  }
  Hash hash() const override {
    return hash_;
  }
  Hash hash_norm() const override {
    return hash_norm_;
  }
  ton::WorkchainId wc() const override {
    return wc_;
  }

  ton::StdSmcAddress addr() const override {
    return addr_;
  }

  ExtMessageQ(td::BufferSlice data, td::Ref<vm::Cell> root, AccountIdPrefixFull shard, ton::WorkchainId wc,
              ton::StdSmcAddress addr, Hash hash, Hash hash_norm);
  static td::Result<td::Ref<ExtMessageQ>> create_ext_message(td::BufferSlice data,
                                                             block::SizeLimitsConfig::ExtMsgLimits limits);
  static td::Result<td::Ref<ExtMessageQ>> create_ext_message(td::Ref<vm::Cell> root);  // Skips message size checks.

  // Transaction phase configs prepared from a (config, wc, utime) triple. Reusable across many
  // messages (the collator reuses the equivalent structs for all transactions of a block).
  // Not movable: storage_phase_cfg points into storage_prices.
  struct ExecutionConfig {
    td::Ref<vm::Cell> old_mparams;
    std::vector<block::StoragePrices> storage_prices;
    block::StoragePhaseConfig storage_phase_cfg{&storage_prices};
    td::BitArray<256> rand_seed;
    block::ComputePhaseConfig compute_phase_cfg;
    block::ActionPhaseConfig action_phase_cfg;
    block::SerializeConfig serialize_config;
    td::RefInt256 masterchain_create_fee, basechain_create_fee;

    static td::Result<std::unique_ptr<ExecutionConfig>> create(const block::ConfigInfo& config, ton::WorkchainId wc,
                                                               UnixTime utime, bool with_vm_log);
  };
  static td::Status run_message_on_account(ton::WorkchainId wc, block::Account* acc, UnixTime utime, LogicalTime lt,
                                           td::Ref<vm::Cell> msg_root, ExecutionConfig& exec_config);
  static td::Status run_message_on_account(ton::WorkchainId wc, block::Account* acc, UnixTime utime, LogicalTime lt,
                                           td::Ref<vm::Cell> msg_root, const block::ConfigInfo& config);
};

td::Result<td::Bits256> get_ext_in_msg_hash_norm(td::Ref<vm::Cell> ext_in_msg_cell);

class WalletMessageProcessor {
 public:
  virtual ~WalletMessageProcessor() = default;
  virtual std::string name() const = 0;
  virtual td::Result<std::pair<td::uint32, UnixTime>> parse_message(td::Ref<vm::Cell> msg_root) const = 0;
  virtual td::Result<td::uint32> get_wallet_seqno(td::Ref<vm::Cell> data_root) const = 0;
  virtual td::Result<td::Ref<vm::Cell>> set_wallet_seqno(td::Ref<vm::Cell> data_root, td::uint32 new_seqno) const = 0;

  static const WalletMessageProcessor* get(td::Bits256 code_hash);
};

}  // namespace validator

}  // namespace ton
