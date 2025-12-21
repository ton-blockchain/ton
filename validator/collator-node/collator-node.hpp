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

#include <map>
#include <optional>

#include "interfaces/validator-manager.h"
#include "rldp/rldp.h"
#include "rldp2/rldp.h"

#include "collator-node-session.hpp"

namespace ton::validator {

class ValidatorManager;

class CollatorNode : public td::actor::Actor {
 public:
  CollatorNode(adnl::AdnlNodeIdShort local_id, td::Ref<ValidatorManagerOptions> opts,
               td::actor::ActorId<ValidatorManager> manager, td::actor::ActorId<adnl::Adnl> adnl,
               td::actor::ActorId<rldp2::Rldp> rldp);
  void start_up() override;
  void tear_down() override;
  void add_shard(ShardIdFull shard);
  void del_shard(ShardIdFull shard);

  void update_options(td::Ref<ValidatorManagerOptions> opts);

  void new_masterchain_block_notification(td::Ref<MasterchainState> state);
  void update_shard_client_handle(BlockHandle shard_client_handle);
  void new_shard_block_accepted(BlockIdExt block_id, CatchainSeqno cc_seqno);

 private:
  void receive_query(adnl::AdnlNodeIdShort src, td::BufferSlice data, td::Promise<td::BufferSlice> promise);
  void process_generate_block_query(adnl::AdnlNodeIdShort src, ShardIdFull shard, CatchainSeqno cc_seqno,
                                    std::vector<BlockIdExt> prev_blocks, BlockCandidatePriority priority,
                                    bool is_optimistic, td::Timestamp timeout, td::Promise<BlockCandidate> promise);
  void process_ping(adnl::AdnlNodeIdShort src, ton_api::collatorNode_ping& ping, td::Promise<td::BufferSlice> promise);

  bool can_collate_shard(ShardIdFull shard) const;

  adnl::AdnlNodeIdShort local_id_;
  td::Ref<ValidatorManagerOptions> opts_;
  td::actor::ActorId<ValidatorManager> manager_;
  td::actor::ActorId<adnl::Adnl> adnl_;
  td::actor::ActorId<rldp2::Rldp> rldp_;
  std::vector<ShardIdFull> collating_shards_;
  std::set<adnl::AdnlNodeIdShort> validator_adnl_ids_;

  struct ValidatorGroupInfo {
    CatchainSeqno cc_seqno{0};
    std::vector<BlockIdExt> prev;
    td::actor::ActorOwn<CollatorNodeSession> actor;
  };
  struct FutureValidatorGroup {
    std::vector<BlockIdExt> pending_blocks;
    std::vector<td::Promise<td::Unit>> promises;
  };
  std::map<ShardIdFull, ValidatorGroupInfo> validator_groups_;
  std::map<std::pair<ShardIdFull, CatchainSeqno>, FutureValidatorGroup> future_validator_groups_;

  td::Ref<MasterchainState> last_masterchain_state_;
  BlockHandle shard_client_handle_;

  td::Status mc_config_status_ = td::Status::Error("not inited");
  BlockSeqno last_key_block_seqno_ = (BlockSeqno)-1;

  td::Result<FutureValidatorGroup*> get_future_validator_group(ShardIdFull shard, CatchainSeqno cc_seqno);

  td::Status check_out_of_sync();
  td::Status check_mc_config();

  bool can_generate() {
    return check_out_of_sync().is_ok() && mc_config_status_.is_ok();
  }

  static constexpr int COLLATOR_NODE_VERSION = 1;

 public:
  static constexpr int VERSION_OPTIMISTIC_COLLATE = 1;
};

}  // namespace ton::validator
