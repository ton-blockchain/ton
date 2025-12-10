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
#include "td/actor/MultiPromise.h"

#include "shard-block-verifier.hpp"

namespace ton::validator {

void ShardBlockVerifier::start_up() {
  update_config(opts_->get_shard_block_verifier_config());
  update_masterchain_state(last_masterchain_state_);

  class Callback : public adnl::Adnl::Callback {
   public:
    explicit Callback(td::actor::ActorId<ShardBlockVerifier> id) : id_(std::move(id)) {
    }
    void receive_message(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data) override {
      td::actor::send_closure(id_, &ShardBlockVerifier::process_message, src, std::move(data));
    }
    void receive_query(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data,
                       td::Promise<td::BufferSlice> promise) override {
    }

   private:
    td::actor::ActorId<ShardBlockVerifier> id_;
  };
  td::actor::send_closure(adnl_, &adnl::Adnl::subscribe, local_id_,
                          adnl::Adnl::int_to_bytestring(ton_api::shardBlockVerifier_confirmBlocks::ID),
                          std::make_unique<Callback>(actor_id(this)));
  td::actor::send_closure(rldp_, &rldp2::Rldp::add_id, local_id_);
}

void ShardBlockVerifier::tear_down() {
  td::actor::send_closure(adnl_, &adnl::Adnl::unsubscribe, local_id_,
                          adnl::Adnl::int_to_bytestring(ton_api::shardBlockVerifier_confirmBlocks::ID));
}

void ShardBlockVerifier::update_masterchain_state(td::Ref<MasterchainState> state) {
  last_masterchain_state_ = std::move(state);
  for (auto it = blocks_.begin(); it != blocks_.end();) {
    if (is_block_outdated(it->first)) {
      it->second.finalize_promises();
      it = blocks_.erase(it);
    } else {
      ++it;
    }
  }
}

void ShardBlockVerifier::wait_shard_blocks(std::vector<BlockIdExt> blocks, td::Promise<td::Unit> promise) {
  td::MultiPromise mp;
  auto ig = mp.init_guard();
  ig.add_promise(std::move(promise));
  for (const BlockIdExt& block_id : blocks) {
    BlockInfo* info = get_block_info(block_id);
    if (info && !info->confirmed) {
      info->promises.push_back(ig.get_promise());
    }
  }
}

void ShardBlockVerifier::update_config(td::Ref<ShardBlockVerifierConfig> new_config) {
  auto old_config = std::move(config_);
  config_ = std::move(new_config);
  auto old_blocks = std::move(blocks_);
  blocks_.clear();
  for (auto& [block_id, old_info] : old_blocks) {
    BlockInfo* new_info = get_block_info(block_id);
    if (new_info == nullptr) {
      old_info.finalize_promises();
      continue;
    }
    new_info->promises = std::move(old_info.promises);
    if (new_info->confirmed) {
      new_info->finalize_promises();
    }
    for (size_t old_src_idx = 0; old_src_idx < old_info.confirmed_by.size(); ++old_src_idx) {
      if (old_info.confirmed_by[old_src_idx]) {
        set_block_confirmed(old_config->shards[old_info.config_shard_idx].trusted_nodes[old_src_idx], block_id);
      }
    }
  }

  alarm_timestamp().relax(send_subscribe_at_ = td::Timestamp::now());
}

void ShardBlockVerifier::alarm() {
  if (send_subscribe_at_ && send_subscribe_at_.is_in_past()) {
    for (auto& shard_config : config_->shards) {
      for (auto& node_id : shard_config.trusted_nodes) {
        td::Promise<td::BufferSlice> P = [shard = shard_config.shard_id, node_id](td::Result<td::BufferSlice> R) {
          if (R.is_error()) {
            LOG(WARNING) << "Subscribe to " << node_id << " for " << shard.to_str() << " : " << R.move_as_error();
          }
        };
        td::actor::send_closure(rldp_, &rldp2::Rldp::send_query, local_id_, node_id, "subscribe", std::move(P),
                                td::Timestamp::in(3.0),
                                create_serialize_tl_object<ton_api::shardBlockVerifier_subscribe>(
                                    create_tl_shard_id(shard_config.shard_id), 0));
      }
    }
    send_subscribe_at_ = td::Timestamp::in(SEND_SUBSCRIBE_PERIOD);
  }
  alarm_timestamp().relax(send_subscribe_at_);
}

void ShardBlockVerifier::process_message(adnl::AdnlNodeIdShort src, td::BufferSlice data) {
  auto r_obj = fetch_tl_object<ton_api::shardBlockVerifier_confirmBlocks>(data, true);
  if (r_obj.is_error()) {
    return;
  }
  for (const auto& b : r_obj.ok()->blocks_) {
    set_block_confirmed(src, create_block_id(b));
  }
}

int ShardBlockVerifier::get_config_shard_idx(const ShardIdFull& shard_id) const {
  for (size_t i = 0; i < config_->shards.size(); i++) {
    if (shard_intersects(shard_id, config_->shards[i].shard_id)) {
      return (int)i;
    }
  }
  return -1;
}

bool ShardBlockVerifier::is_block_outdated(const BlockIdExt& block_id) const {
  ShardIdFull shard = block_id.shard_full();
  shard.shard |= 1;
  auto shard_desc = last_masterchain_state_->get_shard_from_config(shard, false);
  return shard_desc.not_null() && shard_desc->top_block_id().seqno() >= block_id.seqno();
}

ShardBlockVerifier::BlockInfo* ShardBlockVerifier::get_block_info(const BlockIdExt& block_id) {
  auto it = blocks_.find(block_id);
  if (it != blocks_.end()) {
    return &it->second;
  }
  int config_shard_idx = get_config_shard_idx(block_id.shard_full());
  if (config_shard_idx < 0 || is_block_outdated(block_id)) {
    return nullptr;
  }
  auto& shard_config = config_->shards[config_shard_idx];
  BlockInfo& info = blocks_[block_id];
  info.config_shard_idx = config_shard_idx;
  info.confirmed_by.resize(shard_config.trusted_nodes.size(), false);
  info.confirmed = (shard_config.required_confirms == 0);
  return &info;
}

void ShardBlockVerifier::set_block_confirmed(adnl::AdnlNodeIdShort src, BlockIdExt block_id) {
  BlockInfo* info = get_block_info(block_id);
  if (info == nullptr) {
    LOG(INFO) << "Confirm for " << block_id.to_str() << " from " << src << " : ignored";
    return;
  }
  auto& shard_config = config_->shards[info->config_shard_idx];
  size_t src_idx = 0;
  while (src_idx < shard_config.trusted_nodes.size() && shard_config.trusted_nodes[src_idx] != src) {
    ++src_idx;
  }
  if (src_idx == shard_config.trusted_nodes.size()) {
    LOG(INFO) << "Confirm for " << block_id.to_str() << " from " << src << " : unknown src";
    return;
  }
  if (info->confirmed_by[src_idx]) {
    LOG(INFO) << "Confirm for " << block_id.to_str() << " from " << src << " #" << src_idx << " : duplicate";
    return;
  }
  info->confirmed_by[src_idx] = true;
  ++info->confirmed_by_cnt;
  LOG(INFO) << "Confirm for " << block_id.to_str() << " from " << src << " #" << src_idx << " : accepted ("
            << info->confirmed_by_cnt << "/" << shard_config.required_confirms << "/"
            << shard_config.trusted_nodes.size() << ")"
            << (info->confirmed_by_cnt == shard_config.required_confirms ? ", CONFIRMED" : "");
  if (info->confirmed_by_cnt == shard_config.required_confirms) {
    info->confirmed = true;
    info->finalize_promises();
    info->promises.clear();
  }
}

}  // namespace ton::validator
