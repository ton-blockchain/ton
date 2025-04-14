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
  std::shared_ptr<block::Config> config_;
  vm::Dictionary libraries_;
  int vm_log_verbosity_;
  ton::UnixTime unixtime_;
  ton::LogicalTime lt_;
  td::BitArray<256> rand_seed_;
  bool ignore_chksig_;
  bool debug_enabled_;
  td::Ref<vm::Tuple> prev_blocks_info_;

public:
  TransactionEmulator(std::shared_ptr<block::Config> config, int vm_log_verbosity = 0) :
    config_(std::move(config)), libraries_(256), vm_log_verbosity_(vm_log_verbosity),
    unixtime_(0), lt_(0), rand_seed_(td::BitArray<256>::zero()), ignore_chksig_(false), debug_enabled_(false) {
  }

  struct EmulationResult {
    std::string vm_log;
    double elapsed_time;

    EmulationResult(std::string vm_log_, double elapsed_time_) : vm_log(vm_log_), elapsed_time(elapsed_time_) {}
    virtual ~EmulationResult() = default;
  };

  struct EmulationSuccess : EmulationResult {
    td::Ref<vm::Cell> transaction;
    block::Account account;
    td::Ref<vm::Cell> actions;

    EmulationSuccess(td::Ref<vm::Cell> transaction_, block::Account account_, std::string vm_log_,
                     td::Ref<vm::Cell> actions_, double elapsed_time_)
        : EmulationResult(vm_log_, elapsed_time_)
        , transaction(transaction_)
        , account(std::move(account_))
        , actions(actions_) {
    }
  };

  struct EmulationExternalNotAccepted: EmulationResult {
    int vm_exit_code;

    EmulationExternalNotAccepted(std::string vm_log_, int vm_exit_code_, double elapsed_time_) :
      EmulationResult(vm_log_, elapsed_time_), vm_exit_code(vm_exit_code_)
    {}
  };

  struct EmulationChain {
    std::vector<td::Ref<vm::Cell>> transactions;
    block::Account account;
  };

  const block::Config& get_config() {
    return *config_;
  }

  ton::UnixTime get_unixtime() {
    return unixtime_;
  }

  td::Result<std::unique_ptr<EmulationResult>> emulate_transaction(
      block::Account&& account, td::Ref<vm::Cell> msg_root, ton::UnixTime utime, ton::LogicalTime lt, int trans_type);

  td::Result<EmulationSuccess> emulate_transaction(block::Account&& account, td::Ref<vm::Cell> original_trans);
  td::Result<EmulationChain> emulate_transactions_chain(block::Account&& account, std::vector<td::Ref<vm::Cell>>&& original_transactions);

  void set_unixtime(ton::UnixTime unixtime);
  void set_lt(ton::LogicalTime lt);
  void set_rand_seed(td::BitArray<256>& rand_seed);
  void set_ignore_chksig(bool ignore_chksig);
  void set_config(std::shared_ptr<block::Config> config);
  void set_libs(vm::Dictionary &&libs);
  void set_debug_enabled(bool debug_enabled);
  void set_prev_blocks_info(td::Ref<vm::Tuple> prev_blocks_info);

private:
  bool check_state_update(const block::Account& account, const block::gen::Transaction::Record& trans);

  td::Result<std::unique_ptr<block::transaction::Transaction>> create_transaction(
                                                         td::Ref<vm::Cell> msg_root, block::Account* acc,
                                                         ton::UnixTime utime, ton::LogicalTime lt, int trans_type,
                                                         block::StoragePhaseConfig* storage_phase_cfg,
                                                         block::ComputePhaseConfig* compute_phase_cfg,
                                                         block::ActionPhaseConfig* action_phase_cfg);
};
} // namespace emulator
