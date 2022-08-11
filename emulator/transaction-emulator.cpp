#include <string>
#include "transaction-emulator.h"
#include "crypto/common/refcnt.hpp"
#include "crypto/openssl/rand.hpp"
#include "vm/cp0.h"

using td::Ref;
using namespace std::string_literals;

namespace emulator {
td::Result<td::Ref<vm::Cell>> TransactionEmulator::emulate_transaction(td::Ref<vm::Cell> msg_root) {
    auto cs = vm::load_cell_slice(msg_root);
    bool external;
    Ref<vm::CellSlice> src, dest;
    switch (block::gen::t_CommonMsgInfo.get_tag(cs)) {
        case block::gen::CommonMsgInfo::ext_in_msg_info: {
            block::gen::CommonMsgInfo::Record_ext_in_msg_info info;
            if (!tlb::unpack(cs, info)) {
                return td::Status::Error("cannot unpack inbound external message");
            }
            dest = std::move(info.dest);
            external = true;
            break;
        }
        case block::gen::CommonMsgInfo::int_msg_info: {
            block::gen::CommonMsgInfo::Record_int_msg_info info;
            if (!tlb::unpack(cs, info)) {
                return td::Status::Error("cannot unpack internal message to be processed by an ordinary transaction");
            }
            src = std::move(info.src);
            dest = std::move(info.dest);
            external = false;
            break;
        }
        default:
            return td::Status::Error("only ext in and int message are supported");
    }

    ton::WorkchainId wc;
    ton::StdSmcAddress addr;
    if (!block::tlb::t_MsgAddressInt.extract_std_address(dest, wc, addr)) {
        return td::Status::Error("cannot extract message address");
    }

    td::Ref<vm::Cell> old_mparams;
    std::vector<block::StoragePrices> storage_prices_;
    block::StoragePhaseConfig storage_phase_cfg_{&storage_prices_};
    td::BitArray<256> rand_seed_;
    block::ComputePhaseConfig compute_phase_cfg_;
    block::ActionPhaseConfig action_phase_cfg_;
    td::RefInt256 masterchain_create_fee, basechain_create_fee;
    
    auto fetch_res = fetch_config_params(config_, &old_mparams,
                                        &storage_prices_, &storage_phase_cfg_,
                                        &rand_seed_, &compute_phase_cfg_,
                                        &action_phase_cfg_, &masterchain_create_fee,
                                        &basechain_create_fee, wc);
    if(fetch_res.is_error()) {
        return fetch_res.move_as_error_prefix("cannot fetch config params ");
    }

    compute_phase_cfg_.ignore_chksig = external;

    vm::init_op_cp0();

    ton::UnixTime utime = (unsigned)std::time(nullptr);
    ton::LogicalTime lt = account_.last_trans_lt_ + block::ConfigInfo::get_lt_align();
    auto res = create_ordinary_transaction(msg_root, &account_, utime, lt,
                                                    &storage_phase_cfg_, &compute_phase_cfg_,
                                                    &action_phase_cfg_,
                                                    external, lt);
    if(res.is_error()) {
        return res.move_as_error_prefix("cannot run message on account ");
    }
    std::unique_ptr<block::Transaction> trans = res.move_as_ok();

    auto trans_root = trans->commit(account_);
    if (trans_root.is_null()) {
        return td::Status::Error(PSLICE() << "cannot commit new transaction for smart contract");
    }
    return trans_root;
}

// as in collator::impl_fetch_config_params but using block::Config instead block::ConfigInfo
td::Status TransactionEmulator::fetch_config_params(const block::Config& config,
                                              Ref<vm::Cell>* old_mparams,
                                              std::vector<block::StoragePrices>* storage_prices,
                                              block::StoragePhaseConfig* storage_phase_cfg,
                                              td::BitArray<256>* rand_seed,
                                              block::ComputePhaseConfig* compute_phase_cfg,
                                              block::ActionPhaseConfig* action_phase_cfg,
                                              td::RefInt256* masterchain_create_fee,
                                              td::RefInt256* basechain_create_fee,
                                              ton::WorkchainId wc) {
  *old_mparams = config.get_config_param(9);
  {
    auto res = config.get_storage_prices();
    if (res.is_error()) {
      return res.move_as_error();
    }
    *storage_prices = res.move_as_ok();
  }
  {
    // generate rand seed
    prng::rand_gen().strong_rand_bytes(rand_seed->data(), 32);
    LOG(DEBUG) << "block random seed set to " << rand_seed->to_hex();
  }
  {
    // compute compute_phase_cfg / storage_phase_cfg
    auto cell = config.get_config_param(wc == ton::masterchainId ? 20 : 21);
    if (cell.is_null()) {
      return td::Status::Error(-668, "cannot fetch current gas prices and limits from masterchain configuration");
    }
    if (!compute_phase_cfg->parse_GasLimitsPrices(std::move(cell), storage_phase_cfg->freeze_due_limit,
                                                  storage_phase_cfg->delete_due_limit)) {
      return td::Status::Error(-668, "cannot unpack current gas prices and limits from masterchain configuration");
    }
    compute_phase_cfg->block_rand_seed = *rand_seed;
    compute_phase_cfg->libraries = std::make_unique<vm::Dictionary>(libraries_);
    compute_phase_cfg->global_config = config.get_root_cell();
  }
  {
    // compute action_phase_cfg
    block::gen::MsgForwardPrices::Record rec;
    auto cell = config.get_config_param(24);
    if (cell.is_null() || !tlb::unpack_cell(std::move(cell), rec)) {
      return td::Status::Error(-668, "cannot fetch masterchain message transfer prices from masterchain configuration");
    }
    action_phase_cfg->fwd_mc =
        block::MsgPrices{rec.lump_price,           rec.bit_price,          rec.cell_price, rec.ihr_price_factor,
                         (unsigned)rec.first_frac, (unsigned)rec.next_frac};
    cell = config.get_config_param(25);
    if (cell.is_null() || !tlb::unpack_cell(std::move(cell), rec)) {
      return td::Status::Error(-668, "cannot fetch standard message transfer prices from masterchain configuration");
    }
    action_phase_cfg->fwd_std =
        block::MsgPrices{rec.lump_price,           rec.bit_price,          rec.cell_price, rec.ihr_price_factor,
                         (unsigned)rec.first_frac, (unsigned)rec.next_frac};
    action_phase_cfg->workchains = &config.get_workchain_list();
    action_phase_cfg->bounce_msg_body = (config.has_capability(ton::capBounceMsgBody) ? 256 : 0);
  }
  {
    // fetch block_grams_created
    auto cell = config.get_config_param(14);
    if (cell.is_null()) {
      *basechain_create_fee = *masterchain_create_fee = td::zero_refint();
    } else {
      block::gen::BlockCreateFees::Record create_fees;
      if (!(tlb::unpack_cell(cell, create_fees) &&
            block::tlb::t_Grams.as_integer_to(create_fees.masterchain_block_fee, *masterchain_create_fee) &&
            block::tlb::t_Grams.as_integer_to(create_fees.basechain_block_fee, *basechain_create_fee))) {
        return td::Status::Error(-668, "cannot unpack BlockCreateFees from configuration parameter #14");
      }
    }
  }
  return td::Status::OK();
}

td::Result<std::unique_ptr<block::Transaction>> TransactionEmulator::create_ordinary_transaction(Ref<vm::Cell> msg_root,
                                                         block::Account* acc,
                                                         ton::UnixTime utime, ton::LogicalTime lt,
                                                         block::StoragePhaseConfig* storage_phase_cfg,
                                                         block::ComputePhaseConfig* compute_phase_cfg,
                                                         block::ActionPhaseConfig* action_phase_cfg,
                                                         bool external, ton::LogicalTime after_lt) {
  if (acc->last_trans_end_lt_ >= lt && acc->transactions.empty()) {
    return td::Status::Error(-669, PSTRING() << "last transaction time in the state of account " << acc->workchain << ":" << acc->addr.to_hex()
                          << " is too large");
  }
  auto trans_min_lt = lt;
  if (external) {
    // transactions processing external messages must have lt larger than all processed internal messages
    trans_min_lt = std::max(trans_min_lt, after_lt);
  }

  std::unique_ptr<block::Transaction> trans =
      std::make_unique<block::Transaction>(*acc, block::Transaction::tr_ord, trans_min_lt + 1, utime, msg_root);
  bool ihr_delivered = false;  // FIXME
  if (!trans->unpack_input_msg(ihr_delivered, action_phase_cfg)) {
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
    if (!external && !trans->prepare_credit_phase()) {
      return td::Status::Error(-669,"cannot create credit phase of a new transaction for smart contract "s + acc->addr.to_hex());
      }
  } else {
    if (!external && !trans->prepare_credit_phase()) {
      return td::Status::Error(-669,"cannot create credit phase of a new transaction for smart contract "s + acc->addr.to_hex());
      }
    if (!trans->prepare_storage_phase(*storage_phase_cfg, true, true)) {
      return td::Status::Error(-669,"cannot create storage phase of a new transaction for smart contract "s + acc->addr.to_hex());
      }
  }
  if (!trans->prepare_compute_phase(*compute_phase_cfg)) {
    return td::Status::Error(-669,"cannot create compute phase of a new transaction for smart contract "s + acc->addr.to_hex());
  }
  if (!trans->compute_phase->accepted) {
    if (external) {
      // inbound external message was not accepted
        return td::Status::Error(-701,"inbound external message rejected by transaction "s + acc->addr.to_hex());
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
  return std::move(trans);
}
} // namespace emulator