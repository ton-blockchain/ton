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

class ShardBlockVerifier : public td::actor::Actor {
 public:
  ShardBlockVerifier(adnl::AdnlNodeIdShort local_id, td::Ref<MasterchainState> last_masterchain_state,
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
  void wait_shard_blocks(std::vector<BlockIdExt> blocks, td::Promise<td::Unit> promise);

  void update_options(td::Ref<ValidatorManagerOptions> opts) {
    if (config_ != opts->get_shard_block_verifier_config()) {
      update_config(opts->get_shard_block_verifier_config());
    }
    opts_ = std::move(opts);
  }

  void alarm() override;

 private:
  adnl::AdnlNodeIdShort local_id_;
  td::Ref<MasterchainState> last_masterchain_state_;
  td::Ref<ValidatorManagerOptions> opts_;
  td::actor::ActorId<ValidatorManager> manager_;
  td::actor::ActorId<adnl::Adnl> adnl_;
  td::actor::ActorId<rldp2::Rldp> rldp_;

  td::Ref<ShardBlockVerifierConfig> config_;

  td::Timestamp send_subscribe_at_ = td::Timestamp::never();

  struct BlockInfo {
    size_t config_shard_idx = 0;
    std::vector<bool> confirmed_by;
    td::uint32 confirmed_by_cnt = 0;
    bool confirmed = false;
    std::vector<td::Promise<td::Unit>> promises;

    void finalize_promises() {
      for (auto& promise : promises) {
        promise.set_value(td::Unit());
      }
    }
  };
  std::map<BlockIdExt, BlockInfo> blocks_;

  void update_config(td::Ref<ShardBlockVerifierConfig> new_config);
  void process_message(adnl::AdnlNodeIdShort src, td::BufferSlice data);

  int get_config_shard_idx(const ShardIdFull& shard_id) const;
  bool is_block_outdated(const BlockIdExt& block_id) const;
  BlockInfo* get_block_info(const BlockIdExt& block_id);

  void set_block_confirmed(adnl::AdnlNodeIdShort src, BlockIdExt block_id);

  static constexpr double SEND_SUBSCRIBE_PERIOD = 10.0;
};

}  // namespace ton::validator
