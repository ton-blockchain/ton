/*
    This file is part of TON Blockchain Library.

    TON Blockchain Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    TON Blockchain Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with TON Blockchain Library.  If not, see <http://www.gnu.org/licenses/>.
*/
#include "interfaces/validator-full-id.h"
#include "td/utils/Random.h"

#include "block-auto.h"
#include "block-parse.h"
#include "shard.hpp"
#include "transaction.h"
#include "validator-registry-watcher.hpp"

namespace ton::validator {

static bool cell_equal(const Ref<vm::Cell>& a, const Ref<vm::Cell>& b) {
  if (a.is_null()) {
    return b.is_null();
  }
  if (b.is_null()) {
    return false;
  }
  return a->get_hash() == b->get_hash();
}

void ValidatorRegistryWatcher::update(Ref<MasterchainState> mc_state, Ref<ValidatorManagerOptions> opts) {
  if (opts->get_collators_list() != collators_list_ || mc_state->is_key_state()) {
    collators_list_ = opts->get_collators_list();
    new_entry_cell_ = make_entry_cell(mc_state);
    update_at_ = td::Timestamp::now();
  }
  if (mc_state->get_unix_time() < (UnixTime)td::Clocks::system() - 60) {
    return;
  }
  if (!(update_at_ && update_at_.is_in_past())) {
    return;
  }
  update_at_ = td::Timestamp::never();

  bool found = false;
  td::uint32 val_set_idx = 0, val_idx = 0;
  td::Bits256 public_key;
  for (int next : {0, 1, -1}) {
    auto val_set = mc_state->get_total_validator_set(next);
    if (val_set.not_null()) {
      auto [val, idx] = val_set->get_validator_with_idx(key_hash_.bits256_value());
      if (val) {
        val_set_idx = 34 + next * 2;
        val_idx = idx;
        public_key = val->key.as_bits256();
        found = true;
        break;
      }
    }
  }
  if (!found) {
    return;
  }

  block::ValidatorRegistryConfig config;
  auto r_contract_data = get_contract_data(mc_state, config);
  if (r_contract_data.is_error()) {
    LOG(WARNING) << "Update registry: " << r_contract_data.move_as_error();
    update_at_.relax(td::Timestamp::in(60.0));
    return;
  }
  Ref<vm::Cell> contract_data = r_contract_data.move_as_ok();
  auto r_info = get_validator_info(contract_data, public_key);
  if (r_info.is_error()) {
    LOG(ERROR) << "Update registry: failed to get validator info from registry: " << r_info.move_as_error();
    update_at_.relax(td::Timestamp::in(3600.0));
    return;
  }

  auto r_last_cleanup_key_block_seqno = get_last_cleanup_key_block_seqno(contract_data);
  if (r_last_cleanup_key_block_seqno.is_error()) {
    LOG(ERROR) << "Update registry: failed to get last cleanup seqno: "
               << r_last_cleanup_key_block_seqno.move_as_error();
  } else {
    auto last_cleanup_key_block_seqno = r_last_cleanup_key_block_seqno.move_as_ok();
    auto vset = mc_state->get_total_validator_set(0);
    if (vset.not_null() && last_cleanup_key_block_seqno < mc_state->last_key_block_id().seqno() &&
        td::Random::fast(1, (int)vset->size()) == 1) {
      LOG(INFO) << "Update registry: cleanup";
      update_at_.relax(td::Timestamp::in(60.0));
      send_external_message(
          config.contract_address,
          vm::CellBuilder{}.store_long(block::gen::ValRegistryMessageCleanup::cons_tag[0], 32).as_cellslice())
          .start()
          .detach("send registry cleanup");
    }
  }

  ValidatorInfo info = r_info.move_as_ok();
  if (cell_equal(new_entry_cell_, info.entry)) {
    update_at_.relax(td::Timestamp::in(60.0));
    return;
  }
  if (info.allow_update_at > (UnixTime)td::Clocks::system()) {
    auto t = info.allow_update_at - (UnixTime)td::Clocks::system() + 1;
    LOG(INFO) << "Update registry: need to update entry, wait for allow_update_at (" << t << " s)";
    update_at_.relax(td::Timestamp::in(t));
    return;
  }

  LOG(INFO) << "Update registry: updating entry";
  vm::CellBuilder cb;
  cb.store_maybe_ref(new_entry_cell_);
  block::gen::ValRegistryRequest::Record request{.last_key_block_seqno = mc_state->last_key_block_id().seqno(),
                                                 .val_set = (int)val_set_idx,
                                                 .val_idx = (int)val_idx,
                                                 .valid_until = (UnixTime)td::Clocks::system() + 60,
                                                 .new_entry = cb.as_cellslice_ref()};
  Ref<vm::Cell> request_cell;
  CHECK(block::gen::pack_cell(request_cell, request));
  sign_and_send_request(config.contract_address, std::move(request_cell)).start().detach("send registry request");

  update_at_.relax(td::Timestamp::in(60.0));
}

Ref<vm::Cell> ValidatorRegistryWatcher::make_entry_cell(Ref<MasterchainState> mc_state) {
  auto r_config = mc_state->get_validator_registry_config();
  if (r_config.is_error()) {
    LOG(WARNING) << "Make entry cell: " << r_config.move_as_error();
    return {};
  }
  auto config = r_config.move_as_ok();
  std::set<adnl::AdnlNodeIdShort> collators;
  for (auto& shard : collators_list_->shards) {
    collators.insert(shard.collators.begin(), shard.collators.end());
  }
  if (collators.size() > config.max_collators_per_validator) {
    LOG(WARNING) << "Make entry cell: too many collators, pruning (max. " << config.max_collators_per_validator
                 << ", found " << collators.size() << ")";
    while (collators.size() > config.max_collators_per_validator) {
      collators.erase(collators.begin());
    }
  }
  LOG(INFO) << "Make entry cell: " << collators.size() << " collators";
  if (collators.empty()) {
    return {};
  }
  auto monitoring_shards_all = vm::CellBuilder{}.store_long(0, 4).store_ones(1).as_cellslice_ref();
  vm::Dictionary collators_dict{256};
  for (const adnl::AdnlNodeIdShort& id : collators) {
    vm::CellBuilder cb;
    CHECK(block::gen::t_ValRegistryCollator.pack_val_registry_collator(cb, monitoring_shards_all));
    collators_dict.set_builder(id.bits256_value(), std::move(cb));
  }
  Ref<vm::Cell> result;
  CHECK(block::gen::t_ValRegistryEntry.cell_pack_val_registry_entry(result, collators_dict.get_root(),
                                                                    monitoring_shards_all));
  return result;
}

td::actor::Task<> ValidatorRegistryWatcher::sign_and_send_request(StdSmcAddress addr, Ref<vm::Cell> request_cell) {
  td::BufferSlice to_sign{request_cell->get_hash().as_slice()};
  td::BufferSlice signature =
      co_await td::actor::ask(keyring_, &keyring::Keyring::sign_message, key_hash_, std::move(to_sign));
  auto body = vm::CellBuilder{}
                  .store_long(block::gen::ValRegistryMessageRequest::cons_tag[0], 32)
                  .store_bytes(signature)
                  .append_cellslice(vm::load_cell_slice(request_cell))
                  .as_cellslice();
  co_await send_external_message(addr, std::move(body));
  co_return {};
}

td::actor::Task<> ValidatorRegistryWatcher::send_external_message(StdSmcAddress addr, vm::CellSlice body) {
  block::gen::CommonMsgInfo::Record_ext_in_msg_info info;
  info.src = vm::CellBuilder{}.store_zeroes(2).as_cellslice_ref();  // addr_none$00
  info.dest = block::tlb::t_MsgAddressInt.pack_std_address(masterchainId, addr);
  CHECK(block::tlb::t_Grams.pack_integer(info.import_fee, td::zero_refint()));
  vm::CellBuilder cb;
  CHECK(block::gen::pack(cb, info));
  cb.store_zeroes(1);  // State init
  if (cb.can_extend_by(body.size() + 1, body.size_refs())) {
    cb.store_zeroes(1);
    cb.append_cellslice(body);
  } else {
    cb.store_ones(1);
    cb.store_ref(vm::CellBuilder{}.append_cellslice(body).finalize_novm());
  }
  td::BufferSlice data = vm::std_boc_serialize(cb.finalize_novm()).ensure().move_as_ok();
  co_await td::actor::ask(manager_, &ValidatorManager::new_external_message_query, std::move(data));
  co_return {};
}

std::map<PublicKeyHash, std::vector<adnl::AdnlNodeIdShort>> ValidatorRegistryWatcher::get_collators_by_validator(
    Ref<MasterchainState> mc_state) {
  block::ValidatorRegistryConfig config;
  auto r_data = get_contract_data(mc_state, config);
  if (r_data.is_error()) {
    LOG(DEBUG) << "Get collators from validator registry: " << r_data.move_as_error();
    return {};
  }
  auto contract_data = r_data.move_as_ok();

  std::map<PublicKeyHash, std::vector<adnl::AdnlNodeIdShort>> result;
  for (int next : {-1, 0, 1}) {
    auto val_set = mc_state->get_total_validator_set(next);
    if (val_set.is_null()) {
      continue;
    }
    for (auto& val : val_set->export_vector()) {
      PublicKeyHash short_id = ValidatorFullId{val.key}.compute_short_id();
      if (result.contains(short_id)) {
        continue;
      }
      auto r_entry = get_validator_entry(contract_data, val.key.as_bits256(), config);
      if (r_entry.is_error()) {
        LOG(WARNING) << "Get collators by validator from validator registry: failed to get entry for pubkey "
                     << val.key.as_bits256().to_hex() << ": " << r_entry.move_as_error();
        continue;
      }
      auto entry = r_entry.move_as_ok();
      if (!entry.collators.empty()) {
        result.emplace(short_id, std::move(entry.collators));
      }
    }
  }
  return result;
}

td::Result<Ref<vm::Cell>> ValidatorRegistryWatcher::get_contract_data(Ref<MasterchainState> mc_state,
                                                                      block::ValidatorRegistryConfig& registry_config) {
  TRY_RESULT_ASSIGN(registry_config, mc_state->get_validator_registry_config());
  auto mc_config = Ref<MasterchainStateQ>{mc_state}->get_config();
  if (!mc_config->is_special_smartcontract(registry_config.contract_address)) {
    return td::Status::Error(PSTRING() << "registry contract -1:" << registry_config.contract_address.to_hex()
                                       << " should be special");
  }
  auto accounts_dict = mc_config->get_accounts_dict();
  const StdSmcAddress& addr = registry_config.contract_address;
  auto account_cs = accounts_dict.lookup(addr);
  if (account_cs.is_null()) {
    return td::Status::Error(PSTRING() << "registry contract -1:" << addr.to_hex() << " not found");
  }
  block::Account account;
  if (!account.unpack(std::move(account_cs), mc_state->get_unix_time(), true)) {
    return td::Status::Error(PSTRING() << "failed to unpack registry contract -1:" << addr.to_hex());
  }
  if (account.data.is_null()) {
    return td::Status::Error(PSTRING() << "registry contract -1:" << addr.to_hex() << " has no data");
  }
  return account.data;
}

td::Result<ValidatorRegistryWatcher::ValidatorInfo> ValidatorRegistryWatcher::get_validator_info(
    Ref<vm::Cell> contract_data, td::Bits256 public_key) {
  try {
    block::gen::ValRegistryStorage::Record rec;
    if (!block::gen::unpack_cell(contract_data, rec)) {
      return td::Status::Error("failed to parse contract data");
    }
    auto cs = vm::Dictionary{rec.registry, 256}.lookup(public_key);
    if (cs.is_null()) {
      return ValidatorInfo{};
    }
    block::gen::ValRegistryValidator::Record val_rec;
    if (!block::gen::csr_unpack(cs, val_rec)) {
      return td::Status::Error("failed to parse contract data");
    }
    return ValidatorInfo{.allow_update_at = val_rec.allow_update_at, .entry = val_rec.entry->prefetch_ref()};
  } catch (vm::VmError& e) {
    return e.as_status("failed to parse contract data: ");
  }
}

td::Result<ValidatorRegistryWatcher::Entry> ValidatorRegistryWatcher::parse_entry(
    Ref<vm::Cell> cell, const block::ValidatorRegistryConfig& registry_config) {
  try {
    if (cell.is_null()) {
      return Entry{};
    }
    block::gen::ValRegistryEntry::Record rec;
    if (!block::gen::unpack_cell(cell, rec)) {
      return td::Status::Error("failed to parse entry");
    }
    Entry entry;
    td::uint32 cnt = 0;
    vm::Dictionary{rec.collators, 256}.check_for_each([&](Ref<vm::CellSlice>, td::ConstBitPtr key, int) -> bool {
      if (cnt >= registry_config.max_collators_per_validator) {
        return false;
      }
      ++cnt;
      entry.collators.emplace_back(td::Bits256{key});
      return true;
    });
    return entry;
  } catch (vm::VmError& e) {
    return e.as_status("failed to parse entry: ");
  }
}

td::Result<ValidatorRegistryWatcher::Entry> ValidatorRegistryWatcher::get_validator_entry(
    Ref<vm::Cell> contract_data, td::Bits256 public_key, const block::ValidatorRegistryConfig& registry_config) {
  TRY_RESULT(info, get_validator_info(contract_data, public_key));
  return parse_entry(info.entry, registry_config);
}

td::Result<BlockSeqno> ValidatorRegistryWatcher::get_last_cleanup_key_block_seqno(Ref<vm::Cell> contract_data) {
  try {
    block::gen::ValRegistryStorage::Record rec;
    if (!block::gen::unpack_cell(contract_data, rec)) {
      return td::Status::Error("failed to parse contract data");
    }
    return rec.last_cleanup_key_block_seqno;
  } catch (vm::VmError& e) {
    return e.as_status("failed to parse contract data: ");
  }
}

}  // namespace ton::validator
