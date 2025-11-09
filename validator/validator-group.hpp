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

#include <list>

#include "interfaces/validator-manager.h"
#include "rldp/rldp.h"
#include "rldp2/rldp-utils.h"
#include "rldp2/rldp.h"
#include "validator-session/validator-session.h"

#include "collation-manager.hpp"

namespace ton {

namespace validator {

class ValidatorManager;

class IValidatorGroup : public td::actor::Actor {
 public:
  static td::actor::ActorOwn<IValidatorGroup> create_catchain(
      td::Slice name, ShardIdFull shard, PublicKeyHash local_id, ValidatorSessionId session_id,
      td::Ref<ValidatorSet> validator_set, BlockSeqno last_key_block_seqno,
      validatorsession::ValidatorSessionOptions config, td::actor::ActorId<keyring::Keyring> keyring,
      td::actor::ActorId<adnl::Adnl> adnl, td::actor::ActorId<rldp::Rldp> rldp, td::actor::ActorId<rldp2::Rldp> rldp2,
      td::actor::ActorId<overlay::Overlays> overlays, std::string db_root,
      td::actor::ActorId<ValidatorManager> validator_manager, td::actor::ActorId<CollationManager> collation_manager,
      bool create_session, bool allow_unsafe_self_blocks_resync, td::Ref<ValidatorManagerOptions> opts,
      bool monitoring_shard);

  virtual void start(std::vector<BlockIdExt> prev, BlockIdExt min_masterchain_block_id) = 0;
  virtual void create_session() = 0;

  virtual void update_options(td::Ref<ValidatorManagerOptions> opts, bool apply_blocks) = 0;

  virtual void get_validator_group_info_for_litequery(
      td::Promise<tl_object_ptr<lite_api::liteServer_nonfinal_validatorGroupInfo>> promise) = 0;

  virtual void destroy() = 0;
};

}  // namespace validator

}  // namespace ton
