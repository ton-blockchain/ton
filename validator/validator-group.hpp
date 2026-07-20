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

    Copyright 2017-2020 Telegram Systems LLP
*/
#pragma once

#include <memory>
#include <optional>

#include "interfaces/validator-manager.h"

namespace ton {

namespace validator {

class CollatorScoreboard;
class ValidatorManager;

struct GroupIdentity {
  adnl::AdnlNodeIdShort adnl_id;
  std::optional<PublicKeyHash> short_id = std::nullopt;
  bool is_collator = false;

  std::strong_ordering operator<=>(const GroupIdentity&) const = default;

  bool is_validator() const {
    return short_id.has_value();
  }
};

struct GroupParams {
  ShardIdFull shard;
  td::actor::ActorId<ValidatorManager> manager;
  td::actor::ActorId<keyring::Keyring> keyring;
  td::Ref<ValidatorManagerOptions> validator_opts;

  td::Ref<block::ValidatorSet> validator_set;
  GroupIdentity identity;

  NewConsensusConfig config;

  ValidatorSessionId session_id;
  td::actor::ActorId<overlay::Overlays> overlays;
  td::actor::ActorId<adnl::AdnlSenderEx> adnl_sender;
  std::string db_root;

  std::vector<adnl::AdnlNodeIdShort> all_overlay_nodes;
  td::actor::ActorId<CollatorScoreboard> collator_scoreboard;
};

class IValidatorGroup : public td::actor::Actor {
 public:
  static td::actor::ActorOwn<IValidatorGroup> create_bridge(td::Slice name, GroupParams params);

  virtual void start(std::vector<BlockIdExt> prev, BlockIdExt min_masterchain_block_id) = 0;

  virtual void update_options(td::Ref<ValidatorManagerOptions> opts, bool apply_blocks) = 0;

  virtual void notify_mc_finalized(BlockIdExt block) = 0;

  virtual void destroy() = 0;
};

struct ManagerContext {
  td::actor::ActorId<ValidatorManager> manager;
  td::Ref<ValidatorManagerOptions> opts;
  td::actor::ActorId<keyring::Keyring> keyring;
  td::actor::ActorId<overlay::Overlays> overlays;
  td::actor::ActorId<adnl::AdnlSenderEx> quic;
  std::string db_root;

  std::set<PublicKeyHash> validator_keys;
  std::set<adnl::AdnlNodeIdShort> local_collator_adnl_ids;
  td::actor::ActorId<CollatorScoreboard> collator_scoreboard;
};

struct ValidatorGroupCount {
  size_t masterchain = 0;
  size_t shard = 0;
};

class NetworkState {
 public:
  virtual ~NetworkState() = default;

  static std::unique_ptr<NetworkState> create(BlockSeqno start_seqno, td::Ref<MasterchainState> previous_rotation);

  virtual void update(td::Ref<MasterchainState> state, ManagerContext ctx) = 0;
  virtual void update_options(td::Ref<ValidatorManagerOptions> opts) = 0;

  virtual ValidatorGroupCount validator_group_count() const = 0;
};

}  // namespace validator

}  // namespace ton
