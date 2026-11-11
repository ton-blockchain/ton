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

class ValidatorRegistryStateImplNone : public ValidatorRegistryState {
 public:
  ValidatorRegistryStateImplNone() : ValidatorRegistryState(block::ValidatorRegistryConfig{}) {
  }

  std::set<adnl::AdnlNodeIdShort> get_all_collators(Ref<MasterchainState> mc_state) override {
    return {};
  }
};

class ValidatorRegistryStateImplLegacy : public ValidatorRegistryState {
 public:
  struct Entry {
    std::vector<adnl::AdnlNodeIdShort> collators;
  };

  struct ValidatorInfo {
    UnixTime allow_update_at = 0;
    Ref<vm::Cell> entry = {};
  };

  ValidatorRegistryStateImplLegacy(block::ValidatorRegistryConfig config, Ref<vm::Cell> data)
      : ValidatorRegistryState(std::move(config)) {
    block::gen::ValRegistryStorageOld::Record rec;
    if (!block::gen::unpack_cell(data, rec)) {
      return;
    }
    validators_ = vm::Dictionary{rec.registry, 256, false};
    last_cleanup_key_block_seqno_ = rec.last_cleanup_key_block_seqno;
  }

  std::set<adnl::AdnlNodeIdShort> get_all_collators(Ref<MasterchainState> mc_state) override {
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
          auto r_entry = get_validator_entry(val.key.as_bits256());
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

  td::Result<ValidatorInfo> get_validator_info(td::Bits256 public_key) {
    try {
      auto cs = validators_.lookup(public_key);
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
        if (cnt >= config_.legacy_max_collators_per_validator) {
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

  BlockSeqno get_last_cleanup_key_block_seqno() const {
    return last_cleanup_key_block_seqno_;
  }

 private:
  vm::Dictionary validators_{256};
  BlockSeqno last_cleanup_key_block_seqno_ = 0;
};

class ValidatorRegistryStateImpl : public ValidatorRegistryState {
 public:
  ValidatorRegistryStateImpl(block::ValidatorRegistryConfig config, Ref<vm::Cell> data)
      : ValidatorRegistryState(std::move(config)) {
    block::gen::ValRegistryStorage::Record rec;
    if (!block::gen::unpack_cell(data, rec)) {
      return;
    }
    nodes_ = vm::Dictionary{rec.nodes, 256, false};
    votings_ = vm::Dictionary{rec.votings, 256 + 32 + 32, false};
  }

  std::set<adnl::AdnlNodeIdShort> get_all_collators(Ref<MasterchainState> mc_state) override {
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
    for_each_node([&](adnl::AdnlNodeIdShort id, td::uint32 roles) {
      if ((roles & ROLE_COLLATOR) && !all_validator_keys.contains(id.pubkey_hash())) {
        all_collators.insert(id);
      }
    });
    return all_collators;
  }

 private:
  vm::Dictionary nodes_{256};
  vm::Dictionary votings_{256 + 32 + 32};

  vm::Dictionary get_nodes_dict() override {
    return nodes_;
  }

  vm::Dictionary get_votings_dict() override {
    return votings_;
  }
};

}  // namespace

std::unique_ptr<ValidatorRegistryState> ValidatorRegistryState::fetch(Ref<MasterchainState> mc_state) {
  auto r_registry_config = mc_state->get_validator_registry_config();
  if (r_registry_config.is_error()) {
    return create_empty();
  }
  auto registry_config = r_registry_config.move_as_ok();
  auto mc_config = Ref<MasterchainStateQ>{mc_state}->get_config();
  if (!mc_config->is_special_smartcontract(registry_config.contract_address)) {
    return create_empty();
  }
  auto accounts_dict = mc_config->get_accounts_dict();
  const StdSmcAddress& addr = registry_config.contract_address;
  auto account_cs = accounts_dict.lookup(addr);
  if (account_cs.is_null()) {
    return create_empty();
  }
  block::Account account;
  if (!account.unpack(std::move(account_cs), mc_state->get_unix_time(), true) || account.data.is_null() ||
      account.code.is_null()) {
    return create_empty();
  }
  auto code_hash = account.code->get_hash().to_hex();
  if (code_hash == "10B3B24D2F21600F24E6DABB9DF6368A544893752170F4C4E34ECFD267D24CAC") {
    return std::make_unique<ValidatorRegistryStateImplLegacy>(std::move(registry_config), std::move(account.data));
  }
  return std::make_unique<ValidatorRegistryStateImpl>(std::move(registry_config), std::move(account.data));
}

std::unique_ptr<ValidatorRegistryState> ValidatorRegistryState::create_empty() {
  return std::make_unique<ValidatorRegistryStateImplNone>();
}

void ValidatorRegistryState::for_each_node(std::function<void(adnl::AdnlNodeIdShort, td::uint32)> f) {
  try {
    get_nodes_dict().check_for_each([&](Ref<vm::CellSlice> value, td::ConstBitPtr key, int key_len) {
      CHECK(key_len == 256);
      adnl::AdnlNodeIdShort id{td::Bits256{key}};
      block::gen::ValRegistryNode::Record rec;
      if (!block::gen::csr_unpack(value, rec)) {
        VLOG(validator, WARNING) << "Invalid validator registry node " << id;
        return true;
      }
      f(id, rec.roles);
      return true;
    });
  } catch (vm::VmError& e) {
    VLOG(validator, WARNING) << "Dictionary error in validator registry state: " << e.get_msg();
  }
}

void ValidatorRegistryState::scan_nodes_diff(ValidatorRegistryState& prev, ValidatorRegistryState& cur,
                                             std::function<void(adnl::AdnlNodeIdShort, td::uint32, td::uint32)> f) {
  try {
    auto cur_dict = cur.get_nodes_dict();
    prev.get_nodes_dict().scan_diff(
        cur_dict, [&](td::ConstBitPtr key, int key_len, Ref<vm::CellSlice> cs_old, Ref<vm::CellSlice> cs_new) {
          CHECK(key_len == 256);
          adnl::AdnlNodeIdShort id{td::Bits256{key}};
          td::uint32 roles_old = 0, roles_new = 0;
          if (cs_old.not_null()) {
            block::gen::ValRegistryNode::Record rec;
            if (block::gen::csr_unpack(cs_old, rec)) {
              roles_old = rec.roles;
            } else {
              VLOG(validator, WARNING) << "Invalid validator registry node " << id;
            }
          }
          if (cs_new.not_null()) {
            block::gen::ValRegistryNode::Record rec;
            if (block::gen::csr_unpack(cs_new, rec)) {
              roles_new = rec.roles;
            } else {
              VLOG(validator, WARNING) << "Invalid validator registry node " << id;
            }
          }
          if (roles_old != roles_new) {
            f(id, roles_old, roles_new);
          }
          return true;
        });
  } catch (vm::VmError& e) {
    VLOG(validator, WARNING) << "Dictionary error in validator registry state: " << e.get_msg();
  }
}

void ValidatorRegistryState::for_each_voting_for_adnl_id(adnl::AdnlNodeIdShort adnl_id,
                                                         std::function<void(VotingKey, Voting)> f) {
  try {
    auto dict = get_votings_dict();
    if (!dict.cut_prefix_subdict(adnl_id.bits256_value().bits(), 256)) {
      VLOG(validator, WARNING) << "Dictionary error in validator registry state: cut_prefix_subdict failed";
      return;
    }
    dict.check_for_each([&](Ref<vm::CellSlice> value, td::ConstBitPtr key, int key_len) {
      CHECK(key_len == 256 + 32 + 32);
      auto roles_add = (td::uint32)(key + 256).get_uint(32);
      auto roles_remove = (td::uint32)(key + 256 + 32).get_uint(32);
      block::gen::ValRegistryVoting::Record rec;
      if (!block::gen::csr_unpack(value, rec)) {
        VLOG(validator, WARNING) << "Invalid validator registry voting " << adnl_id << " " << roles_add << " "
                                 << roles_remove;
        return true;
      }
      f(VotingKey{adnl_id, roles_add, roles_remove}, Voting{.ends_at = rec.ends_at,
                                                            .created_lt = rec.created_lt,
                                                            .votes = rec.votes,
                                                            .validator_set = rec.validator_set});
      return true;
    });
  } catch (vm::VmError& e) {
    VLOG(validator, WARNING) << "Dictionary error in validator registry state: " << e.get_msg();
  }
}

void ValidatorRegistryState::scan_votings_diff(ValidatorRegistryState& prev, ValidatorRegistryState& cur,
                                               std::function<void(VotingKey, std::optional<Voting>)> f) {
  try {
    auto cur_dict = cur.get_votings_dict();
    prev.get_votings_dict().scan_diff(
        cur_dict, [&](td::ConstBitPtr key, int key_len, Ref<vm::CellSlice>, Ref<vm::CellSlice> cs_new) {
          CHECK(key_len == 256 + 32 + 32);
          adnl::AdnlNodeIdShort id{td::Bits256{key}};
          auto roles_add = (td::uint32)(key + 256).get_uint(32);
          auto roles_remove = (td::uint32)(key + 256 + 32).get_uint(32);
          std::optional<Voting> voting;
          if (cs_new.not_null()) {
            block::gen::ValRegistryVoting::Record rec;
            if (block::gen::csr_unpack(cs_new, rec)) {
              voting = Voting{.ends_at = rec.ends_at,
                              .created_lt = rec.created_lt,
                              .votes = rec.votes,
                              .validator_set = rec.validator_set};
            } else {
              VLOG(validator, WARNING) << "Invalid validator registry voting " << id << " " << roles_add << " "
                                       << roles_remove;
            }
          }
          f(VotingKey{id, roles_add, roles_remove}, std::move(voting));
          return true;
        });
  } catch (vm::VmError& e) {
    VLOG(validator, WARNING) << "Dictionary error in validator registry state: " << e.get_msg();
  }
}

td::StringBuilder& operator<<(td::StringBuilder& sb, const ValidatorRegistryState::VotingKey& key) {
  return sb << "id=" << key.adnl_id << " add=" << key.roles_add << " remove=" << key.roles_remove;
}

namespace {

class ValidatorRegistryWatcherImpl : public ValidatorRegistryWatcher {
 public:
  explicit ValidatorRegistryWatcherImpl(td::actor::ActorId<ValidatorManager> manager,
                                        td::actor::ActorId<keyring::Keyring> keyring)
      : manager_(manager), keyring_(keyring) {
  }

  void add_validator_key(PublicKeyHash key_hash) override {
    local_validators_.emplace(key_hash, LocalValidator{});
    update_is_current_validator();
    for (auto& [_, to_vote] : to_vote_) {
      to_vote.update_at = td::Timestamp::now();
    }
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
  std::unique_ptr<ValidatorRegistryState> registry_state_;
  Ref<CollatorsList> collators_list_;
  Ref<vm::Cell> new_entry_cell_;

  struct ToVote {
    UnixTime ends_at;
    LogicalTime created_lt;
    std::shared_ptr<block::TotalValidatorSet> validator_set;
    vm::CellSlice votes;
    td::Timestamp update_at = td::Timestamp::now();
  };
  std::map<ValidatorRegistryState::VotingKey, ToVote> to_vote_;
  Ref<RegistryVotingConfig> voting_config_;

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

  void update_legacy(Ref<ValidatorManagerOptions> opts);
  void update_local_validator_legacy(PublicKeyHash key_hash, LocalValidator& validator);
  Ref<vm::Cell> make_entry_cell();

  td::actor::Task<> sign_and_send_request(StdSmcAddress addr, PublicKeyHash key_hash, Ref<vm::Cell> request_cell,
                                          bool new_format = false);
  td::actor::Task<> send_external_message(StdSmcAddress addr, vm::CellSlice body);
};

void ValidatorRegistryWatcherImpl::update(Ref<MasterchainState> mc_state, Ref<ValidatorManagerOptions> opts) {
  mc_state_ = mc_state;
  auto new_registry_state = ValidatorRegistryState::fetch(mc_state_);
  if (inited_ && voting_config_.not_null()) {
    ValidatorRegistryState::scan_votings_diff(
        *registry_state_, *new_registry_state,
        [&](ValidatorRegistryState::VotingKey key, std::optional<ValidatorRegistryState::Voting> voting) {
          if (!voting_config_->vote_for(key.adnl_id, key.roles_add, key.roles_remove)) {
            return;
          }
          if (!voting) {
            to_vote_.erase(key);
            return;
          }
          auto [it, added] = to_vote_.emplace(key, ToVote{});
          auto& to_vote = it->second;
          if (added || to_vote.created_lt != voting->created_lt) {
            auto r_validator_set = block::Config::unpack_validator_set(voting->validator_set, true);
            if (r_validator_set.is_error()) {
              VLOG(validator, WARNING) << "Failed to unpack validator set for voting " << key << ": "
                                       << r_validator_set.move_as_error();
              to_vote_.erase(it);
              return;
            }
            to_vote.created_lt = voting->created_lt;
            to_vote.ends_at = voting->ends_at;
            to_vote.validator_set = r_validator_set.move_as_ok();
            VLOG(validator, INFO) << "Found new voting " << key;
          }
          to_vote.votes = vm::load_cell_slice(voting->votes);
          to_vote.update_at = td::Timestamp::now();
        });
  }
  registry_state_ = std::move(new_registry_state);
  if (mc_state_->is_key_state() || !inited_) {
    update_is_current_validator();
  }
  inited_ = true;

  if (voting_config_ != opts->get_registry_voting_config()) {
    voting_config_ = opts->get_registry_voting_config();
    to_vote_.clear();
    for (auto& [adnl_id, roles] : voting_config_->nodes) {
      registry_state_->for_each_voting_for_adnl_id(
          adnl_id, [&](ValidatorRegistryState::VotingKey key, ValidatorRegistryState::Voting voting) {
            if (!voting_config_->vote_for(key.adnl_id, key.roles_add, key.roles_remove)) {
              return;
            }
            auto r_validator_set = block::Config::unpack_validator_set(voting.validator_set, true);
            if (r_validator_set.is_error()) {
              VLOG(validator, WARNING) << "Failed to unpack validator set for voting " << key << ": "
                                       << r_validator_set.move_as_error();
              return;
            }
            auto& to_vote = to_vote_[key];
            to_vote.created_lt = voting.created_lt;
            to_vote.ends_at = voting.ends_at;
            to_vote.validator_set = r_validator_set.move_as_ok();
            to_vote.votes = vm::load_cell_slice(voting.votes);
            VLOG(validator, INFO) << "Found new voting " << key;
          });
    }
  }

  for (auto it = to_vote_.begin(); it != to_vote_.end();) {
    auto& [key, to_vote] = *it;
    if (to_vote.ends_at <= (UnixTime)td::Clocks::system()) {
      VLOG(validator, INFO) << "Voting " << key << " expired";
      it = to_vote_.erase(it);
      continue;
    }
    if (!to_vote.update_at || !to_vote.update_at.is_in_past()) {
      ++it;
      continue;
    }
    bool voted = false;
    for (unsigned idx = 0; idx < to_vote.validator_set->list.size(); ++idx) {
      if (to_vote.votes.size() <= idx || to_vote.votes.bit_at(idx)) {
        continue;
      }
      PublicKeyHash key_hash = PublicKey{pubkeys::Ed25519{to_vote.validator_set->list[idx].pubkey}}.compute_short_id();
      if (!local_validators_.contains(key_hash)) {
        continue;
      }
      VLOG(validator, INFO) << "Voting for " << key;
      block::gen::ValRegistryRequest::Record request{.adnl_id = key.adnl_id.bits256_value(),
                                                     .roles_add = key.roles_add,
                                                     .roles_remove = key.roles_remove,
                                                     .validator_idx = (int)idx,
                                                     .created_lt = to_vote.created_lt,
                                                     .valid_until = (UnixTime)td::Clocks::system() + 60};
      Ref<vm::Cell> request_cell;
      CHECK(block::gen::pack_cell(request_cell, request));
      sign_and_send_request(registry_state_->get_config().contract_address, key_hash, request_cell, true)
          .start()
          .detach("send vote");
      voted = true;
    }
    if (voted) {
      to_vote.update_at = td::Timestamp::in(60.0);
    } else {
      to_vote.update_at = td::Timestamp::never();
    }
  }

  update_legacy(std::move(opts));
}

void ValidatorRegistryWatcherImpl::update_legacy(Ref<ValidatorManagerOptions> opts) {
  auto registry_state_legacy = dynamic_cast<ValidatorRegistryStateImplLegacy*>(registry_state_.get());
  if (!registry_state_legacy || local_validators_.empty()) {
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
    update_local_validator_legacy(id, validator);
  }

  auto vset = mc_state_->get_total_validator_set(0);
  if (is_current_validator_ && vset.not_null() &&
      registry_state_legacy->get_last_cleanup_key_block_seqno() < mc_state_->last_key_block_id().seqno() &&
      td::Random::fast(1, (int)vset->size()) == 1 && try_cleanup_at_.is_in_past()) {
    VLOG(validator, INFO) << "Update registry: cleanup";
    send_external_message(
        registry_state_->get_config().contract_address,
        vm::CellBuilder{}.store_long(block::gen::ValRegistryMessageCleanup::cons_tag[0], 32).as_cellslice())
        .start()
        .detach("send registry cleanup");
    try_cleanup_at_ = td::Timestamp::in(60.0);
  }
}

void ValidatorRegistryWatcherImpl::update_local_validator_legacy(PublicKeyHash key_hash, LocalValidator& validator) {
  auto registry_state_legacy = dynamic_cast<ValidatorRegistryStateImplLegacy*>(registry_state_.get());
  if (!registry_state_legacy) {
    return;
  }
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
  auto r_info = registry_state_legacy->get_validator_info(public_key);
  if (r_info.is_error()) {
    VLOG(validator, ERROR) << "Update registry: failed to get validator info from registry: " << r_info.move_as_error();
    validator.update_at.relax(td::Timestamp::in(3600.0));
    return;
  }
  ValidatorRegistryStateImplLegacy::ValidatorInfo info = r_info.move_as_ok();
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
  block::gen::ValRegistryRequestOld::Record request{.last_key_block_seqno = mc_state_->last_key_block_id().seqno(),
                                                    .val_set = (int)val_set_idx,
                                                    .val_idx = (int)val_idx,
                                                    .valid_until = (UnixTime)td::Clocks::system() + 60,
                                                    .new_entry = cb.as_cellslice_ref()};
  Ref<vm::Cell> request_cell;
  CHECK(block::gen::pack_cell(request_cell, request));
  sign_and_send_request(registry_state_legacy->get_config().contract_address, key_hash, std::move(request_cell))
      .start()
      .detach("send registry request");
  validator.update_at.relax(td::Timestamp::in(60.0));
}

Ref<vm::Cell> ValidatorRegistryWatcherImpl::make_entry_cell() {
  const auto& config = registry_state_->get_config();
  std::set<adnl::AdnlNodeIdShort> collators;
  collators.insert(collators_list_->register_collators.begin(), collators_list_->register_collators.end());
  if (collators.size() > config.legacy_max_collators_per_validator) {
    VLOG(validator, WARNING) << "Make entry cell: too many collators, pruning (max. "
                             << config.legacy_max_collators_per_validator << ", found " << collators.size() << ")";
    while (collators.size() > config.legacy_max_collators_per_validator) {
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
                                                                      Ref<vm::Cell> request_cell, bool new_format) {
  td::BufferSlice to_sign{request_cell->get_hash().as_slice()};
  td::BufferSlice signature =
      co_await td::actor::ask(keyring_, &keyring::Keyring::sign_message, key_hash, std::move(to_sign));
  vm::CellBuilder cb;
  if (new_format) {
    cb.store_long(block::gen::ValRegistryMessageSignedRequest::cons_tag[0], 32);
    cb.store_bytes(signature);
    cb.store_ref(request_cell);
  } else {
    cb.store_long(block::gen::ValRegistryMessageRequest::cons_tag[0], 32);
    cb.store_bytes(signature);
    cb.append_cellslice(vm::load_cell_slice(request_cell));
  }
  co_await send_external_message(addr, cb.as_cellslice());
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
  return ValidatorRegistryState::fetch(mc_state)->get_all_collators(mc_state);
}

td::actor::ActorOwn<ValidatorRegistryWatcher> ValidatorRegistryWatcher::create(
    td::actor::ActorId<ValidatorManager> manager, td::actor::ActorId<keyring::Keyring> keyring) {
  return td::actor::create_actor<ValidatorRegistryWatcherImpl>("ValidatorRegistry", std::move(manager),
                                                               std::move(keyring));
}

}  // namespace ton::validator
