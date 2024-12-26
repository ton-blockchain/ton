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
#pragma once

#include "interfaces/validator-manager.h"
#include "rldp/rldp.h"
#include <map>

namespace ton::validator {

class ValidatorManager;

class CollationManager : public td::actor::Actor {
 public:
  CollationManager(adnl::AdnlNodeIdShort local_id, td::Ref<ValidatorManagerOptions> opts,
                   td::actor::ActorId<ValidatorManager> manager, td::actor::ActorId<rldp::Rldp> rldp)
      : local_id_(local_id), opts_(opts), manager_(manager), rldp_(rldp) {
  }

  void start_up() override;
  void alarm() override;

  void collate_block(ShardIdFull shard, BlockIdExt min_masterchain_block_id, std::vector<BlockIdExt> prev,
                     Ed25519_PublicKey creator, BlockCandidatePriority priority,
                     td::Ref<ValidatorSet> validator_set, td::uint64 max_answer_size,
                     td::CancellationToken cancellation_token, td::Promise<GeneratedCandidate> promise);

  void update_options(td::Ref<ValidatorManagerOptions> opts);

  void validator_group_started(ShardIdFull shard);
  void validator_group_finished(ShardIdFull shard);

  void get_stats(td::Promise<tl_object_ptr<ton_api::engine_validator_collationManagerStats_localId>> promise);

 private:
  adnl::AdnlNodeIdShort local_id_;
  td::Ref<ValidatorManagerOptions> opts_;
  td::actor::ActorId<ValidatorManager> manager_;
  td::actor::ActorId<rldp::Rldp> rldp_;

  void collate_shard_block(ShardIdFull shard, BlockIdExt min_masterchain_block_id, std::vector<BlockIdExt> prev,
                           Ed25519_PublicKey creator, BlockCandidatePriority priority,
                           td::Ref<ValidatorSet> validator_set, td::uint64 max_answer_size,
                           td::CancellationToken cancellation_token, td::Promise<GeneratedCandidate> promise,
                           td::Timestamp timeout);

  void update_collators_list(const CollatorsList& collators_list);

  struct CollatorInfo {
    bool alive = false;
    td::Timestamp ping_at = td::Timestamp::now();
    bool sent_ping = false;
    size_t active_cnt = 0;
    td::Timestamp last_ping_at = td::Timestamp::never();
    td::Status last_ping_status = td::Status::Error("not pinged");
  };
  std::map<adnl::AdnlNodeIdShort, CollatorInfo> collators_;

  struct ShardInfo {
    ShardIdFull shard_id;
    CollatorsList::SelectMode select_mode;
    std::vector<adnl::AdnlNodeIdShort> collators;
    bool self_collate = false;
    size_t cur_idx = 0;

    size_t active_cnt = 0;
  };
  std::vector<ShardInfo> shards_;

  std::map<ShardIdFull, size_t> active_validator_groups_;

  ShardInfo* select_shard_info(ShardIdFull shard);
  void got_pong(adnl::AdnlNodeIdShort id, td::Result<td::BufferSlice> R);
  void on_collate_query_error(adnl::AdnlNodeIdShort id);
};

}  // namespace ton::validator
