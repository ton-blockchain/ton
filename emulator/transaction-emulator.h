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
  int vm_log_verbosity_;

public:
  TransactionEmulator(block::Config&& config, vm::Dictionary&& libraries, int vm_log_verbosity = 0) : 
    config_(std::move(config)), libraries_(std::move(libraries)), vm_log_verbosity_(vm_log_verbosity) {
  }

  struct EmulationResult {
    std::string vm_log;

    EmulationResult(std::string vm_log_) : vm_log(vm_log_) {}
    virtual ~EmulationResult() = default;
  };

  struct EmulationSuccess: EmulationResult {
    td::Ref<vm::Cell> transaction;
    block::Account account;

    EmulationSuccess(td::Ref<vm::Cell> transaction_, block::Account account_, std::string vm_log_) : 
      EmulationResult(vm_log_), transaction(transaction_), account(account_) 
    {}
  };

  struct EmulationExternalNotAccepted: EmulationResult {
    int vm_exit_code;

    EmulationExternalNotAccepted(std::string vm_log_, int vm_exit_code_) : 
      EmulationResult(vm_log_), vm_exit_code(vm_exit_code_) 
    {}
  };

  struct EmulationChain {
    std::vector<td::Ref<vm::Cell>> transactions;
    block::Account account;
  };

  const block::Config& get_config() {
    return config_;
  }

  td::Result<std::unique_ptr<EmulationResult>> emulate_transaction(
      block::Account&& account, td::Ref<vm::Cell> msg_root, ton::UnixTime utime, ton::LogicalTime lt,
      int trans_type, td::BitArray<256>* rand_seed, bool ignore_chksig);

  td::Result<EmulationSuccess> emulate_transaction(block::Account&& account, td::Ref<vm::Cell> original_trans, td::BitArray<256>* rand_seed);
  td::Result<EmulationChain> emulate_transactions_chain(block::Account&& account, std::vector<td::Ref<vm::Cell>>&& original_transactions, td::BitArray<256>* rand_seed);

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
