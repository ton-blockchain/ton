#include <string>
#include "transaction-emulator.h"
#include "crypto/common/refcnt.hpp"
#include "vm/cp0.h"

using td::Ref;
using namespace std::string_literals;

namespace emulator {
td::Result<TransactionEmulator::EmulationResult> TransactionEmulator::emulate_transaction(
    block::Account&& account, td::Ref<vm::Cell> msg_root,
    ton::UnixTime utime, ton::LogicalTime lt, int trans_type, td::BitArray<256> rand_seed) {

    td::Ref<vm::Cell> old_mparams;
    std::vector<block::StoragePrices> storage_prices;
    block::StoragePhaseConfig storage_phase_cfg{&storage_prices};
    block::ComputePhaseConfig compute_phase_cfg;
    block::ActionPhaseConfig action_phase_cfg;
    td::RefInt256 masterchain_create_fee, basechain_create_fee;
    
    auto fetch_res = block::FetchConfigParams::fetch_config_params(config_, &old_mparams,
                                                                   &storage_prices, &storage_phase_cfg,
                                                                   &rand_seed, &compute_phase_cfg,
                                                                   &action_phase_cfg, &masterchain_create_fee,
                                                                   &basechain_create_fee, account.workchain);
    if(fetch_res.is_error()) {
        return fetch_res.move_as_error_prefix("cannot fetch config params ");
    }
    compute_phase_cfg.libraries = std::make_unique<vm::Dictionary>(libraries_);
    compute_phase_cfg.ignore_chksig = ignore_chksig_;

    vm::init_op_cp0();

    if (!utime) {
      utime = (unsigned)std::time(nullptr);
    }
    if (!lt) {
      lt = (account.last_trans_lt_ / block::ConfigInfo::get_lt_align() + 1) * block::ConfigInfo::get_lt_align(); // next block after account_.last_trans_lt_
    }

    auto res = create_transaction(msg_root, &account, utime, lt, trans_type,
                                                    &storage_phase_cfg, &compute_phase_cfg,
                                                    &action_phase_cfg);
    if(res.is_error()) {
        return res.move_as_error_prefix("cannot run message on account ");
    }
    std::unique_ptr<block::transaction::Transaction> trans = res.move_as_ok();

    auto trans_root = trans->commit(account);
    if (trans_root.is_null()) {
        return td::Status::Error(PSLICE() << "cannot commit new transaction for smart contract");
    }

    return TransactionEmulator::EmulationResult{ std::move(trans_root), std::move(account) };
}

td::Result<TransactionEmulator::EmulationResult> TransactionEmulator::emulate_transaction(block::Account&& account, td::Ref<vm::Cell> original_trans, td::BitArray<256> rand_seed) {

    block::gen::Transaction::Record record_trans;
    if (!tlb::unpack_cell(original_trans, record_trans)) {
      return td::Status::Error("Failed to unpack Transaction");
    }

    ton::LogicalTime lt = record_trans.lt;
    ton::UnixTime utime = record_trans.now;
    account.now_ = utime;
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

    TRY_RESULT(emulation_result, emulate_transaction(std::move(account), msg_root, utime, lt, trans_type, rand_seed));

    if (td::Bits256(emulation_result.transaction->get_hash().bits()) != td::Bits256(original_trans->get_hash().bits())) {
      return td::Status::Error("transaction hash mismatch");
    }

    if (!check_state_update(emulation_result.account, record_trans)) {
      return td::Status::Error("account hash mismatch");
    }

    return emulation_result;
}

td::Result<TransactionEmulator::EmulationResults> TransactionEmulator::emulate_transactions(block::Account&& account, std::vector<td::Ref<vm::Cell>>&& original_transactions, td::BitArray<256> rand_seed) {

  std::vector<td::Ref<vm::Cell>> emulated_transactions;
  for (const auto& original_trans : original_transactions) {
    if (original_trans.is_null()) {
      continue;
    }

    TRY_RESULT(emulation_result, emulate_transaction(std::move(account), original_trans, rand_seed));
    emulated_transactions.push_back(std::move(emulation_result.transaction));
    account = std::move(emulation_result.account);
  }

  return TransactionEmulator::EmulationResults{ std::move(emulated_transactions), std::move(account) };
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
    if (external) {
      // inbound external message was not accepted
      auto const& cp = *trans->compute_phase;
      return td::Status::Error(
          -701,
          PSLICE() << "inbound external message rejected by transaction " << acc->addr.to_hex() << ":\n" <<
              "exitcode=" << cp.exit_code << ", steps=" << cp.vm_steps << ", gas_used=" << cp.gas_used <<
              (cp.vm_log.empty() ? "" : "\nVM Log (truncated):\n..." + cp.vm_log));
    } else if (trans->compute_phase->skip_reason == block::ComputePhase::sk_none) {
      return td::Status::Error(-669,"new ordinary transaction for smart contract "s + acc->addr.to_hex() +
                " has not been accepted by the smart contract (?)");
    }
  }

  if (trans->compute_phase->success && !trans->prepare_action_phase(*action_phase_cfg)) {
    return td::Status::Error(-669,"cannot create action phase of a new transaction for smart contract "s + acc->addr.to_hex());
  }

  if (trans->bounce_enabled && !trans->compute_phase->success && !trans->prepare_bounce_phase(*action_phase_cfg)) {
    return td::Status::Error(-669,"cannot create bounce phase of a new transaction for smart contract "s + acc->addr.to_hex());
  }

  if (!trans->serialize()) {
    return td::Status::Error(-669,"cannot serialize new transaction for smart contract "s + acc->addr.to_hex());
  }

  return trans;
}
} // namespace emulator
