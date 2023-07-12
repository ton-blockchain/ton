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

namespace ton {

namespace validator {

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

 private:
  void receive_query(adnl::AdnlNodeIdShort src, td::BufferSlice data, td::Promise<td::BufferSlice> promise);
  void receive_query_cont(ShardIdFull shard, td::Ref<MasterchainState> min_mc_state,
                          std::vector<BlockIdExt> prev_blocks, Ed25519_PublicKey creator,
                          td::Promise<BlockCandidate> promise);

  bool can_collate_shard(ShardIdFull shard) const;

  adnl::AdnlNodeIdShort local_id_;
  td::actor::ActorId<ValidatorManager> manager_;
  td::actor::ActorId<adnl::Adnl> adnl_;
  td::actor::ActorId<rldp::Rldp> rldp_;
  std::vector<ShardIdFull> shards_;
  std::set<adnl::AdnlNodeIdShort> validators_;

  BlockIdExt last_masterchain_block_{};
  std::map<ShardIdFull, BlockIdExt> last_top_blocks_;

  struct CacheEntry {
    bool started = false;
    td::optional<BlockCandidate> result;
    std::vector<td::Promise<BlockCandidate>> promises;
  };
  std::map<std::tuple<BlockSeqno, ShardIdFull, std::vector<BlockIdExt>>, std::shared_ptr<CacheEntry>> cache_;

  td::optional<BlockIdExt> get_shard_top_block(ShardIdFull shard) const {
    auto it = last_top_blocks_.lower_bound(shard);
    if (it != last_top_blocks_.end() && shard_intersects(it->first, shard)) {
      return it->second;
    }
    if (it != last_top_blocks_.begin()) {
      --it;
      if (shard_intersects(it->first, shard)) {
        return it->second;
      }
    }
    return {};
  }

  void process_result(std::shared_ptr<CacheEntry> cache_entry, td::Result<BlockCandidate> R);
};

}  // namespace validator

}  // namespace ton
