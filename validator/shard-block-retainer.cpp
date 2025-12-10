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
#include <delay.h>

#include "shard-block-retainer.hpp"

namespace ton::validator {

void ShardBlockRetainer::start_up() {
  if (last_masterchain_state_.not_null()) {
    update_masterchain_state(last_masterchain_state_);
  }

  class Callback : public adnl::Adnl::Callback {
   public:
    explicit Callback(td::actor::ActorId<ShardBlockRetainer> id) : id_(std::move(id)) {
    }
    void receive_message(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data) override {
    }
    void receive_query(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data,
                       td::Promise<td::BufferSlice> promise) override {
      td::actor::send_closure(id_, &ShardBlockRetainer::process_query, src, std::move(data), std::move(promise));
    }

   private:
    td::actor::ActorId<ShardBlockRetainer> id_;
  };
  td::actor::send_closure(adnl_, &adnl::Adnl::subscribe, local_id_,
                          adnl::Adnl::int_to_bytestring(ton_api::shardBlockVerifier_subscribe::ID),
                          std::make_unique<Callback>(actor_id(this)));
  td::actor::send_closure(rldp_, &rldp2::Rldp::add_id, local_id_);
}

void ShardBlockRetainer::tear_down() {
  td::actor::send_closure(adnl_, &adnl::Adnl::unsubscribe, local_id_,
                          adnl::Adnl::int_to_bytestring(ton_api::shardBlockVerifier_subscribe::ID));
}

void ShardBlockRetainer::update_masterchain_state(td::Ref<MasterchainState> state) {
  last_masterchain_state_ = state;
  for (auto it = confirmed_blocks_.begin(); it != confirmed_blocks_.end();) {
    if (is_block_outdated(*it)) {
      it = confirmed_blocks_.erase(it);
    } else {
      ++it;
    }
  }
  if (last_masterchain_state_->is_key_state() || !inited_) {
    validator_adnl_ids_.clear();
    for (int next = 0; next <= 1; ++next) {
      auto vset = last_masterchain_state_->get_total_validator_set(next);
      if (vset.is_null()) {
        continue;
      }
      for (auto& val : vset->export_vector()) {
        adnl::AdnlNodeIdShort adnl_id{val.addr};
        if (adnl_id.is_zero()) {
          adnl_id = adnl::AdnlNodeIdShort{ValidatorFullId{val.key}.short_id()};
          validator_adnl_ids_.insert(adnl_id);
        }
      }
    }
    LOG(INFO) << "Updating validator set: " << validator_adnl_ids_.size() << " adnl ids";
    for (auto it = subscribers_.begin(); it != subscribers_.end();) {
      if (it->second.is_in_past()) {
        LOG(INFO) << "Unsubscribed " << it->first.first << " for " << it->first.second.to_str() << " (expired)";
        it = subscribers_.erase(it);
        continue;
      }
      if (!validator_adnl_ids_.contains(it->first.first)) {
        LOG(INFO) << "Unsubscribed " << it->first.first << " for " << it->first.second.to_str() << " (not a validator)";
        it = subscribers_.erase(it);
        continue;
      }
      ++it;
    }
  }
  if (!inited_) {
    delay_action(
        [SelfId = actor_id(this), manager = manager_]() {
          td::actor::send_closure(
              manager, &ValidatorManager::iterate_temp_block_handles, [SelfId](const BlockHandleInterface& handle) {
                if (!handle.id().is_masterchain() && handle.received_state()) {
                  td::actor::send_closure(SelfId, &ShardBlockRetainer::got_block_from_db, handle.id());
                }
              });
        },
        td::Timestamp::in(1.0));
  }
  inited_ = true;
}

void ShardBlockRetainer::new_shard_block_description(td::Ref<ShardTopBlockDescription> desc) {
  if (last_masterchain_state_.is_null() || is_block_outdated(desc->block_id()) ||
      !opts_->need_monitor(desc->shard(), last_masterchain_state_)) {
    return;
  }
  td::actor::send_closure(
      manager_, &ValidatorManager::wait_block_state_short, desc->block_id(), 0, td::Timestamp::in(30.0), true,
      [SelfId = actor_id(this), desc](td::Result<td::Ref<ShardState>> R) {
        if (R.is_error()) {
          LOG(WARNING) << "Wait block state for " << desc->block_id().to_str() << " : " << R.move_as_error();
          td::actor::send_closure(SelfId, &ShardBlockRetainer::new_shard_block_description, std::move(desc));
          return;
        }
        td::actor::send_closure(SelfId, &ShardBlockRetainer::confirm_shard_block_description, std::move(desc));
      });
}

void ShardBlockRetainer::process_query(adnl::AdnlNodeIdShort src, td::BufferSlice data,
                                       td::Promise<td::BufferSlice> promise) {
  TRY_RESULT_PROMISE(promise, query, fetch_tl_object<ton_api::shardBlockVerifier_subscribe>(data, true))
  ShardIdFull shard = create_shard_id(query->shard_);
  if (!shard.is_valid_ext() || shard.is_masterchain()) {
    promise.set_error(td::Status::Error(PSTRING() << "invalid shard " << shard.to_str()));
    return;
  }
  if (!validator_adnl_ids_.contains(src)) {
    promise.set_error(td::Status::Error(PSTRING() << "unauthorized src " << src));
    return;
  }
  td::Timestamp& ttl = subscribers_[{src, shard}];
  if (!ttl) {
    std::vector<BlockIdExt> blocks;
    for (const BlockIdExt& block : confirmed_blocks_) {
      if (shard_intersects(block.shard_full(), shard)) {
        blocks.push_back(block);
      }
    }
    LOG(INFO) << "New subscriber " << src << " for " << shard.to_str() << ", sending " << blocks.size() << " blocks";
    send_confirmations(src, std::move(blocks));
  }
  ttl = td::Timestamp::in(SUBSCRIPTION_TTL);
  promise.set_value(create_serialize_tl_object<ton_api::shardBlockVerifier_subscribed>(0));
}

void ShardBlockRetainer::send_confirmations(adnl::AdnlNodeIdShort dst, std::vector<BlockIdExt> blocks) {
  for (size_t l = 0; l < blocks.size(); l += MAX_BLOCKS_PER_MESSAGE) {
    size_t r = std::min(l + MAX_BLOCKS_PER_MESSAGE, blocks.size());
    auto query = create_tl_object<ton_api::shardBlockVerifier_confirmBlocks>();
    for (size_t i = l; i < r; ++i) {
      query->blocks_.push_back(create_tl_block_id(blocks[i]));
    }
    td::actor::send_closure(rldp_, &rldp2::Rldp::send_message, local_id_, dst, serialize_tl_object(query, true));
  }
}

void ShardBlockRetainer::confirm_shard_block_description(td::Ref<ShardTopBlockDescription> desc) {
  for (const BlockIdExt& block_id : desc->get_chain_blocks()) {
    confirm_block(block_id);
  }
}

void ShardBlockRetainer::confirm_block(BlockIdExt block_id) {
  if (is_block_outdated(block_id) || !confirmed_blocks_.insert(block_id).second) {
    return;
  }
  size_t sent = 0;
  for (auto it = subscribers_.begin(); it != subscribers_.end();) {
    if (it->second.is_in_past()) {
      LOG(INFO) << "Unsubscribed " << it->first.first << " for " << it->first.second.to_str() << " (expired)";
      it = subscribers_.erase(it);
      continue;
    }
    if (shard_intersects(it->first.second, block_id.shard_full())) {
      ++sent;
      send_confirmations(it->first.first, {block_id});
    }
    ++it;
  }
  LOG(INFO) << "Confirmed block " << block_id.to_str() << ", sending " << sent << " confirmations";
}

void ShardBlockRetainer::got_block_from_db(BlockIdExt block_id) {
  if (!is_block_outdated(block_id)) {
    LOG(INFO) << "Loaded confirmed block from DB: " << block_id.to_str();
    confirm_block(block_id);
  }
}

bool ShardBlockRetainer::is_block_outdated(const BlockIdExt& block_id) const {
  ShardIdFull shard = block_id.shard_full();
  shard.shard |= 1;
  auto shard_desc = last_masterchain_state_->get_shard_from_config(shard, false);
  return shard_desc.not_null() && shard_desc->top_block_id().seqno() >= block_id.seqno();
}

}  // namespace ton::validator
