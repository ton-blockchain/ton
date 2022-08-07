#pragma once
#include "crypto/common/refcnt.hpp"
#include "ton/ton-types.h"
#include "crypto/vm/cells.h"
#include "block/transaction.h"
#include "block/block-auto.h"
#include "block/block-parse.h"
#include "block/mc-config.h"

class TransactionEmulator {
  block::Account account_;
  std::unique_ptr<block::ConfigInfo> config_;

 public:
  TransactionEmulator(block::Account&& account, std::unique_ptr<block::ConfigInfo>&& config) : 
    account_(std::move(account)), config_(std::move(config)) {
  }

  const block::Account& get_account() const {
    return account_;
  }

  std::unique_ptr<block::ConfigInfo> get_config() {
    return std::move(config_);
  }

  td::Result<td::Ref<vm::Cell>> emulate_transaction(td::Ref<vm::Cell> msg_root);
};
