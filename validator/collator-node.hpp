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

  void new_masterchain_block_notification(td::Ref<MasterchainState> state);

 private:
  void receive_query(adnl::AdnlNodeIdShort src, td::BufferSlice data, td::Promise<td::BufferSlice> promise);
  void receive_query_cont(adnl::AdnlNodeIdShort src, ShardIdFull shard, td::Ref<MasterchainState> min_mc_state,
                          std::vector<BlockIdExt> prev_blocks, Ed25519_PublicKey creator,
                          td::Promise<td::BufferSlice> promise);

  bool collate_shard(ShardIdFull shard) const;

  adnl::AdnlNodeIdShort local_id_;
  td::actor::ActorId<ValidatorManager> manager_;
  td::actor::ActorId<adnl::Adnl> adnl_;
  td::actor::ActorId<rldp::Rldp> rldp_;
  std::vector<ShardIdFull> shards_;
};

}  // namespace validator

}  // namespace ton
