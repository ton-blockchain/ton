#pragma once
#include "crypto/common/refcnt.hpp"
#include "ton/ton-types.h"
#include "crypto/vm/cells.h"
#include "block/transaction.h"
#include "block/block-auto.h"
#include "block/block-parse.h"
#include "block/mc-config.h"

namespace emulator {
class TransactionEmulator {
  block::Account account_;
  block::Config config_;
  vm::Dictionary libraries_;

public:
  TransactionEmulator(block::Account&& account, block::Config&& config, vm::Dictionary&& libraries) : 
    account_(std::move(account)), config_(std::move(config)), libraries_(std::move(libraries)) {
  }

  const block::Account& get_account() const {
    return account_;
  }

  const block::Config* get_config() {
    return &config_;
  }

  td::Result<td::Ref<vm::Cell>> emulate_transaction(td::Ref<vm::Cell> msg_root);

private:
  td::Status fetch_config_params(const block::Config& config,
                                              td::Ref<vm::Cell>* old_mparams,
                                              std::vector<block::StoragePrices>* storage_prices,
                                              block::StoragePhaseConfig* storage_phase_cfg,
                                              td::BitArray<256>* rand_seed,
                                              block::ComputePhaseConfig* compute_phase_cfg,
                                              block::ActionPhaseConfig* action_phase_cfg,
                                              td::RefInt256* masterchain_create_fee,
                                              td::RefInt256* basechain_create_fee,
                                              ton::WorkchainId wc);

  td::Result<std::unique_ptr<block::Transaction>> create_ordinary_transaction(td::Ref<vm::Cell> msg_root,
                                                         block::Account* acc,
                                                         ton::UnixTime utime, ton::LogicalTime lt,
                                                         block::StoragePhaseConfig* storage_phase_cfg,
                                                         block::ComputePhaseConfig* compute_phase_cfg,
                                                         block::ActionPhaseConfig* action_phase_cfg,
                                                         bool external, ton::LogicalTime after_lt);
};
} // namespace emulator