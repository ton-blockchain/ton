#include <algorithm>
#include <limits>
#include <utility>

#include "benchmark/collation/fixture.h"
#include "block/block-auto.h"
#include "block/block-db.h"
#include "block/block-parse.h"
#include "block/mc-config.h"
#include "td/utils/crypto.h"
#include "td/utils/filesystem.h"
#include "validator/fabric.h"
#include "validator/impl/external-message.hpp"
#include "vm/boc.h"
#include "vm/cells/CellBuilder.h"
#include "vm/cells/CellSlice.h"
#include "vm/cells/MerkleUpdate.h"
#include "vm/dict.h"

namespace bench::collation {
namespace {

constexpr td::uint32 kWalletId = 0;

td::Bits256 make_seed(td::uint64 seed) {
  unsigned char input[32]{};
  constexpr char kPrefix[] = "ton-collation-jetton";
  static_assert(sizeof(kPrefix) - 1 <= 24);
  std::copy(kPrefix, kPrefix + sizeof(kPrefix) - 1, input);
  for (int i = 0; i < 8; ++i) {
    input[24 + i] = static_cast<unsigned char>(seed >> (8 * i));
  }
  return td::sha256_bits256(td::Slice{input, sizeof(input)});
}

td::Result<Uint128> checked_add(Uint128 lhs, Uint128 rhs, td::Slice field) {
  constexpr Uint128 kMax = ~static_cast<Uint128>(0);
  if (rhs > kMax - lhs) {
    return td::Status::Error(PSLICE() << field << " overflows uint128");
  }
  return lhs + rhs;
}

td::Result<Uint128> checked_mul(Uint128 lhs, Uint128 rhs, td::Slice field) {
  constexpr Uint128 kMax = ~static_cast<Uint128>(0);
  if (lhs != 0 && rhs > kMax / lhs) {
    return td::Status::Error(PSLICE() << field << " overflows uint128");
  }
  return lhs * rhs;
}

td::RefInt256 to_refint(Uint128 value) {
  return td::dec_string_to_int256(u128_to_dec(value));
}

td::Status require_zero_balance(const td::Ref<vm::CellSlice>& packed, td::Slice field) {
  block::CurrencyCollection balance;
  if (!balance.validate_unpack(packed) || !balance.is_zero()) {
    return td::Status::Error(PSLICE() << field << " must be an empty zero balance");
  }
  return td::Status::OK();
}

td::Status set_grams(td::Ref<vm::CellSlice>& packed, Uint128 amount, td::Slice field) {
  block::CurrencyCollection balance{to_refint(amount)};
  if (!balance.pack_to(packed)) {
    return td::Status::Error(PSLICE() << "cannot pack " << field);
  }
  return td::Status::OK();
}

td::Status add_grams(td::Ref<vm::CellSlice>& packed, Uint128 amount, td::Slice field) {
  block::CurrencyCollection balance;
  if (!balance.validate_unpack(packed) || balance.has_extra()) {
    return td::Status::Error(PSLICE() << "cannot unpack plain " << field);
  }
  balance += to_refint(amount);
  if (!balance.pack_to(packed)) {
    return td::Status::Error(PSLICE() << "cannot repack " << field);
  }
  return td::Status::OK();
}

td::Result<ton::BlockIdExt> make_block_id(ton::WorkchainId workchain, ton::ShardId shard, ton::BlockSeqno seqno,
                                          const td::Ref<vm::Cell>& root) {
  TRY_RESULT(boc, vm::std_boc_serialize(root, 31));
  auto file_hash = block::compute_file_hash(boc);
  return ton::BlockIdExt{workchain, shard, seqno, ton::RootHash{root->get_hash().bits()}, file_hash};
}

td::Status add_account(vm::AugmentedDictionary& accounts, const td::Bits256& address, const td::Ref<vm::Cell>& account,
                       td::Slice description) {
  if (!block::gen::t_Account.validate_ref(account)) {
    return td::Status::Error(PSLICE() << description << " fails Account TLB validation");
  }
  vm::CellBuilder leaf;
  if (!(leaf.store_ref_bool(account) && leaf.store_zeroes_bool(256 + 64)) ||
      !accounts.set_builder(address.cbits(), 256, leaf, vm::Dictionary::SetMode::Add)) {
    return td::Status::Error(PSLICE() << "cannot insert " << description << " (address collision?)");
  }
  return td::Status::OK();
}

td::Result<td::Ref<vm::Cell>> repack_accounts(block::gen::ShardStateUnsplit::Record& state,
                                              vm::AugmentedDictionary& accounts, Uint128 total_balance) {
  vm::CellBuilder accounts_builder;
  if (!accounts.append_dict_to_bool(accounts_builder)) {
    return td::Status::Error("cannot repack ShardAccounts dictionary");
  }
  state.accounts = accounts_builder.finalize();
  TRY_STATUS(set_grams(state.r1.total_balance, total_balance, "ShardState.total_balance"));

  td::Ref<vm::Cell> root;
  if (!tlb::pack_cell(root, state) ||
      !block::gen::t_ShardStateUnsplit.validate_ref(std::numeric_limits<int>::max(), root)) {
    return td::Status::Error("cannot repack valid basechain state");
  }
  return root;
}

td::Result<std::pair<td::uint64, td::uint64>> compute_storage_used(const td::Ref<vm::Cell>& storage) {
  if (storage.is_null()) {
    return td::Status::Error("cannot compute storage usage for a null AccountStorage cell");
  }
  vm::CellStorageStat stat;
  TRY_RESULT(stat_info, stat.compute_used_storage(storage));
  static_cast<void>(stat_info);
  return std::pair<td::uint64, td::uint64>{static_cast<td::uint64>(stat.cells), static_cast<td::uint64>(stat.bits)};
}

td::Result<std::pair<td::uint64, td::uint64>> unpack_storage_used(const td::Ref<vm::CellSlice>& packed) {
  block::gen::StorageUsed::Record used;
  if (!tlb::csr_unpack(packed, used)) {
    return td::Status::Error("cannot unpack configuration account StorageUsed");
  }
  const auto cells = block::tlb::t_VarUInteger_7.as_uint(*used.cells);
  const auto bits = block::tlb::t_VarUInteger_7.as_uint(*used.bits);
  if (cells == std::numeric_limits<td::uint64>::max() || bits == std::numeric_limits<td::uint64>::max()) {
    return td::Status::Error("configuration account StorageUsed is not a canonical uint64 pair");
  }
  return std::pair<td::uint64, td::uint64>{static_cast<td::uint64>(cells), static_cast<td::uint64>(bits)};
}

td::Status require_global_id_config(const td::Ref<vm::Cell>& config_root, int global_id, td::Slice description) {
  if (config_root.is_null()) {
    return td::Status::Error(PSLICE() << description << " has a null configuration dictionary");
  }
  vm::Dictionary config{config_root, 32};
  auto param = config.lookup_ref(td::BitArray<32>{19});
  int stored_global_id = 0;
  if (param.is_null() || !block::gen::ConfigParam{19}.cell_unpack_cons19(param, stored_global_id) ||
      stored_global_id != global_id) {
    return td::Status::Error(PSLICE() << description << " does not contain ConfigParam 19 global_id=" << global_id);
  }
  return td::Status::OK();
}

td::Result<td::Ref<vm::Cell>> config_from_state_extra(const block::gen::McStateExtra::Record& extra,
                                                      td::BitArray<256>* config_addr = nullptr) {
  block::gen::ConfigParams::Record params;
  if (!tlb::csr_unpack(extra.config, params)) {
    return td::Status::Error("cannot unpack masterchain ConfigParams");
  }
  if (config_addr != nullptr) {
    *config_addr = params.config_addr;
  }
  return params.config;
}

td::Result<td::Ref<vm::Cell>> config_from_smc(const block::gen::ShardStateUnsplit::Record& state,
                                              const td::BitArray<256>& config_addr) {
  try {
    vm::AugmentedDictionary accounts{vm::load_cell_slice_ref(state.accounts), 256, block::tlb::aug_ShardAccounts};
    if (!accounts.is_valid()) {
      return td::Status::Error("masterchain account dictionary is invalid");
    }
    return block::get_config_data_from_smc(accounts.lookup(config_addr));
  } catch (vm::VmError& error) {
    return error.as_status("cannot read configuration smart-contract account: ");
  } catch (vm::VmVirtError& error) {
    return error.as_status("cannot read virtualized configuration smart-contract account: ");
  }
}

// Stage ConfigParam 19 as a real pending configuration-contract change.  The
// effective MC0 McStateExtra deliberately keeps the old root: the untimed MC1
// bootstrap observes the changed contract data, emits a key block, rotates
// catchain/shard groups, and installs this root into its McStateExtra.
td::Status stage_global_id_config_change_impl(block::gen::ShardStateUnsplit::Record& state,
                                              const block::gen::McStateExtra::Record& extra) {
  td::BitArray<256> config_addr;
  TRY_RESULT(effective_config, config_from_state_extra(extra, &config_addr));
  vm::Dictionary config{effective_config, 32};
  if (config.lookup_ref(td::BitArray<32>{19}).not_null()) {
    return td::Status::Error("fixture masterchain state unexpectedly already has ConfigParam 19");
  }

  vm::AugmentedDictionary accounts{vm::load_cell_slice_ref(state.accounts), 256, block::tlb::aug_ShardAccounts};
  if (!accounts.is_valid()) {
    return td::Status::Error("masterchain account dictionary is invalid");
  }
  auto config_account_value = accounts.lookup(config_addr);
  if (config_account_value.is_null()) {
    return td::Status::Error("configuration smart-contract account is absent");
  }
  TRY_RESULT(contract_config, block::get_config_data_from_smc(config_account_value));
  if (contract_config->get_hash() != effective_config->get_hash()) {
    return td::Status::Error("fixture starts with divergent effective and configuration-contract roots");
  }

  td::Ref<vm::Cell> param19;
  if (!block::gen::ConfigParam{19}.cell_pack_cons19(param19, state.global_id) ||
      !block::gen::ConfigParam{19}.validate_ref(param19) ||
      !config.set_ref(td::BitArray<32>{19}, param19, vm::Dictionary::SetMode::Add)) {
    return td::Status::Error("cannot insert ConfigParam 19 into configuration dictionary");
  }
  auto pending_config = config.get_root_cell();
  if (!block::valid_config_data(pending_config, config_addr, true, true)) {
    return td::Status::Error("configuration dictionary with ConfigParam 19 is invalid");
  }
  TRY_STATUS(require_global_id_config(pending_config, state.global_id, "pending configuration"));

  block::gen::ShardAccount::Record shard_account;
  block::gen::Account::Record_account account;
  block::gen::StorageInfo::Record storage_info;
  block::gen::AccountStorage::Record storage;
  if (!tlb::csr_unpack(config_account_value, shard_account) || !tlb::unpack_cell(shard_account.account, account) ||
      !tlb::csr_unpack(account.storage_stat, storage_info) || !tlb::csr_unpack(account.storage, storage)) {
    return td::Status::Error("cannot unpack configuration smart-contract account");
  }
  if (storage_info.storage_extra.is_null() || storage_info.storage_extra->prefetch_ulong(3) != 0) {
    return td::Status::Error("configuration account uses an unsupported AccountStorage dictionary hash");
  }

  td::Ref<vm::Cell> old_storage_cell;
  if (!tlb::pack_cell(old_storage_cell, storage)) {
    return td::Status::Error("cannot repack original configuration AccountStorage");
  }
  TRY_RESULT(recorded_old_used, unpack_storage_used(storage_info.used));
  TRY_RESULT(computed_old_used, compute_storage_used(old_storage_cell));
  if (recorded_old_used != computed_old_used) {
    return td::Status::Error(PSLICE() << "configuration account has inconsistent StorageUsed: recorded="
                                      << recorded_old_used.first << "/" << recorded_old_used.second
                                      << " computed=" << computed_old_used.first << "/" << computed_old_used.second);
  }

  auto active_state = storage.state;
  if (active_state.is_null() || active_state.write().fetch_ulong(1) != 1) {
    return td::Status::Error("configuration smart-contract account is not active");
  }
  block::gen::StateInit::Record state_init;
  if (!tlb::csr_unpack(active_state, state_init) || state_init.data.is_null()) {
    return td::Status::Error("cannot unpack configuration account StateInit");
  }
  auto maybe_data = state_init.data;
  if (maybe_data.write().fetch_ulong(1) != 1 || maybe_data->size() != 0 || maybe_data->size_refs() != 1) {
    return td::Status::Error("configuration account StateInit has no canonical persistent data reference");
  }
  auto old_data = maybe_data->prefetch_ref();
  auto old_data_slice = vm::load_cell_slice(old_data);
  if (!old_data_slice.have_refs(1) || old_data_slice.prefetch_ref()->get_hash() != effective_config->get_hash()) {
    return td::Status::Error("configuration account persistent data does not reference its effective configuration");
  }

  vm::CellBuilder data_builder;
  if (!data_builder.store_bits_bool(old_data_slice.data_bits(), old_data_slice.size()) ||
      !data_builder.store_ref_bool(pending_config)) {
    return td::Status::Error("cannot begin rebuilding configuration account persistent data");
  }
  for (unsigned i = 1; i < old_data_slice.size_refs(); ++i) {
    if (!data_builder.store_ref_bool(old_data_slice.prefetch_ref(i))) {
      return td::Status::Error("cannot preserve configuration account persistent-data references");
    }
  }
  auto new_data = data_builder.finalize();
  vm::CellBuilder maybe_data_builder;
  if (!(maybe_data_builder.store_long_bool(1, 1) && maybe_data_builder.store_ref_bool(new_data))) {
    return td::Status::Error("cannot repack configuration account persistent-data reference");
  }
  state_init.data = vm::load_cell_slice_ref(maybe_data_builder.finalize());

  td::Ref<vm::Cell> state_init_cell;
  td::Ref<vm::Cell> account_state_cell;
  if (!tlb::pack_cell(state_init_cell, state_init) ||
      !block::gen::t_AccountState.cell_pack_account_active(account_state_cell,
                                                           vm::load_cell_slice_ref(state_init_cell))) {
    return td::Status::Error("cannot repack configuration account active state");
  }
  storage.state = vm::load_cell_slice_ref(account_state_cell);

  td::Ref<vm::Cell> new_storage_cell;
  if (!tlb::pack_cell(new_storage_cell, storage)) {
    return td::Status::Error("cannot repack modified configuration AccountStorage");
  }
  TRY_RESULT(new_used, compute_storage_used(new_storage_cell));
  vm::CellBuilder used_builder;
  if (!block::store_UInt7(used_builder, new_used.first, new_used.second)) {
    return td::Status::Error("configuration account StorageUsed exceeds VarUInteger 7");
  }
  storage_info.used = vm::load_cell_slice_ref(used_builder.finalize());
  account.storage = vm::load_cell_slice_ref(new_storage_cell);
  if (!tlb::csr_pack(account.storage_stat, storage_info) || !tlb::pack_cell(shard_account.account, account) ||
      !block::gen::t_Account.validate_ref(shard_account.account)) {
    return td::Status::Error("cannot repack modified configuration account");
  }

  vm::CellBuilder shard_account_builder;
  if (!block::gen::t_ShardAccount.pack(shard_account_builder, shard_account) ||
      !accounts.set_builder(config_addr, shard_account_builder, vm::Dictionary::SetMode::Replace)) {
    return td::Status::Error("cannot replace configuration account in ShardAccounts");
  }
  vm::CellBuilder accounts_builder;
  if (!accounts.append_dict_to_bool(accounts_builder)) {
    return td::Status::Error("cannot repack masterchain ShardAccounts");
  }
  state.accounts = accounts_builder.finalize();

  TRY_RESULT(installed_contract_config, config_from_smc(state, config_addr));
  if (installed_contract_config->get_hash() != pending_config->get_hash() ||
      effective_config->get_hash() == pending_config->get_hash()) {
    return td::Status::Error("configuration-contract pending change was not installed exactly");
  }
  return td::Status::OK();
}

td::Status stage_global_id_config_change(block::gen::ShardStateUnsplit::Record& state,
                                         const block::gen::McStateExtra::Record& extra) {
  try {
    return stage_global_id_config_change_impl(state, extra);
  } catch (vm::VmError& error) {
    return error.as_status("cannot stage ConfigParam 19 in the configuration smart-contract account: ");
  } catch (vm::VmVirtError& error) {
    return error.as_status("cannot stage ConfigParam 19 in a virtualized configuration smart-contract account: ");
  }
}

td::Status verify_bootstrapped_masterchain_config(const td::Ref<vm::Cell>& state_root,
                                                  unsigned previous_catchain_seqno) {
  block::gen::ShardStateUnsplit::Record state;
  block::gen::McStateExtra::Record extra;
  if (!tlb::unpack_cell(state_root, state) || state.custom.is_null() || state.custom->prefetch_ulong(1) != 1 ||
      !tlb::unpack_cell(state.custom->prefetch_ref(), extra)) {
    return td::Status::Error("cannot unpack bootstrapped masterchain configuration");
  }
  td::BitArray<256> config_addr;
  TRY_RESULT(effective_config, config_from_state_extra(extra, &config_addr));
  TRY_RESULT(contract_config, config_from_smc(state, config_addr));
  if (effective_config->get_hash() != contract_config->get_hash() || !extra.r1.after_key_block) {
    return td::Status::Error("bootstrapped effective and configuration-contract roots differ");
  }
  TRY_STATUS(require_global_id_config(effective_config, state.global_id, "bootstrapped configuration"));
  block::gen::ValidatorInfo::Record validator_info;
  if (!tlb::csr_unpack(extra.r1.validator_info, validator_info) || !validator_info.nx_cc_updated ||
      previous_catchain_seqno == std::numeric_limits<unsigned>::max() ||
      validator_info.catchain_seqno != previous_catchain_seqno + 1) {
    return td::Status::Error("masterchain bootstrap did not rotate all catchain/shard groups");
  }
  return td::Status::OK();
}

}  // namespace

td::Result<Fixture> build_fixture(const FixtureConfig& config) {
  if (config.zerostate_path.empty() || config.base_state_path.empty() || config.contracts_dir.empty()) {
    return td::Status::Error("zerostate_path, base_state_path, and contracts_dir must be set");
  }
  if (config.transfers == 0) {
    return td::Status::Error("transfers must be positive");
  }
  if (config.transfers > (std::numeric_limits<td::uint64>::max() - 1) / 4 ||
      config.accounts < config.transfers * 4 + 1) {
    return td::Status::Error("accounts must be at least 4 * transfers + 1");
  }
  if (config.owner_balance == 0 || config.jetton_wallet_ton_balance == 0 || config.minter_ton_balance == 0 ||
      config.jetton_initial_balance == 0 || config.jetton_transfer_amount == 0 || config.message_value == 0 ||
      config.forward_ton_amount == 0 || config.forward_ton_amount >= config.message_value ||
      config.jetton_transfer_amount > config.jetton_initial_balance) {
    return td::Status::Error("fixture balances and transfer amounts are inconsistent");
  }

  const auto seed = make_seed(config.seed);
  TRY_RESULT(contracts, load_contracts(config.contracts_dir));

  TRY_RESULT(jetton_wallet_count,
             checked_mul(static_cast<Uint128>(config.transfers), static_cast<Uint128>(2), "jetton wallet count"));
  TRY_RESULT(total_supply, checked_mul(jetton_wallet_count, config.jetton_initial_balance, "jetton total supply"));
  auto empty_content = build_empty_cell();
  auto minter_data = build_minter_data(total_supply, empty_content, contracts.jw_code);
  auto minter_state_init = build_state_init(contracts.minter_code, minter_data);
  td::Bits256 minter_addr{minter_state_init->get_hash().bits()};

  TRY_RESULT(base_boc, td::read_file(config.base_state_path));
  TRY_RESULT(base_root, vm::std_boc_deserialize(base_boc.as_slice()));
  base_boc.clear();
  block::gen::ShardStateUnsplit::Record base_state_record;
  if (!tlb::unpack_cell(base_root, base_state_record)) {
    return td::Status::Error("cannot unpack basechain ShardStateUnsplit");
  }
  block::ShardId parsed_base_shard{base_state_record.shard_id};
  ton::ShardIdFull base_shard{parsed_base_shard};
  if (!parsed_base_shard.is_valid() || base_shard.workchain != 0 || base_shard.shard != ton::shardIdAll ||
      base_state_record.seq_no != 0) {
    return td::Status::Error(PSLICE() << "fixture input is not the full wc0 seqno-0 state: shard="
                                      << base_shard.to_str() << " seqno=" << base_state_record.seq_no);
  }
  // The historical tontester fixture stores the full-shard marker bit in the
  // raw ShardIdent.shard_prefix field.  The reference parser normalizes that
  // representation, but portable TLB consumers expect the canonical raw
  // prefix (zero for the full shard).  Canonicalize the in-memory copy before
  // any account/state hashes are derived; the source fixture stays untouched.
  vm::CellBuilder canonical_shard_ident;
  if (!block::tlb::t_ShardIdent.pack(canonical_shard_ident, base_shard)) {
    return td::Status::Error("cannot encode canonical full-wc0 ShardIdent");
  }
  base_state_record.shard_id = vm::load_cell_slice_ref(canonical_shard_ident.finalize());
  TRY_STATUS(require_zero_balance(base_state_record.r1.total_balance, "basechain total balance"));

  vm::AugmentedDictionary accounts{vm::load_cell_slice_ref(base_state_record.accounts), 256,
                                   block::tlb::aug_ShardAccounts};
  td::uint64 preserved_accounts = 0;
  if (!accounts.check_for_each([&](td::Ref<vm::CellSlice>, td::ConstBitPtr, int) {
        ++preserved_accounts;
        return true;
      })) {
    return td::Status::Error("cannot enumerate existing basechain accounts");
  }
  if (preserved_accounts != 0) {
    return td::Status::Error("basechain fixture must start with an empty account dictionary");
  }

  const td::uint64 hot_owner_count = config.transfers * 2;
  const td::uint64 total_jetton_wallets = config.transfers * 2;
  const td::uint64 owner_count = config.accounts - total_jetton_wallets - 1;
  std::vector<WalletInfo> hot_wallets;
  hot_wallets.reserve(hot_owner_count);
  for (td::uint64 i = 0; i < hot_owner_count; ++i) {
    TRY_RESULT(wallet, derive_wallet(seed, i, kWalletId, minter_addr, contracts));
    hot_wallets.push_back(std::move(wallet));
  }

  auto sample_w5_data = build_w5_data(hot_wallets.front().pubkey, kWalletId);
  std::vector<td::Ref<vm::Cell>> w5_roots{contracts.w5_code, sample_w5_data};
  const auto w5_used = compute_account_storage_used(config.owner_balance, w5_roots);
  auto sample_jw_data =
      build_jw_data(config.jetton_initial_balance, hot_wallets.front().w5_addr, minter_addr, contracts.jw_code);
  std::vector<td::Ref<vm::Cell>> jw_roots{contracts.jw_code, sample_jw_data};
  const auto jw_used = compute_account_storage_used(config.jetton_wallet_ton_balance, jw_roots);
  std::vector<td::Ref<vm::Cell>> minter_roots{contracts.minter_code, minter_data};
  const auto minter_used = compute_account_storage_used(config.minter_ton_balance, minter_roots);

  std::vector<block::StdAddress> source_owners(config.transfers);
  std::vector<block::StdAddress> source_jettons(config.transfers);
  std::vector<block::StdAddress> recipient_owners(config.transfers);
  std::vector<block::StdAddress> recipient_jettons(config.transfers);

  for (td::uint64 i = 0; i < owner_count; ++i) {
    td::Bits256 pubkey;
    td::Bits256 address;
    if (i < hot_owner_count) {
      pubkey = hot_wallets[i].pubkey;
      address = hot_wallets[i].w5_addr;
    } else {
      pubkey = tagged_sha256(seed, "filler", i);
      auto data = build_w5_data(pubkey, kWalletId);
      address = build_state_init(contracts.w5_code, data)->get_hash().bits();
    }
    auto data = build_w5_data(pubkey, kWalletId);
    auto account = build_account(address, config.owner_balance, contracts.w5_code, std::move(data), w5_used,
                                 base_state_record.gen_utime);
    TRY_STATUS(add_account(accounts, address, account, PSLICE() << "WalletV5 owner #" << i));
    if (i < config.transfers) {
      source_owners[i] = block::StdAddress{0, address};
    } else if (i < hot_owner_count) {
      recipient_owners[i - config.transfers] = block::StdAddress{0, address};
    }
  }

  for (td::uint64 i = 0; i < total_jetton_wallets; ++i) {
    const auto& wallet = hot_wallets[i];
    auto data = build_jw_data(config.jetton_initial_balance, wallet.w5_addr, minter_addr, contracts.jw_code);
    auto account = build_account(wallet.jw_addr, config.jetton_wallet_ton_balance, contracts.jw_code, std::move(data),
                                 jw_used, base_state_record.gen_utime);
    TRY_STATUS(add_account(accounts, wallet.jw_addr, account, PSLICE() << "jetton wallet #" << i));
    if (i < config.transfers) {
      source_jettons[i] = block::StdAddress{0, wallet.jw_addr};
    } else {
      recipient_jettons[i - config.transfers] = block::StdAddress{0, wallet.jw_addr};
    }
  }

  auto minter_account = build_account(minter_addr, config.minter_ton_balance, contracts.minter_code, minter_data,
                                      minter_used, base_state_record.gen_utime);
  TRY_STATUS(add_account(accounts, minter_addr, minter_account, "jetton minter"));

  TRY_RESULT(owner_ton, checked_mul(static_cast<Uint128>(owner_count), config.owner_balance, "owner TON balance"));
  TRY_RESULT(jetton_ton, checked_mul(static_cast<Uint128>(total_jetton_wallets), config.jetton_wallet_ton_balance,
                                     "jetton-wallet TON balance"));
  TRY_RESULT(total_base_balance, checked_add(owner_ton, jetton_ton, "basechain total balance"));
  TRY_RESULT_ASSIGN(total_base_balance,
                    checked_add(total_base_balance, config.minter_ton_balance, "basechain total balance"));

  TRY_RESULT(new_base_root, repack_accounts(base_state_record, accounts, total_base_balance));
  block::gen::ShardStateUnsplit::Record portable_base_state;
  block::gen::ShardIdent::Record portable_shard_ident;
  if (!tlb::unpack_cell(new_base_root, portable_base_state) ||
      !block::gen::t_ShardIdent.unpack(portable_base_state.shard_id.write(), portable_shard_ident) ||
      portable_shard_ident.shard_pfx_bits != 0 || portable_shard_ident.workchain_id != 0 ||
      portable_shard_ident.shard_prefix != 0) {
    return td::Status::Error("repacked basechain state has a non-canonical full-wc0 ShardIdent");
  }
  TRY_RESULT(prev_id, make_block_id(0, ton::shardIdAll, 0, new_base_root));
  {
    block::ShardState unpacked;
    TRY_STATUS(unpacked.unpack_state(prev_id, new_base_root));
  }
  TRY_RESULT(prev_state, ton::validator::create_shard_state(prev_id, new_base_root));

  TRY_RESULT(mc_boc, td::read_file(config.zerostate_path));
  TRY_RESULT(mc_root, vm::std_boc_deserialize(mc_boc.as_slice()));
  mc_boc.clear();
  block::gen::ShardStateUnsplit::Record mc_state_record;
  if (!tlb::unpack_cell(mc_root, mc_state_record)) {
    return td::Status::Error("cannot unpack masterchain ShardStateUnsplit");
  }
  block::ShardId parsed_mc_shard{mc_state_record.shard_id};
  ton::ShardIdFull mc_shard{parsed_mc_shard};
  if (!parsed_mc_shard.is_valid() || !mc_shard.is_masterchain() || mc_state_record.seq_no != 0 ||
      mc_state_record.custom.is_null() || !mc_state_record.custom->size_refs()) {
    return td::Status::Error("fixture input is not a masterchain seqno-0 state with McStateExtra");
  }

  block::gen::McStateExtra::Record extra;
  if (!tlb::unpack_cell(mc_state_record.custom->prefetch_ref(), extra)) {
    return td::Status::Error("cannot unpack McStateExtra");
  }
  if (mc_state_record.global_id != -777 || base_state_record.global_id != mc_state_record.global_id) {
    return td::Status::Error("fixture shard states must use the portable benchmark global_id -777");
  }
  TRY_STATUS(stage_global_id_config_change(mc_state_record, extra));
  block::ShardConfig shard_config;
  if (!shard_config.unpack(extra.shard_hashes) ||
      !shard_config.new_workchain(0, 0, prev_id.root_hash, prev_id.file_hash)) {
    return td::Status::Error("cannot register modified wc0 state in masterchain shard configuration");
  }
  extra.shard_hashes = shard_config.get_root_csr();
  TRY_STATUS(add_grams(extra.global_balance, total_base_balance, "McStateExtra.global_balance"));
  td::Ref<vm::Cell> extra_cell;
  if (!tlb::pack_cell(extra_cell, extra)) {
    return td::Status::Error("cannot repack McStateExtra");
  }
  vm::CellBuilder custom_builder;
  if (!(custom_builder.store_long_bool(1, 1) && custom_builder.store_ref_bool(extra_cell))) {
    return td::Status::Error("cannot repack ShardState.custom");
  }
  mc_state_record.custom = vm::load_cell_slice_ref(custom_builder.finalize());

  td::Ref<vm::Cell> new_mc_root;
  if (!tlb::pack_cell(new_mc_root, mc_state_record) ||
      !block::gen::t_ShardStateUnsplit.validate_ref(std::numeric_limits<int>::max(), new_mc_root)) {
    return td::Status::Error("cannot repack valid masterchain state");
  }
  TRY_RESULT(mc_id, make_block_id(ton::masterchainId, ton::shardIdAll, 0, new_mc_root));
  {
    block::ShardState unpacked;
    TRY_STATUS(unpacked.unpack_state(mc_id, new_mc_root));
  }
  TRY_RESULT(mc_shard_state, ton::validator::create_shard_state(mc_id, new_mc_root));
  td::Ref<ton::validator::MasterchainState> mc_state{std::move(mc_shard_state)};
  if (mc_state.is_null()) {
    return td::Status::Error("created masterchain state has the wrong dynamic type");
  }
  auto validator_set = mc_state->get_validator_set(base_shard);
  auto mc_validator_set = mc_state->get_validator_set(mc_shard);
  if (validator_set.is_null() || mc_validator_set.is_null()) {
    return td::Status::Error("cannot compute wc0 validator set");
  }
  auto validators = validator_set->export_vector();
  if (validators.empty() || mc_validator_set->export_vector().empty()) {
    return td::Status::Error("computed validator set is empty");
  }

  Manifest manifest;
  manifest.seed = seed;
  manifest.wallet_id = kWalletId;
  manifest.minter_addr = minter_addr;
  manifest.num_v5 = owner_count;
  manifest.w5_code_hash = contracts.w5_code->get_hash().bits();
  manifest.jw_code_hash = contracts.jw_code->get_hash().bits();
  SpamParams spam_params;
  spam_params.msg_value = config.message_value;
  spam_params.jetton_amount = config.jetton_transfer_amount;
  spam_params.forward_ton_amount = config.forward_ton_amount;
  std::vector<td::Ref<ton::validator::ExtMessage>> messages;
  messages.reserve(config.transfers);
  const auto ext_msg_limits = mc_state->get_ext_msg_limits();
  for (td::uint64 i = 0; i < config.transfers; ++i) {
    // Both workloads move value between the very same hot wallet pair; only the
    // signed payload differs (jetton transfer body vs plain empty-body value).
    TRY_RESULT(root, config.workload == Workload::Transfer
                         ? build_signed_simple_external(seed, i, config.transfers + i, manifest, contracts, spam_params)
                         : build_signed_external(seed, i, config.transfers + i, manifest, contracts, spam_params));
    TRY_RESULT(data, vm::std_boc_serialize(std::move(root)));
    TRY_RESULT(message, ton::validator::ExtMessageQ::create_ext_message(std::move(data), ext_msg_limits));
    messages.emplace_back(std::move(message));
  }

  Fixture fixture;
  fixture.shard = base_shard;
  fixture.prev_id = prev_id;
  fixture.mc_id = mc_id;
  fixture.state_root = std::move(new_base_root);
  fixture.prev_state = std::move(prev_state);
  fixture.mc_state = std::move(mc_state);
  fixture.validator_set = std::move(validator_set);
  fixture.mc_validator_set = std::move(mc_validator_set);
  fixture.creator = validators.front().key;
  fixture.messages = std::move(messages);
  fixture.gen_utime = base_state_record.gen_utime;
  fixture.source_owner_addresses = std::move(source_owners);
  fixture.source_jetton_addresses = std::move(source_jettons);
  fixture.recipient_owner_addresses = std::move(recipient_owners);
  fixture.recipient_jetton_addresses = std::move(recipient_jettons);
  fixture.minter_address = block::StdAddress{0, minter_addr};
  fixture.w5_code = std::move(contracts.w5_code);
  fixture.jw_code = std::move(contracts.jw_code);
  fixture.minter_code = std::move(contracts.minter_code);
  fixture.injected_accounts = config.accounts;
  fixture.preserved_accounts = preserved_accounts;
  return fixture;
}

td::Status apply_masterchain_bootstrap(Fixture& fixture, const ton::BlockCandidate& candidate) {
  if (fixture.mc_state.is_null() || !fixture.mc_id.is_masterchain() || fixture.mc_id.seqno() != 0 ||
      fixture.mc_block_data.not_null()) {
    return td::Status::Error("fixture is not awaiting a masterchain seqno-1 bootstrap");
  }
  if (!candidate.id.is_masterchain() || candidate.id.seqno() != 1 ||
      block::compute_file_hash(candidate.data) != candidate.id.file_hash) {
    return td::Status::Error("masterchain bootstrap candidate has an invalid id or file hash");
  }

  TRY_RESULT(block_data, ton::validator::create_block(candidate.id, candidate.data.clone()));
  block::gen::Block::Record block_record;
  if (!tlb::unpack_cell(block_data->root_cell(), block_record)) {
    return td::Status::Error("cannot unpack masterchain bootstrap block");
  }
  block::gen::BlockInfo::Record block_info;
  if (!tlb::unpack_cell(block_record.info, block_info) || !block_info.key_block || block_record.global_id != -777) {
    return td::Status::Error("masterchain bootstrap must produce a key block for global_id -777");
  }
  auto previous_root = fixture.mc_state->root_cell();
  if (previous_root.is_null()) {
    return td::Status::Error("masterchain bootstrap previous state has no root cell");
  }
  block::gen::ShardStateUnsplit::Record previous_state;
  block::gen::McStateExtra::Record previous_extra;
  block::gen::ValidatorInfo::Record previous_validator_info;
  if (!tlb::unpack_cell(previous_root, previous_state) || previous_state.custom.is_null() ||
      previous_state.custom->prefetch_ulong(1) != 1 ||
      !tlb::unpack_cell(previous_state.custom->prefetch_ref(), previous_extra) ||
      !tlb::csr_unpack(previous_extra.r1.validator_info, previous_validator_info)) {
    return td::Status::Error("cannot unpack pre-bootstrap masterchain validator info");
  }
  TRY_STATUS(vm::MerkleUpdate::validate(block_record.state_update));
  TRY_STATUS(vm::MerkleUpdate::may_apply(previous_root, block_record.state_update));
  TRY_RESULT(next_root, vm::MerkleUpdate::apply(previous_root, block_record.state_update));
  TRY_STATUS(verify_bootstrapped_masterchain_config(next_root, previous_validator_info.catchain_seqno));
  {
    block::ShardState unpacked;
    TRY_STATUS(unpacked.unpack_state(candidate.id, next_root));
  }
  TRY_RESULT(next_shard_state, ton::validator::create_shard_state(candidate.id, next_root));
  td::Ref<ton::validator::MasterchainState> next_mc_state{std::move(next_shard_state)};
  if (next_mc_state.is_null()) {
    return td::Status::Error("masterchain bootstrap produced a non-masterchain state");
  }
  auto base_descriptor = next_mc_state->get_shard_from_config(fixture.shard);
  if (base_descriptor.is_null() || base_descriptor->top_block_id() != fixture.prev_id) {
    return td::Status::Error("masterchain bootstrap did not preserve the registered wc0 state");
  }
  auto validator_set = next_mc_state->get_validator_set(fixture.shard);
  if (validator_set.is_null()) {
    return td::Status::Error("cannot compute the post-bootstrap wc0 validator set");
  }
  auto validators = validator_set->export_vector();
  if (validators.empty()) {
    return td::Status::Error("post-bootstrap wc0 validator set is empty");
  }

  fixture.mc_id = candidate.id;
  fixture.mc_state = std::move(next_mc_state);
  fixture.mc_block_data = std::move(block_data);
  fixture.validator_set = std::move(validator_set);
  fixture.creator = validators.front().key;
  fixture.gen_utime = std::max(fixture.gen_utime, fixture.mc_state->get_unix_time());
  return td::Status::OK();
}

}  // namespace bench::collation
