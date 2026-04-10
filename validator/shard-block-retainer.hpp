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

#include "interfaces/validator-manager.h"
#include "rldp2/rldp.h"

namespace ton::validator {

class ShardBlockRetainer : public td::actor::Actor {
 public:
  ShardBlockRetainer(adnl::AdnlNodeIdShort local_id, td::Ref<MasterchainState> last_masterchain_state,
                     td::Ref<ValidatorManagerOptions> opts, td::actor::ActorId<ValidatorManager> manager,
                     td::actor::ActorId<adnl::Adnl> adnl, td::actor::ActorId<rldp2::Rldp> rldp)
      : local_id_(local_id)
      , last_masterchain_state_(last_masterchain_state)
      , opts_(std::move(opts))
      , manager_(std::move(manager))
      , adnl_(std::move(adnl))
      , rldp_(std::move(rldp)) {
  }

  void start_up() override;
  void tear_down() override;
  void update_masterchain_state(td::Ref<MasterchainState> state);
  void new_shard_block_description(td::Ref<ShardTopBlockDescription> desc);

  void update_options(td::Ref<ValidatorManagerOptions> opts) {
    opts_ = std::move(opts);
  }

 private:
  adnl::AdnlNodeIdShort local_id_;
  td::Ref<MasterchainState> last_masterchain_state_;
  td::Ref<ValidatorManagerOptions> opts_;
  td::actor::ActorId<ValidatorManager> manager_;
  td::actor::ActorId<adnl::Adnl> adnl_;
  td::actor::ActorId<rldp2::Rldp> rldp_;

  bool inited_ = false;
  std::set<adnl::AdnlNodeIdShort> validator_adnl_ids_;
  std::map<std::pair<adnl::AdnlNodeIdShort, ShardIdFull>, td::Timestamp> subscribers_;
  std::set<BlockIdExt> confirmed_blocks_;

  void process_query(adnl::AdnlNodeIdShort src, td::BufferSlice data, td::Promise<td::BufferSlice> promise);
  void send_confirmations(adnl::AdnlNodeIdShort dst, std::vector<BlockIdExt> blocks);

  void confirm_shard_block_description(td::Ref<ShardTopBlockDescription> desc);
  void confirm_block(BlockIdExt block_id);

  void got_block_from_db(BlockIdExt block_id);

  bool is_block_outdated(const BlockIdExt& block_id) const;

  static constexpr double SUBSCRIPTION_TTL = 60.0;
  static constexpr size_t MAX_BLOCKS_PER_MESSAGE = 8;
};

}  // namespace ton::validator
