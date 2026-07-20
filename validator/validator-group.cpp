/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include <array>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

#include "interfaces/shard.h"
#include "interfaces/validator-full-id.h"
#include "keys/keys.hpp"
#include "td/utils/Time.h"
#include "ton/ton-io.hpp"
#include "ton/ton-types.h"

#include "validator-group.hpp"
#include "validator-registry-watcher.hpp"

namespace ton::validator {

namespace tl {

using groupNew = ton_api::validator_groupNew;

}

namespace {

struct Genesis {
  std::vector<BlockIdExt> blocks;
  BlockIdExt min_mc_block_id;
};

struct SessionInfo {
  ShardIdFull shard;
  td::Ref<block::ValidatorSet> validator_set;
  ValidatorSessionId session_id;
  NewConsensusConfig config;
  std::vector<adnl::AdnlNodeIdShort> overlay_members;
  std::vector<GroupIdentity> identities;

  CatchainSeqno cc_seqno() const {
    return validator_set->get_catchain_seqno();
  }
};

struct OverlayMembers {
  std::vector<adnl::AdnlNodeIdShort> all;
  std::set<adnl::AdnlNodeIdShort> local;
};

OverlayMembers overlay_members_of(const MasterchainState &state, const ManagerContext &deps,
                                  const std::vector<adnl::AdnlNodeIdShort> &all_collators) {
  OverlayMembers members;
  members.all = all_collators;

  for (int i = -1; i <= 1; ++i) {
    auto vset = state.get_total_validator_set(i);
    if (vset.is_null()) {
      continue;
    }
    for (const auto &val : vset->export_vector()) {
      PublicKeyHash key_hash = ValidatorFullId{val.key}.compute_short_id();
      adnl::AdnlNodeIdShort adnl_id{val.addr.is_zero() ? key_hash.bits256_value() : val.addr};
      members.all.push_back(adnl_id);
      if (deps.validator_keys.contains(key_hash)) {
        members.local.insert(adnl_id);
      }
    }
  }
  for (const auto &id : all_collators) {
    if (deps.local_collator_adnl_ids.contains(id)) {
      members.local.insert(id);
    }
  }

  std::sort(members.all.begin(), members.all.end());
  members.all.erase(std::unique(members.all.begin(), members.all.end()), members.all.end());

  return members;
}

struct Context {
  const ManagerContext &deps;
  const MasterchainState &state;
  OverlayMembers overlay_members;
  td::uint32 unsafe_rotate_id = 0;
  bool should_manage_groups = false;
};

std::vector<GroupIdentity> identities_for(const Context &ctx, const td::Ref<block::ValidatorSet> &val_set) {
  std::vector<GroupIdentity> identities;
  std::set<adnl::AdnlNodeIdShort> group_validator_adnl_ids;

  for (auto key : ctx.deps.validator_keys) {
    if (auto validator = val_set->get_validator(key.bits256_value())) {
      PublicKeyHash key_hash = ValidatorFullId{validator->key}.compute_short_id();
      adnl::AdnlNodeIdShort adnl_id{validator->addr.is_zero() ? key_hash.bits256_value() : validator->addr};

      group_validator_adnl_ids.insert(adnl_id);
      identities.push_back({
          .adnl_id = adnl_id,
          .short_id = key_hash,
          .is_collator = false,
      });
    }
  }

  if (identities.size() > 1) {
    LOG(ERROR) << "Multiple known validator keys are in an active validator set. This is unsupported.";
  }

  for (auto adnl_id : ctx.overlay_members.local) {
    if (!group_validator_adnl_ids.contains(adnl_id)) {
      identities.push_back({
          .adnl_id = adnl_id,
          .short_id = std::nullopt,
          .is_collator = ctx.deps.local_collator_adnl_ids.contains(adnl_id),
      });
    }
  }

  return identities;
}

ValidatorSessionId session_id_for(const Context &ctx, ShardIdFull shard, const td::Ref<block::ValidatorSet> &val_set) {
  std::vector<tl_object_ptr<ton_api::validator_groupMember>> vec;
  for (auto &n : val_set->export_vector()) {
    auto pub = PublicKey{pubkeys::Ed25519{n.key}};
    vec.push_back(
        create_tl_object<ton_api::validator_groupMember>(pub.compute_short_id().bits256_value(), n.addr, n.weight));
  }

  Bits256 opts_hash = {{10, 91,  242, 57, 159, 23,  47,  238, 90,  142, 120, 111, 85, 169, 210, 113,
                        73, 209, 237, 51, 230, 184, 224, 204, 129, 239, 69,  250, 59, 140, 184, 215}};
  if (ctx.unsafe_rotate_id != 0) {
    opts_hash.set_zero();
    std::memcpy(opts_hash.as_slice().data(), &ctx.unsafe_rotate_id, sizeof(ctx.unsafe_rotate_id));
  }

  return create_hash_tl_object<tl::groupNew>(shard.workchain, shard.shard, ctx.deps.opts->get_maximal_vertical_seqno(),
                                             ctx.state.last_key_block_id().seqno(), val_set->get_catchain_seqno(),
                                             opts_hash, std::move(vec));
}

SessionInfo session_info(const Context &ctx, ShardIdFull shard, td::Ref<block::ValidatorSet> validator_set) {
  auto config = ctx.state.get_new_consensus_config(shard.workchain);

  return {
      .shard = shard,
      .validator_set = validator_set,
      .session_id = session_id_for(ctx, shard, validator_set),
      .config = config,
      .overlay_members = ctx.overlay_members.all,
      .identities = identities_for(ctx, validator_set),
  };
}

td::actor::ActorOwn<IValidatorGroup> make_group(const Context &ctx, const SessionInfo &info,
                                                const GroupIdentity &identity) {
  GroupParams params{
      .shard = info.shard,
      .manager = ctx.deps.manager,
      .keyring = ctx.deps.keyring,
      .validator_opts = ctx.deps.opts,
      .validator_set = info.validator_set,
      .identity = identity,
      .config = info.config,
      .session_id = info.session_id,
      .overlays = ctx.deps.overlays,
      .adnl_sender = ctx.deps.quic,
      .db_root = ctx.deps.db_root,
      .all_overlay_nodes = info.overlay_members,
      .collator_scoreboard = ctx.deps.collator_scoreboard,
  };
  return IValidatorGroup::create_bridge(PSTRING() << "valgroup" << info.shard, params);
}

std::map<ShardIdFull, std::vector<BlockIdExt>> masterchain_target(const MasterchainState &state) {
  return {{ShardIdFull{masterchainId, shardIdAll}, {state.get_block_id()}}};
}

std::map<ShardIdFull, std::vector<BlockIdExt>> basechain_target(const MasterchainState &state) {
  std::map<ShardIdFull, std::vector<BlockIdExt>> target;
  for (auto &descr : state.get_shards()) {
    auto shard = descr->shard();
    if (descr->before_split()) {
      ShardIdFull l{shard.workchain, shard_child(shard.shard, true)};
      ShardIdFull r{shard.workchain, shard_child(shard.shard, false)};
      target[l] = {descr->top_block_id()};
      target[r] = {descr->top_block_id()};
    } else if (descr->before_merge()) {
      ShardIdFull p{shard.workchain, shard_parent(shard.shard)};
      auto &blocks = target[p];
      if (blocks.empty()) {
        blocks.resize(2);
      }
      bool left = shard_child(p.shard, true) == shard.shard;
      blocks[left ? 0 : 1] = descr->top_block_id();
    } else {
      target[shard] = {descr->top_block_id()};
    }
  }
  return target;
}

std::set<ShardIdFull> masterchain_future_shards(const MasterchainState &state) {
  return {ShardIdFull{masterchainId, shardIdAll}};
}

std::set<ShardIdFull> basechain_future_shards(const MasterchainState &state) {
  using namespace std::chrono_literals;

  std::set<ShardIdFull> shards;
  auto now = td::UTCClock::now();
  for (auto &descr : state.get_shards()) {
    auto shard = descr->shard();
    switch (descr->fsm_state()) {
      case McShardHash::FsmState::fsm_split:
        if (descr->fsm_utime_chrono() < now + 60s) {
          shards.insert(ShardIdFull{shard.workchain, shard_child(shard.shard, true)});
          shards.insert(ShardIdFull{shard.workchain, shard_child(shard.shard, false)});
        } else {
          shards.insert(shard);
        }
        break;
      case McShardHash::FsmState::fsm_merge:
        if (descr->fsm_utime_chrono() < now + 60s) {
          shards.insert(ShardIdFull{shard.workchain, shard_parent(shard.shard)});
        } else {
          shards.insert(shard);
        }
        break;
      default:
        shards.insert(shard);
    }
  }
  return shards;
}

struct Group {
  ShardIdFull shard;
  CatchainSeqno cc_seqno = 0;
  ValidatorSessionId session_id;
  std::map<GroupIdentity, td::actor::ActorOwn<IValidatorGroup>> actors;

  Group() = default;
  Group(Group &&) = default;
  Group &operator=(Group &&) = default;

  std::string name() const {
    return PSTRING() << "validator group " << shard << "." << cc_seqno << ":" << session_id;
  }

  // FIXME: destroy() wipes the per-session consensus DB, which is correct when a session is
  // superseded but not when NetworkState itself is destroyed with sessions still active.
  ~Group() {
    if (!actors.empty()) {
      LOG(INFO) << "Destroying " << name();
    }
    for (auto &[identity, actor] : actors) {
      auto id = actor.release();
      td::actor::send_closure(id, &IValidatorGroup::destroy);
    }
  }

  void reconcile(const Context &ctx, const SessionInfo &info, const std::optional<Genesis> &start_genesis) {
    shard = info.shard;
    cc_seqno = info.cc_seqno();
    session_id = info.session_id;
    for (auto &identity : info.identities) {
      if (actors.contains(identity)) {
        continue;
      }
      auto actor = make_group(ctx, info, identity);
      if (start_genesis) {
        td::actor::send_closure(actor.get(), &IValidatorGroup::start, start_genesis->blocks,
                                start_genesis->min_mc_block_id);
      }
      actors.emplace(identity, std::move(actor));
    }

    BlockIdExt finalized_block;
    if (shard.is_masterchain()) {
      finalized_block = ctx.state.get_block_id();
    } else {
      auto descr = ctx.state.get_shard_from_config(shard);
      if (descr.is_null()) {
        return;
      }
      finalized_block = descr->top_block_id();
    }
    for (const auto &[_, actor] : actors) {
      td::actor::send_closure(actor, &IValidatorGroup::notify_mc_finalized, finalized_block);
    }
  }

  void start_all(const Genesis &genesis) {
    for (auto &[identity, actor] : actors) {
      td::actor::send_closure(actor.get(), &IValidatorGroup::start, genesis.blocks, genesis.min_mc_block_id);
    }
  }

  bool has_validator() const {
    for (auto &[identity, actor] : actors) {
      if (identity.is_validator()) {
        return true;
      }
    }
    return false;
  }

  void update_options(td::Ref<ValidatorManagerOptions> opts) {
    for (auto &[identity, actor] : actors) {
      td::actor::send_closure(actor.get(), &IValidatorGroup::update_options, opts, true);
    }
  }
};

class GroupSlot {
 public:
  GroupSlot(ShardIdFull shard) : shard_(shard) {
  }

  bool active() const {
    return groups_.has_value();
  }

  ShardIdFull shard() const {
    return shard_;
  }

  CatchainSeqno cc_seqno() const {
    return groups_ ? groups_->cc_seqno : 0;
  }

  bool has_validator() const {
    return groups_ && groups_->has_validator();
  }

  void update_options(td::Ref<ValidatorManagerOptions> opts) {
    if (groups_) {
      groups_->update_options(opts);
    }
  }

  void reconcile(const Context &ctx, const Genesis &genesis, std::map<ValidatorSessionId, Group> &future) {
    auto val_set = ctx.state.get_validator_set(shard_);
    if (val_set.is_null()) {
      drop();
      return;
    }
    auto info = session_info(ctx, shard_, val_set);
    if (groups_ && groups_->session_id != info.session_id) {
      drop();
    }
    if (info.identities.empty()) {
      return;
    }

    bool fresh = !groups_;
    if (fresh) {
      if (auto it = future.find(info.session_id); it != future.end()) {
        groups_ = std::move(it->second);
        future.erase(it);
        groups_->start_all(genesis);
      } else {
        groups_ = Group{};
      }
    }

    groups_->reconcile(ctx, info, genesis);

    if (fresh) {
      LOG(INFO) << "Started " << groups_->name();
    }
  }

  void drop() {
    groups_.reset();
  }

 private:
  ShardIdFull shard_;
  std::optional<Group> groups_;
};

class ShardTree {
 public:
  struct ActiveGroup {
    ShardIdFull shard;
    CatchainSeqno seqno;
  };

  explicit ShardTree(ShardIdFull shard) : shard_(shard), slot_(shard) {
  }

  void update(const Context &ctx, const std::map<ShardIdFull, std::vector<BlockIdExt>> &target,
              std::map<ValidatorSessionId, Group> &future) {
    if (auto it = target.find(shard_); it != target.end()) {
      become_leaf(ctx, it->second, future);
    } else {
      become_internal(ctx, target, future);
    }
  }

  void collect_active(std::vector<ActiveGroup> &out) const {
    if (children_) {
      (*children_)[0]->collect_active(out);
      (*children_)[1]->collect_active(out);
    } else if (slot_.active()) {
      out.emplace_back(slot_.shard(), slot_.cc_seqno());
    }
  }

  size_t count_validator_groups() const {
    if (children_) {
      return (*children_)[0]->count_validator_groups() + (*children_)[1]->count_validator_groups();
    }
    return slot_.has_validator() ? 1 : 0;
  }

  void update_options(td::Ref<ValidatorManagerOptions> opts) {
    slot_.update_options(opts);
    if (children_) {
      (*children_)[0]->update_options(opts);
      (*children_)[1]->update_options(opts);
    }
  }

 private:
  void become_leaf(const Context &ctx, const std::vector<BlockIdExt> &blocks,
                   std::map<ValidatorSessionId, Group> &future) {
    children_.reset();

    auto val_set = ctx.state.get_validator_set(shard_);
    CHECK(val_set.not_null());

    auto session_id = session_id_for(ctx, shard_, val_set);
    if (!genesis_ || session_ != session_id) {
      genesis_ = Genesis{blocks, ctx.state.get_block_id()};
    }
    session_ = session_id;

    if (ctx.should_manage_groups) {
      slot_.reconcile(ctx, *genesis_, future);
    }
  }

  void become_internal(const Context &ctx, const std::map<ShardIdFull, std::vector<BlockIdExt>> &target,
                       std::map<ValidatorSessionId, Group> &future) {
    slot_.drop();
    genesis_.reset();
    session_.reset();
    if (!children_) {
      children_.emplace();
      (*children_)[0] = std::make_unique<ShardTree>(ShardIdFull{shard_.workchain, shard_child(shard_.shard, true)});
      (*children_)[1] = std::make_unique<ShardTree>(ShardIdFull{shard_.workchain, shard_child(shard_.shard, false)});
    }
    (*children_)[0]->update(ctx, target, future);
    (*children_)[1]->update(ctx, target, future);
  }

  ShardIdFull shard_;
  std::optional<std::array<std::unique_ptr<ShardTree>, 2>> children_;
  std::optional<Genesis> genesis_;
  std::optional<ValidatorSessionId> session_;
  GroupSlot slot_;
};

class WorkchainState {
 public:
  explicit WorkchainState(WorkchainId workchain)
      : workchain_(workchain), tree_(std::make_unique<ShardTree>(ShardIdFull{workchain, shardIdAll})) {
  }

  void update(const Context &ctx, const std::map<ShardIdFull, std::vector<BlockIdExt>> &target,
              const std::set<ShardIdFull> &future_shards) {
    tree_->update(ctx, target, future_);
    if (ctx.should_manage_groups) {
      update_future(ctx, future_shards);
    }
  }

  size_t count_validator_groups() const {
    return tree_->count_validator_groups();
  }

  void update_options(td::Ref<ValidatorManagerOptions> opts) {
    tree_->update_options(opts);
    for (auto &[session_id, group] : future_) {
      group.update_options(opts);
    }
  }

 private:
  void update_future(const Context &ctx, const std::set<ShardIdFull> &future_shards) {
    if (ctx.state.rotated_all_shards()) {
      future_.clear();
      return;
    }
    for (auto &shard : future_shards) {
      auto val_set = ctx.state.get_next_validator_set(shard);
      if (val_set.is_null()) {
        continue;
      }
      auto info = session_info(ctx, shard, val_set);
      if (info.identities.empty()) {
        continue;
      }

      bool is_new = !future_.contains(info.session_id);
      auto &group = future_[info.session_id];
      group.reconcile(ctx, info, std::nullopt);
      if (is_new) {
        LOG(INFO) << "Created tentative " << group.name();
      }
    }

    std::vector<ShardTree::ActiveGroup> active;
    tree_->collect_active(active);
    for (auto it = future_.begin(); it != future_.end();) {
      const Group &group = it->second;
      bool superseded = false;
      for (auto &[active_shard, active_seqno] : active) {
        bool equal = active_shard.shard == group.shard.shard;
        bool related = shard_is_ancestor(active_shard.shard, group.shard.shard) ||
                       shard_is_ancestor(group.shard.shard, active_shard.shard);
        if ((active_seqno >= group.cc_seqno && equal) || (active_seqno > group.cc_seqno && related)) {
          superseded = true;
          break;
        }
      }
      if (superseded) {
        it = future_.erase(it);
      } else {
        ++it;
      }
    }
  }

  WorkchainId workchain_;
  std::unique_ptr<ShardTree> tree_;
  std::map<ValidatorSessionId, Group> future_;
};

class NetworkStateImpl final : public NetworkState {
 public:
  explicit NetworkStateImpl(BlockSeqno start_seqno, td::Ref<MasterchainState> previous_rotation)
      : start_seqno_(start_seqno), masterchain_(masterchainId), basechain_(basechainId) {
    current_collators_ = ValidatorRegistryWatcher::get_all_collators(previous_rotation);
  }

  void update(td::Ref<MasterchainState> state_ref, ManagerContext deps) override {
    const MasterchainState &state = *state_ref;
    std::vector<adnl::AdnlNodeIdShort> next_collators;
    if (state.rotated_all_shards()) {
      genesis_known_ = true;
      next_collators = ValidatorRegistryWatcher::get_all_collators(state_ref);
    }
    if (state.is_key_state()) {
      CHECK(state.rotated_all_shards());
      current_collators_ = next_collators;
    }
    if (!genesis_known_) {
      return;
    }

    auto mc_val_set = state.get_validator_set(ShardIdFull{masterchainId});
    auto rotate_id = deps.opts->check_unsafe_catchain_rotate(state.get_seqno(), mc_val_set->get_catchain_seqno());

    auto overlay_members = overlay_members_of(state, deps, current_collators_);

    Context ctx{
        .deps = deps,
        .state = state,
        .overlay_members = overlay_members,
        .unsafe_rotate_id = rotate_id,
        .should_manage_groups = state.get_seqno() >= start_seqno_,
    };

    masterchain_.update(ctx, masterchain_target(state), masterchain_future_shards(state));
    if (state.get_seqno() != 0) {
      // FIXME: We can potentially require zerostates to have ShardHashes populated instead.
      basechain_.update(ctx, basechain_target(state), basechain_future_shards(state));
    }

    if (state.rotated_all_shards() && !state.is_key_state()) {
      current_collators_ = next_collators;
    }
  }

  void update_options(td::Ref<ValidatorManagerOptions> opts) override {
    masterchain_.update_options(opts);
    basechain_.update_options(opts);
  }

  ValidatorGroupCount validator_group_count() const override {
    return {
        .masterchain = masterchain_.count_validator_groups(),
        .shard = basechain_.count_validator_groups(),
    };
  }

 private:
  BlockSeqno start_seqno_;
  bool genesis_known_ = false;

  std::vector<adnl::AdnlNodeIdShort> current_collators_;

  WorkchainState masterchain_;
  WorkchainState basechain_;
};

}  // namespace

std::unique_ptr<NetworkState> NetworkState::create(BlockSeqno start_seqno,
                                                   td::Ref<MasterchainState> previous_rotation) {
  return std::make_unique<NetworkStateImpl>(start_seqno, previous_rotation);
}

}  // namespace ton::validator
