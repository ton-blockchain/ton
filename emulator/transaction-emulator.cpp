#include <string>
#include "transaction-emulator.h"
#include "crypto/common/refcnt.hpp"
#include "vm/vm.h"
#include "tdutils/td/utils/Time.h"

using td::Ref;
using namespace std::string_literals;

namespace emulator {
td::Result<std::unique_ptr<TransactionEmulator::EmulationResult>> TransactionEmulator::emulate_transaction(
    block::Account&& account, td::Ref<vm::Cell> msg_root, ton::UnixTime utime, ton::LogicalTime lt, int trans_type) {

    td::Ref<vm::Cell> old_mparams;
    std::vector<block::StoragePrices> storage_prices;
    block::StoragePhaseConfig storage_phase_cfg{&storage_prices};
    block::ComputePhaseConfig compute_phase_cfg;
    block::ActionPhaseConfig action_phase_cfg;
    block::SerializeConfig serialize_config;
    td::RefInt256 masterchain_create_fee, basechain_create_fee;
    
    if (!utime) {
      utime = unixtime_;
    }
    if (!utime) {
      utime = (unsigned)std::time(nullptr);
    }

    auto fetch_res = block::FetchConfigParams::fetch_config_params(
        *config_, prev_blocks_info_, &old_mparams, &storage_prices, &storage_phase_cfg, &rand_seed_, &compute_phase_cfg,
        &action_phase_cfg, &serialize_config, &masterchain_create_fee, &basechain_create_fee, account.workchain, utime);
    if(fetch_res.is_error()) {
        return fetch_res.move_as_error_prefix("cannot fetch config params ");
    }

    TRY_STATUS(vm::init_vm(debug_enabled_));

    if (!lt) {
      lt = lt_;
    }
    if (!lt) {
      lt = (account.last_trans_lt_ / block::ConfigInfo::get_lt_align() + 1) * block::ConfigInfo::get_lt_align(); // next block after account_.last_trans_lt_
    }
    account.block_lt = lt - lt % block::ConfigInfo::get_lt_align();

    compute_phase_cfg.libraries = std::make_unique<vm::Dictionary>(libraries_);
    compute_phase_cfg.ignore_chksig = ignore_chksig_;
    compute_phase_cfg.with_vm_log = true;
    compute_phase_cfg.vm_log_verbosity = vm_log_verbosity_;

    double start_time = td::Time::now();
    auto res = create_transaction(msg_root, &account, utime, lt, trans_type,
                                                    &storage_phase_cfg, &compute_phase_cfg,
                                                    &action_phase_cfg);
    double elapsed = td::Time::now() - start_time;

    if(res.is_error()) {
      return res.move_as_error_prefix("cannot run message on account ");
    }
    std::unique_ptr<block::transaction::Transaction> trans = res.move_as_ok();

    if (!trans->compute_phase->accepted && trans->in_msg_extern) {
      auto vm_log = trans->compute_phase->vm_log;
      auto vm_exit_code = trans->compute_phase->exit_code;
      return std::make_unique<TransactionEmulator::EmulationExternalNotAccepted>(std::move(vm_log), vm_exit_code, elapsed);
    }

    if (!trans->serialize(serialize_config)) {
      return td::Status::Error(-669,"cannot serialize new transaction for smart contract "s + trans->account.addr.to_hex());
    }

    auto trans_root = trans->commit(account);
    if (trans_root.is_null()) {
      return td::Status::Error(PSLICE() << "cannot commit new transaction for smart contract");
    }

    return std::make_unique<TransactionEmulator::EmulationSuccess>(std::move(trans_root), std::move(account), 
      std::move(trans->compute_phase->vm_log), std::move(trans->compute_phase->actions), elapsed);
}

td::Result<TransactionEmulator::EmulationSuccess> TransactionEmulator::emulate_transaction(block::Account&& account, td::Ref<vm::Cell> original_trans) {

    block::gen::Transaction::Record record_trans;
    if (!tlb::unpack_cell(original_trans, record_trans)) {
      return td::Status::Error("Failed to unpack Transaction");
    }

    ton::LogicalTime lt = record_trans.lt;
    ton::UnixTime utime = record_trans.now;
    account.now_ = utime;
    account.block_lt = record_trans.lt - record_trans.lt % block::ConfigInfo::get_lt_align();
    td::Ref<vm::Cell> msg_root = record_trans.r1.in_msg->prefetch_ref();
    int tag = block::gen::t_TransactionDescr.get_tag(vm::load_cell_slice(record_trans.description));

    int trans_type = block::transaction::Transaction::tr_none;
    switch (tag) {
      case block::gen::TransactionDescr::trans_ord: {
        trans_type = block::transaction::Transaction::tr_ord;
        break;
      }
      case block::gen::TransactionDescr::trans_storage: {
        trans_type = block::transaction::Transaction::tr_storage;
        break;
      }
      case block::gen::TransactionDescr::trans_tick_tock: {
        block::gen::TransactionDescr::Record_trans_tick_tock tick_tock;
        if (!tlb::unpack_cell(record_trans.description, tick_tock)) {
          return td::Status::Error("Failed to unpack tick tock transaction description");
        }
        trans_type = tick_tock.is_tock ? block::transaction::Transaction::tr_tock : block::transaction::Transaction::tr_tick;
        break;
      }
      case block::gen::TransactionDescr::trans_split_prepare: {
        trans_type = block::transaction::Transaction::tr_split_prepare;
        break;
      }
      case block::gen::TransactionDescr::trans_split_install: {
        trans_type = block::transaction::Transaction::tr_split_install;
        break;
      }
      case block::gen::TransactionDescr::trans_merge_prepare: {
        trans_type = block::transaction::Transaction::tr_merge_prepare;
        break;
      }
      case block::gen::TransactionDescr::trans_merge_install: {
        trans_type = block::transaction::Transaction::tr_merge_install;
        break;
      }
    }

    TRY_RESULT(emulation, emulate_transaction(std::move(account), msg_root, utime, lt, trans_type));
    
    if (auto emulation_result_ptr = dynamic_cast<EmulationSuccess*>(emulation.get())) {
      auto& emulation_result = *emulation_result_ptr;     
    
      if (td::Bits256(emulation_result.transaction->get_hash().bits()) != td::Bits256(original_trans->get_hash().bits())) {
        return td::Status::Error("transaction hash mismatch");
      }

      if (!check_state_update(emulation_result.account, record_trans)) {
        return td::Status::Error("account hash mismatch");
      }

      return std::move(emulation_result);

    } else if (auto emulation_not_accepted_ptr = dynamic_cast<EmulationExternalNotAccepted*>(emulation.get())) {
      return td::Status::Error( PSTRING()
        << "VM Log: " << emulation_not_accepted_ptr->vm_log 
        << ", VM Exit Code: " << emulation_not_accepted_ptr->vm_exit_code 
        << ", Elapsed Time: " << emulation_not_accepted_ptr->elapsed_time);
    } else {
       return td::Status::Error("emulation failed");
    }
}

td::Result<TransactionEmulator::EmulationChain> TransactionEmulator::emulate_transactions_chain(block::Account&& account, std::vector<td::Ref<vm::Cell>>&& original_transactions) {

  std::vector<td::Ref<vm::Cell>> emulated_transactions;
  for (const auto& original_trans : original_transactions) {
    if (original_trans.is_null()) {
      continue;
    }

    TRY_RESULT(emulation_result, emulate_transaction(std::move(account), original_trans));
    emulated_transactions.push_back(std::move(emulation_result.transaction));
    account = std::move(emulation_result.account);
  }

  return TransactionEmulator::EmulationChain{ std::move(emulated_transactions), std::move(account) };
}

bool TransactionEmulator::check_state_update(const block::Account& account, const block::gen::Transaction::Record& trans) {
  block::gen::HASH_UPDATE::Record hash_update;
  return tlb::type_unpack_cell(trans.state_update, block::gen::t_HASH_UPDATE_Account, hash_update) &&
    hash_update.new_hash == account.total_state->get_hash().bits();
}

td::Result<std::unique_ptr<block::transaction::Transaction>> TransactionEmulator::create_transaction(
                                                         td::Ref<vm::Cell> msg_root, block::Account* acc,
                                                         ton::UnixTime utime, ton::LogicalTime lt, int trans_type,
                                                         block::StoragePhaseConfig* storage_phase_cfg,
                                                         block::ComputePhaseConfig* compute_phase_cfg,
                                                         block::ActionPhaseConfig* action_phase_cfg) {
  bool external{false}, ihr_delivered{false}, need_credit_phase{false};

  if (msg_root.not_null()) {
    auto cs = vm::load_cell_slice(msg_root);
    external = block::gen::t_CommonMsgInfo.get_tag(cs);
  }

  if (trans_type == block::transaction::Transaction::tr_ord) {
    need_credit_phase = !external;
  } else if (trans_type == block::transaction::Transaction::tr_merge_install) {
    need_credit_phase = true;
  }

  std::unique_ptr<block::transaction::Transaction> trans =
      std::make_unique<block::transaction::Transaction>(*acc, trans_type, lt, utime, msg_root);

  if (msg_root.not_null() && !trans->unpack_input_msg(ihr_delivered, action_phase_cfg)) {
    if (external) {
      // inbound external message was not accepted
      return td::Status::Error(-701,"inbound external message rejected by account "s + acc->addr.to_hex() +
                                                           " before smart-contract execution");
    }
    return td::Status::Error(-669,"cannot unpack input message for a new transaction");
  }

  if (trans->bounce_enabled) {
    if (!trans->prepare_storage_phase(*storage_phase_cfg, true)) {
      return td::Status::Error(-669,"cannot create storage phase of a new transaction for smart contract "s + acc->addr.to_hex());
    }
    if (need_credit_phase && !trans->prepare_credit_phase()) {
      return td::Status::Error(-669,"cannot create credit phase of a new transaction for smart contract "s + acc->addr.to_hex());
    }
  } else {
    if (need_credit_phase && !trans->prepare_credit_phase()) {
      return td::Status::Error(-669,"cannot create credit phase of a new transaction for smart contract "s + acc->addr.to_hex());
    }
    if (!trans->prepare_storage_phase(*storage_phase_cfg, true, need_credit_phase)) {
      return td::Status::Error(-669,"cannot create storage phase of a new transaction for smart contract "s + acc->addr.to_hex());
    }
  }

  if (!trans->prepare_compute_phase(*compute_phase_cfg)) {
    return td::Status::Error(-669,"cannot create compute phase of a new transaction for smart contract "s + acc->addr.to_hex());
  }

  if (!trans->compute_phase->accepted) {
    if (!external && trans->compute_phase->skip_reason == block::ComputePhase::sk_none) {
      return td::Status::Error(-669,"new ordinary transaction for smart contract "s + acc->addr.to_hex() +
                " has not been accepted by the smart contract (?)");
    }
  }

  if (trans->compute_phase->success && !trans->prepare_action_phase(*action_phase_cfg)) {
    return td::Status::Error(-669,"cannot create action phase of a new transaction for smart contract "s + acc->addr.to_hex());
  }

  if (trans->bounce_enabled
  && (!trans->compute_phase->success || trans->action_phase->state_exceeds_limits || trans->action_phase->bounce)
  && !trans->prepare_bounce_phase(*action_phase_cfg)) {
    return td::Status::Error(-669,"cannot create bounce phase of a new transaction for smart contract "s + acc->addr.to_hex());
  }

  return trans;
}

void TransactionEmulator::set_unixtime(ton::UnixTime unixtime) {
  unixtime_ = unixtime;
}

void TransactionEmulator::set_lt(ton::LogicalTime lt) {
  lt_ = lt;
}

void TransactionEmulator::set_rand_seed(td::BitArray<256>& rand_seed) {
  rand_seed_ = rand_seed;
}

void TransactionEmulator::set_ignore_chksig(bool ignore_chksig) {
  ignore_chksig_ = ignore_chksig;
}

void TransactionEmulator::set_config(std::shared_ptr<block::Config> config) {
  config_ = std::move(config);
}

void TransactionEmulator::set_libs(vm::Dictionary &&libs) {
  libraries_ = std::forward<vm::Dictionary>(libs);
}

void TransactionEmulator::set_debug_enabled(bool debug_enabled) {
  debug_enabled_ = debug_enabled;
}

void TransactionEmulator::set_prev_blocks_info(td::Ref<vm::Tuple> prev_blocks_info) {
  prev_blocks_info_ = std::move(prev_blocks_info);
}

} // namespace emulator
