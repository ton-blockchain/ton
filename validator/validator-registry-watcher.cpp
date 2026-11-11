/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#include "td/utils/Random.h"

#include "block-auto.h"
#include "block-parse.h"
#include "shard.hpp"
#include "transaction.h"
#include "validator-registry-watcher.hpp"

namespace ton::validator {

namespace {
bool cell_equal(const Ref<vm::Cell>& a, const Ref<vm::Cell>& b) {
  if (a.is_null()) {
    return b.is_null();
  }
  if (b.is_null()) {
    return false;
  }
  return a->get_hash() == b->get_hash();
}

struct Entry {
  std::vector<adnl::AdnlNodeIdShort> collators;
};

struct ValidatorInfo {
  UnixTime allow_update_at = 0;
  Ref<vm::Cell> entry = {};
};

struct RegistryState {
  vm::Dictionary validators{256};
  BlockSeqno last_cleanup_key_block_seqno = 0;
  block::ValidatorRegistryConfig config;

  td::Result<ValidatorInfo> get_validator_info(td::Bits256 public_key) {
    try {
      auto cs = validators.lookup(public_key);
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

  td::Result<Entry> get_validator_entry(td::Bits256 public_key) {
    try {
      TRY_RESULT(info, get_validator_info(public_key));
      if (info.entry.is_null()) {
        return Entry{};
      }
      block::gen::ValRegistryEntry::Record rec;
      if (!block::gen::unpack_cell(info.entry, rec)) {
        return td::Status::Error("failed to parse entry");
      }
      Entry entry;
      td::uint32 cnt = 0;
      vm::Dictionary{rec.collators, 256}.check_for_each([&](Ref<vm::CellSlice>, td::ConstBitPtr key, int) -> bool {
        if (cnt >= config.max_collators_per_validator) {
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

  static td::Result<RegistryState> fetch(Ref<MasterchainState> mc_state) {
    TRY_RESULT(registry_config, mc_state->get_validator_registry_config());
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

    block::gen::ValRegistryStorage::Record rec;
    if (!block::gen::unpack_cell(account.data, rec)) {
      return td::Status::Error(PSTRING() << "failed to unpack registry contract -1:" << addr.to_hex() << " data");
    }
    RegistryState result;
    result.validators = vm::Dictionary{rec.registry, 256, false};
    result.last_cleanup_key_block_seqno = rec.last_cleanup_key_block_seqno;
    result.config = std::move(registry_config);
    return std::move(result);
  }
};

class ValidatorRegistryWatcherImpl : public ValidatorRegistryWatcher {
 public:
  explicit ValidatorRegistryWatcherImpl(td::actor::ActorId<ValidatorManager> manager,
                                        td::actor::ActorId<keyring::Keyring> keyring)
      : manager_(manager), keyring_(keyring) {
  }

  void add_validator_key(PublicKeyHash key_hash) override {
    local_validators_.emplace(key_hash, LocalValidator{});
    update_is_current_validator();
  }

  void del_validator_key(PublicKeyHash key_hash) override {
    local_validators_.erase(key_hash);
    update_is_current_validator();
  }

  void update(Ref<MasterchainState> mc_state, Ref<ValidatorManagerOptions> opts) override;

 private:
  td::actor::ActorId<ValidatorManager> manager_;
  td::actor::ActorId<keyring::Keyring> keyring_;

  struct LocalValidator {
    td::Timestamp update_at = td::Timestamp::now();
  };
  std::map<PublicKeyHash, LocalValidator> local_validators_;
  td::Timestamp try_cleanup_at_ = td::Timestamp::now();
  bool is_current_validator_ = false;
  bool inited_ = false;

  Ref<MasterchainState> mc_state_;
  RegistryState registry_state_;
  Ref<CollatorsList> collators_list_;
  Ref<vm::Cell> new_entry_cell_;

  void update_is_current_validator() {
    is_current_validator_ = false;
    if (mc_state_.is_null()) {
      return;
    }
    auto val_set = mc_state_->get_total_validator_set(0);
    if (val_set.is_null()) {
      return;
    }
    for (auto& [id, _] : local_validators_) {
      if (val_set->get_validator(id.bits256_value())) {
        is_current_validator_ = true;
        return;
      }
    }
  }

  void update_local_validator(PublicKeyHash key_hash, LocalValidator& validator);
  Ref<vm::Cell> make_entry_cell();

  td::actor::Task<> sign_and_send_request(StdSmcAddress addr, PublicKeyHash key_hash, Ref<vm::Cell> request_cell);
  td::actor::Task<> send_external_message(StdSmcAddress addr, vm::CellSlice body);
};

void ValidatorRegistryWatcherImpl::update(Ref<MasterchainState> mc_state, Ref<ValidatorManagerOptions> opts) {
  mc_state_ = mc_state;
  auto r_registry_state = RegistryState::fetch(mc_state_);
  if (r_registry_state.is_error()) {
    VLOG(validator, WARNING) << "Validator registry: " << r_registry_state.move_as_error();
    registry_state_ = RegistryState{};
  } else {
    registry_state_ = r_registry_state.move_as_ok();
  }
  if (mc_state_->is_key_state() || !inited_) {
    update_is_current_validator();
  }
  inited_ = true;
  if (local_validators_.empty()) {
    return;
  }
  if (opts->get_collators_list() != collators_list_ || mc_state_->is_key_state()) {
    collators_list_ = opts->get_collators_list();
    new_entry_cell_ = make_entry_cell();
    for (auto& [_, validator] : local_validators_) {
      validator.update_at = td::Timestamp::now();
    }
  }
  if (mc_state_->get_unix_time() < (UnixTime)td::Clocks::system() - 60) {
    return;
  }
  for (auto& [id, validator] : local_validators_) {
    update_local_validator(id, validator);
  }

  auto vset = mc_state_->get_total_validator_set(0);
  if (is_current_validator_ && vset.not_null() &&
      registry_state_.last_cleanup_key_block_seqno < mc_state_->last_key_block_id().seqno() &&
      td::Random::fast(1, (int)vset->size()) == 1 && try_cleanup_at_.is_in_past()) {
    VLOG(validator, INFO) << "Update registry: cleanup";
    send_external_message(
        registry_state_.config.contract_address,
        vm::CellBuilder{}.store_long(block::gen::ValRegistryMessageCleanup::cons_tag[0], 32).as_cellslice())
        .start()
        .detach("send registry cleanup");
    try_cleanup_at_ = td::Timestamp::in(60.0);
  }
}

void ValidatorRegistryWatcherImpl::update_local_validator(PublicKeyHash key_hash, LocalValidator& validator) {
  if (!validator.update_at || !validator.update_at.is_in_past()) {
    return;
  }
  validator.update_at = td::Timestamp::never();
  bool found = false;
  td::uint32 val_set_idx = 0, val_idx = 0;
  td::Bits256 public_key;
  for (int next : {0, 1, -1}) {
    auto val_set = mc_state_->get_total_validator_set(next);
    if (val_set.not_null()) {
      auto [val, idx] = val_set->get_validator_with_idx(key_hash.bits256_value());
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
  auto r_info = registry_state_.get_validator_info(public_key);
  if (r_info.is_error()) {
    VLOG(validator, ERROR) << "Update registry: failed to get validator info from registry: " << r_info.move_as_error();
    validator.update_at.relax(td::Timestamp::in(3600.0));
    return;
  }
  ValidatorInfo info = r_info.move_as_ok();
  if (cell_equal(new_entry_cell_, info.entry)) {
    validator.update_at.relax(td::Timestamp::in(60.0));
    return;
  }
  if (info.allow_update_at > (UnixTime)td::Clocks::system()) {
    auto t = info.allow_update_at - (UnixTime)td::Clocks::system() + 1;
    VLOG(validator, INFO) << "Update registry: need to update entry, wait for allow_update_at (" << t << " s)";
    validator.update_at.relax(td::Timestamp::in(t));
    return;
  }

  VLOG(validator, INFO) << "Update registry: updating entry";
  vm::CellBuilder cb;
  cb.store_maybe_ref(new_entry_cell_);
  block::gen::ValRegistryRequest::Record request{.last_key_block_seqno = mc_state_->last_key_block_id().seqno(),
                                                 .val_set = (int)val_set_idx,
                                                 .val_idx = (int)val_idx,
                                                 .valid_until = (UnixTime)td::Clocks::system() + 60,
                                                 .new_entry = cb.as_cellslice_ref()};
  Ref<vm::Cell> request_cell;
  CHECK(block::gen::pack_cell(request_cell, request));
  sign_and_send_request(registry_state_.config.contract_address, key_hash, std::move(request_cell))
      .start()
      .detach("send registry request");
  validator.update_at.relax(td::Timestamp::in(60.0));
}

Ref<vm::Cell> ValidatorRegistryWatcherImpl::make_entry_cell() {
  std::set<adnl::AdnlNodeIdShort> collators;
  collators.insert(collators_list_->register_collators.begin(), collators_list_->register_collators.end());
  if (collators.size() > registry_state_.config.max_collators_per_validator) {
    VLOG(validator, WARNING) << "Make entry cell: too many collators, pruning (max. "
                             << registry_state_.config.max_collators_per_validator << ", found " << collators.size()
                             << ")";
    while (collators.size() > registry_state_.config.max_collators_per_validator) {
      collators.erase(collators.begin());
    }
  }
  VLOG(validator, INFO) << "Make entry cell: " << collators.size() << " collators";
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

td::actor::Task<> ValidatorRegistryWatcherImpl::sign_and_send_request(StdSmcAddress addr, PublicKeyHash key_hash,
                                                                      Ref<vm::Cell> request_cell) {
  td::BufferSlice to_sign{request_cell->get_hash().as_slice()};
  td::BufferSlice signature =
      co_await td::actor::ask(keyring_, &keyring::Keyring::sign_message, key_hash, std::move(to_sign));
  auto body = vm::CellBuilder{}
                  .store_long(block::gen::ValRegistryMessageRequest::cons_tag[0], 32)
                  .store_bytes(signature)
                  .append_cellslice(vm::load_cell_slice(request_cell))
                  .as_cellslice();
  co_await send_external_message(addr, std::move(body));
  co_return {};
}

td::actor::Task<> ValidatorRegistryWatcherImpl::send_external_message(StdSmcAddress addr, vm::CellSlice body) {
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

}  // namespace

std::set<adnl::AdnlNodeIdShort> ValidatorRegistryWatcher::get_all_collators(Ref<MasterchainState> mc_state) {
  auto r_registry_state = RegistryState::fetch(mc_state);
  if (r_registry_state.is_error()) {
    VLOG(validator, DEBUG) << "Get collators from validator registry: " << r_registry_state.move_as_error();
    return {};
  }
  auto registry_state = r_registry_state.move_as_ok();

  std::set<PublicKeyHash> all_validator_keys;
  for (int next : {-1, 0, 1}) {
    auto val_set = mc_state->get_total_validator_set(next);
    if (val_set.not_null()) {
      for (auto& val : val_set->export_vector()) {
        PublicKeyHash key = PublicKey{pubkeys::Ed25519{val.key}}.compute_short_id();
        all_validator_keys.insert(key);
        if (!val.addr.is_zero()) {
          all_validator_keys.emplace(val.addr);
        }
      }
    }
  }

  std::set<adnl::AdnlNodeIdShort> all_collators;
  std::set<PublicKeyHash> visited;
  for (int next : {-1, 0, 1}) {
    auto val_set = mc_state->get_total_validator_set(next);
    if (val_set.not_null()) {
      for (auto& val : val_set->export_vector()) {
        PublicKeyHash key = PublicKey{pubkeys::Ed25519{val.key}}.compute_short_id();
        if (!visited.insert(key).second) {
          continue;
        }
        auto r_entry = registry_state.get_validator_entry(val.key.as_bits256());
        if (r_entry.is_error()) {
          VLOG(validator, WARNING) << "Get collators from validator registry: failed to get entry for pubkey "
                                   << val.key.as_bits256().to_hex() << ": " << r_entry.move_as_error();
          continue;
        }
        auto entry = r_entry.move_as_ok();
        for (const adnl::AdnlNodeIdShort& collator_id : entry.collators) {
          if (!all_validator_keys.contains(collator_id.pubkey_hash())) {
            all_collators.insert(collator_id);
          }
        }
      }
    }
  }
  return all_collators;
}

td::actor::ActorOwn<ValidatorRegistryWatcher> ValidatorRegistryWatcher::create(
    td::actor::ActorId<ValidatorManager> manager, td::actor::ActorId<keyring::Keyring> keyring) {
  return td::actor::create_actor<ValidatorRegistryWatcherImpl>("ValidatorRegistry", std::move(manager),
                                                               std::move(keyring));
}

}  // namespace ton::validator
