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
  block::Config config_;
  vm::Dictionary libraries_;
  bool ignore_chksig_;

public:
  TransactionEmulator(block::Config&& config, vm::Dictionary&& libraries, bool ignore_chksig) :
    config_(std::move(config)), libraries_(std::move(libraries)), ignore_chksig_(ignore_chksig) {
  }

  struct EmulationResult {
    td::Ref<vm::Cell> transaction;
    block::Account account;
  };

  struct EmulationResults {
    std::vector<td::Ref<vm::Cell>> transactions;
    block::Account account;
  };

  const block::Config& get_config() {
    return config_;
  }

  td::Result<EmulationResult> emulate_transaction(
      block::Account&& account, td::Ref<vm::Cell> msg_root,
      ton::UnixTime utime = 0, ton::LogicalTime lt = 0,
      int trans_type = block::transaction::Transaction::tr_ord,
      td::BitArray<256>* rand_seed = nullptr);

  td::Result<EmulationResult> emulate_transaction(block::Account&& account, td::Ref<vm::Cell> original_trans, td::BitArray<256>* rand_seed);
  td::Result<EmulationResults> emulate_transactions(block::Account&& account, std::vector<td::Ref<vm::Cell>>&& original_transactions, td::BitArray<256>* rand_seed);

private:
  bool check_state_update(const block::Account& account, const block::gen::Transaction::Record& trans);

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

  td::Result<std::unique_ptr<block::transaction::Transaction>> create_transaction(
                                                         td::Ref<vm::Cell> msg_root, block::Account* acc,
                                                         ton::UnixTime utime, ton::LogicalTime lt, int trans_type,
                                                         block::StoragePhaseConfig* storage_phase_cfg,
                                                         block::ComputePhaseConfig* compute_phase_cfg,
                                                         block::ActionPhaseConfig* action_phase_cfg);
};
} // namespace emulator
