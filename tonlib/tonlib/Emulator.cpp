#include "Emulator.h"
#include "block/transaction.h"
#include "vm/cp0.h"

using td::Ref;
using namespace std::string_literals;

namespace emulator {
// as in collator::impl_fetch_config_params but using block::Config instead block::ConfigInfo
td::Status fetch_config_params(const vm::Dictionary& libraries,
                               const block::Config& config,
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
    compute_phase_cfg->libraries = std::make_unique<vm::Dictionary>(libraries);
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

td::Result<std::unique_ptr<block::transaction::Transaction>> create_transaction(
    block::gen::Transaction::Record &record_trans,
    block::Account* acc,
    block::StoragePhaseConfig* storage_phase_cfg,
    block::ComputePhaseConfig* compute_phase_cfg,
    block::ActionPhaseConfig* action_phase_cfg) {

  auto lt = record_trans.lt;
  auto now = record_trans.now;
  acc->now_ = now;
  td::Ref<vm::Cell> msg_root = record_trans.r1.in_msg->prefetch_ref();
  bool external{false}, ihr_delivered{false}, need_credit_phase{false};
  int tag = block::gen::t_TransactionDescr.get_tag(vm::load_cell_slice(record_trans.description));

  if (msg_root.not_null()) {
    auto cs = vm::load_cell_slice(msg_root);
    external = block::gen::t_CommonMsgInfo.get_tag(cs);
  }

  int trans_type = block::transaction::Transaction::tr_none;
  switch (tag) {
    case block::gen::TransactionDescr::trans_ord: {
      trans_type = block::transaction::Transaction::tr_ord;
      need_credit_phase = !external;
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
      need_credit_phase = true;
      break;
    }
  }

  std::unique_ptr<block::transaction::Transaction> trans =
    std::make_unique<block::transaction::Transaction>(*acc, trans_type, lt, now, msg_root);

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

bool check_state_update(const block::Account& account, const block::gen::Transaction::Record& trans) {
  block::gen::HASH_UPDATE::Record hash_update;
  return tlb::type_unpack_cell(trans.state_update, block::gen::t_HASH_UPDATE_Account, hash_update) &&
    hash_update.new_hash == account.total_state->get_hash().bits();
}

td::Status emulate_transactions(td::Ref<vm::Cell>&& mc_state_root, block::StdAddress address, ton::UnixTime now,
                                td::Ref<vm::CellSlice>&& shard_account_cell_slice, ton::Bits256&& rand_seed,
                                std::vector<td::Ref<vm::Cell>>&& transactions,
                                td::int64& balance, ton::UnixTime& storage_last_paid, vm::CellStorageStat& storage_stat,
                                td::Ref<vm::Cell>& code, td::Ref<vm::Cell>& data, td::Ref<vm::Cell>& state,
                                std::string& frozen_hash, ton::LogicalTime& last_trans_lt, ton::Bits256& last_trans_hash, td::uint32& gen_utime) {
  TRY_RESULT(config, block::Config::extract_from_state(mc_state_root, 0b11'11111111));

  block::gen::ShardStateUnsplit::Record shard_state;
  if (!tlb::unpack_cell(mc_state_root, shard_state)) {
    return td::Status::Error("Failed to unpack masterchain state");
  }
  vm::Dictionary libraries(shard_state.r1.libraries->prefetch_ref(), 256);

  auto account = block::Account(address.workchain, address.addr.bits());
  bool is_special = address.workchain == ton::masterchainId && config->is_special_smartcontract(address.addr);
  if (!account.unpack(std::move(shard_account_cell_slice), td::Ref<vm::CellSlice>(), now, is_special)) {
    return td::Status::Error("Can't unpack shard account");
  }

  td::Ref<vm::Cell> old_mparams;
  std::vector<block::StoragePrices> storage_prices;
  block::StoragePhaseConfig storage_phase_cfg{&storage_prices};
  block::ComputePhaseConfig compute_phase_cfg;
  block::ActionPhaseConfig action_phase_cfg;
  td::RefInt256 masterchain_create_fee, basechain_create_fee;

  TRY_STATUS_PREFIX(fetch_config_params(libraries, *config, &old_mparams,
                                        &storage_prices, &storage_phase_cfg,
                                        &rand_seed, &compute_phase_cfg,
                                        &action_phase_cfg, &masterchain_create_fee,
                                        &basechain_create_fee, account.workchain), "cannot fetch config params ");

  vm::init_op_cp0();

  for (const auto& original_trans : transactions) {
    if (original_trans.is_null()) {
      continue;
    }

    block::gen::Transaction::Record record_trans;
    if (!tlb::unpack_cell(original_trans, record_trans)) {
      return td::Status::Error("Failed to unpack Transaction");
    }

    TRY_RESULT(emulated_trans, create_transaction(record_trans, &account, &storage_phase_cfg, &compute_phase_cfg, &action_phase_cfg));

    if (td::Bits256(emulated_trans->root->get_hash().bits()) != td::Bits256(original_trans->get_hash().bits())) {
      return td::Status::Error("transaction hash mismatch");
    }

    auto trans_root = emulated_trans->commit(account);
    if (trans_root.is_null()) {
      return td::Status::Error("cannot commit new transaction for smart contract");
    }

    if (!check_state_update(account, record_trans)) {
      return td::Status::Error("account hash mismatch");
    }
  }

  balance = account.get_balance().grams->to_long();
  storage_last_paid = std::move(account.last_paid);
  storage_stat = std::move(account.storage_stat);
  code = std::move(account.code);
  data = std::move(account.data);
  state = std::move(account.total_state);
  last_trans_lt = account.last_trans_lt_;
  last_trans_hash = account.last_trans_hash_;
  gen_utime = account.now_;

  if (account.status == block::Account::acc_frozen) {
    frozen_hash = (char*)account.state_hash.data();
  }

  return td::Status::OK();
}
}  // namespace emulator
