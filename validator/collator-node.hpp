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

class CollatorNode : public td::actor::Actor {
 public:
  CollatorNode(adnl::AdnlNodeIdShort local_id, td::actor::ActorId<ValidatorManager> manager,
               td::actor::ActorId<adnl::Adnl> adnl, td::actor::ActorId<rldp::Rldp> rldp);
  void start_up() override;
  void tear_down() override;
  void add_shard(ShardIdFull shard);
  void del_shard(ShardIdFull shard);

  void new_masterchain_block_notification(td::Ref<MasterchainState> state);
  void update_validator_group_info(ShardIdFull shard, std::vector<BlockIdExt> prev, CatchainSeqno cc_seqno);

 private:
  void receive_query(adnl::AdnlNodeIdShort src, td::BufferSlice data, td::Promise<td::BufferSlice> promise);

  bool can_collate_shard(ShardIdFull shard) const;

  adnl::AdnlNodeIdShort local_id_;
  td::actor::ActorId<ValidatorManager> manager_;
  td::actor::ActorId<adnl::Adnl> adnl_;
  td::actor::ActorId<rldp::Rldp> rldp_;
  std::vector<ShardIdFull> collating_shards_;
  std::set<adnl::AdnlNodeIdShort> validator_adnl_ids_;

  struct CacheEntry {
    bool started = false;
    BlockSeqno block_seqno = 0;
    td::optional<BlockCandidate> result;
    td::CancellationTokenSource cancellation_token_source;
    std::vector<td::Promise<BlockCandidate>> promises;

    void cancel(td::Status reason);
  };
  struct ValidatorGroupInfo {
    CatchainSeqno cc_seqno{0};
    std::vector<BlockIdExt> prev;
    BlockSeqno next_block_seqno{0};
    std::map<std::vector<BlockIdExt>, std::shared_ptr<CacheEntry>> cache;

    void cleanup();
  };
  struct FutureValidatorGroup {
    std::vector<std::vector<BlockIdExt>> pending_blocks;
    std::vector<td::Promise<td::Unit>> promises;
  };
  std::map<ShardIdFull, ValidatorGroupInfo> validator_groups_;
  std::map<std::pair<ShardIdFull, CatchainSeqno>, FutureValidatorGroup> future_validator_groups_;

  td::Ref<MasterchainState> last_masterchain_state_;

  td::Result<FutureValidatorGroup*> get_future_validator_group(ShardIdFull shard, CatchainSeqno cc_seqno);

  void generate_block(ShardIdFull shard, CatchainSeqno cc_seqno, std::vector<BlockIdExt> prev_blocks,
                      td::Timestamp timeout, td::Promise<BlockCandidate> promise);
  void process_result(std::shared_ptr<CacheEntry> cache_entry, td::Result<BlockCandidate> R);

 public:
  static tl_object_ptr<ton_api::collatorNode_Candidate> serialize_candidate(const BlockCandidate& block, bool compress);
  static td::Result<BlockCandidate> deserialize_candidate(tl_object_ptr<ton_api::collatorNode_Candidate> f,
                                                          int max_decompressed_data_size);
};

}  // namespace ton::validator
