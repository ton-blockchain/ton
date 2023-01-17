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
#include "collator-node.hpp"
#include "ton/ton-tl.hpp"
#include "fabric.h"

namespace ton {

namespace validator {

CollatorNode::CollatorNode(adnl::AdnlNodeIdShort local_id, td::actor::ActorId<ValidatorManager> manager,
                           td::actor::ActorId<adnl::Adnl> adnl, td::actor::ActorId<rldp::Rldp> rldp)
    : local_id_(local_id), manager_(std::move(manager)), adnl_(std::move(adnl)), rldp_(std::move(rldp)) {
}

void CollatorNode::start_up() {
  class Cb : public adnl::Adnl::Callback {
   public:
    Cb(td::actor::ActorId<CollatorNode> id) : id_(std::move(id)) {
    }
    void receive_message(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data) override {
    }
    void receive_query(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data,
                       td::Promise<td::BufferSlice> promise) override {
      td::actor::send_closure(id_, &CollatorNode::receive_query, src, std::move(data), std::move(promise));
    }

   private:
    td::actor::ActorId<CollatorNode> id_;
  };
  td::actor::send_closure(adnl_, &adnl::Adnl::subscribe, local_id_,
                          adnl::Adnl::int_to_bytestring(ton_api::collatorNode_generateBlock::ID),
                          std::make_unique<Cb>(actor_id(this)));
  td::actor::send_closure(rldp_, &rldp::Rldp::add_id, adnl::AdnlNodeIdShort(local_id_));
}

void CollatorNode::tear_down() {
  td::actor::send_closure(adnl_, &adnl::Adnl::unsubscribe, local_id_,
                          adnl::Adnl::int_to_bytestring(ton_api::collatorNode_generateBlock::ID));
}

void CollatorNode::add_shard(ShardIdFull shard) {
  if (std::find(shards_.begin(), shards_.end(), shard) != shards_.end()) {
    return;
  }
  LOG(INFO) << "Collator node: local_id=" << local_id_ << " , shard=" << shard.to_str();
  shards_.push_back(shard);
}

void CollatorNode::del_shard(ShardIdFull shard) {
  auto it = std::find(shards_.begin(), shards_.end(), shard);
  if (it != shards_.end()) {
    shards_.erase(it);
  }
}

void CollatorNode::new_masterchain_block_notification(td::Ref<MasterchainState> state) {
  std::vector<BlockIdExt> top_blocks = {state->get_block_id()};
  std::vector<ShardIdFull> next_shards;
  if (collate_shard(ShardIdFull(masterchainId))) {
    next_shards.push_back(ShardIdFull(masterchainId));
  }
  for (const auto& desc : state->get_shards()) {
    top_blocks.push_back(desc->top_block_id());
    ShardIdFull shard = desc->shard();
    if (desc->before_split()) {
      if (collate_shard(shard_child(shard, true))) {
        next_shards.push_back(shard_child(shard, true));
      }
      if (collate_shard(shard_child(shard, false))) {
        next_shards.push_back(shard_child(shard, false));
      }
    } else if (desc->before_merge()) {
      if (is_left_child(shard) && collate_shard(shard_parent(shard))) {
        next_shards.push_back(shard_parent(shard));
      }
    } else if (collate_shard(shard)) {
      next_shards.push_back(shard);
    }
  }
  for (const ShardIdFull& shard : next_shards) {
    for (const BlockIdExt& neighbor : top_blocks) {
      if (neighbor.shard_full() != shard && block::ShardConfig::is_neighbor(shard, neighbor.shard_full())) {
        td::actor::send_closure(manager_, &ValidatorManager::wait_out_msg_queue_proof, neighbor, shard, 0,
                                td::Timestamp::in(10.0), [](td::Ref<OutMsgQueueProof>) {});
      }
    }
  }
  if (validators_.empty() || state->is_key_state()) {
    validators_.clear();
    for (int next : {-1, 0, 1}) {
      td::Ref<ValidatorSet> vals = state->get_total_validator_set(next);
      if (vals.not_null()) {
        for (const ValidatorDescr& descr : vals->export_vector()) {
          if (descr.addr.is_zero()) {
            validators_.insert(
                adnl::AdnlNodeIdShort(PublicKey(pubkeys::Ed25519{descr.key.as_bits256()}).compute_short_id()));
          } else {
            validators_.insert(adnl::AdnlNodeIdShort(descr.addr));
          }
        }
      }
    }
  }
}

static td::BufferSlice serialize_error(td::Status error) {
  return create_serialize_tl_object<ton_api::collatorNode_generateBlockError>(error.code(), error.message().c_str());
}

void CollatorNode::receive_query(adnl::AdnlNodeIdShort src, td::BufferSlice data,
                                 td::Promise<td::BufferSlice> promise) {
  auto SelfId = actor_id(this);
  auto status = [&]() -> td::Status {
    if (!validators_.count(src)) {
      return td::Status::Error("src is not a validator");
    }
    TRY_RESULT(f, fetch_tl_object<ton_api::collatorNode_generateBlock>(std::move(data), true));
    ShardIdFull shard(f->workchain_, f->shard_);
    if (!shard.is_valid_ext()) {
      return td::Status::Error(PSTRING() << "invalid shard " << shard.to_str());
    }
    if (!collate_shard(shard)) {
      return td::Status::Error(PSTRING() << "this node doesn't collate shard " << shard.to_str());
    }
    if (f->prev_blocks_.size() != 1 && f->prev_blocks_.size() != 2) {
      return td::Status::Error(PSTRING() << "invalid size of prev_blocks: " << f->prev_blocks_.size());
    }
    std::vector<BlockIdExt> prev_blocks;
    for (const auto& b : f->prev_blocks_) {
      prev_blocks.push_back(create_block_id(b));
    }
    BlockIdExt min_mc_id = create_block_id(f->min_mc_id_);
    Ed25519_PublicKey creator(f->creator_);

    LOG(INFO) << "Query from " << src << ": shard=" << shard.to_str() << ", min_mc_id=" << min_mc_id.to_str();

    auto P = td::PromiseCreator::lambda([=, prev_blocks = std::move(prev_blocks),
                                         promise = std::move(promise)](td::Result<td::Ref<ShardState>> R) mutable {
      if (R.is_error()) {
        LOG(WARNING) << "Query from " << src << ", error: " << R.error();
        promise.set_result(serialize_error(R.move_as_error_prefix("failed to get masterchain state: ")));
      } else {
        td::Ref<MasterchainState> state(R.move_as_ok());
        if (state.is_null()) {
          LOG(WARNING) << "Query from " << src << ", error: failed to get masterchain state";
          promise.set_result(serialize_error(R.move_as_error_prefix("failed to get masterchain state: ")));
          return;
        }
        td::actor::send_closure(SelfId, &CollatorNode::receive_query_cont, src, shard, std::move(state),
                                std::move(prev_blocks), creator, std::move(promise));
      }
    });
    td::actor::send_closure(manager_, &ValidatorManager::wait_block_state_short, min_mc_id, 1, td::Timestamp::in(5.0),
                            std::move(P));
    return td::Status::OK();
  }();
  if (status.is_error()) {
    LOG(WARNING) << "Query from " << src << ", error: " << status;
    promise.set_result(serialize_error(std::move(status)));
  }
}

void CollatorNode::receive_query_cont(adnl::AdnlNodeIdShort src, ShardIdFull shard,
                                      td::Ref<MasterchainState> min_mc_state, std::vector<BlockIdExt> prev_blocks,
                                      Ed25519_PublicKey creator, td::Promise<td::BufferSlice> promise) {
  auto P = td::PromiseCreator::lambda([promise = std::move(promise), src](td::Result<BlockCandidate> R) mutable {
    if (R.is_error()) {
      LOG(WARNING) << "Query from " << src << ", error: " << R.error();
      promise.set_result(serialize_error(R.move_as_error()));
    } else {
      LOG(INFO) << "Query from " << src << ", success";
      auto block = R.move_as_ok();
      auto result = create_serialize_tl_object<ton_api::collatorNode_generateBlockSuccess>(
          create_tl_object<ton_api::db_candidate>(PublicKey{pubkeys::Ed25519{block.pubkey.as_bits256()}}.tl(),
                                                  create_tl_block_id(block.id), std::move(block.data),
                                                  std::move(block.collated_data)));
      promise.set_result(std::move(result));
    }
  });

  run_collate_query(shard, min_mc_state->get_block_id(), std::move(prev_blocks), creator,
                    min_mc_state->get_validator_set(shard), manager_, td::Timestamp::in(10.0), std::move(P));
}

bool CollatorNode::collate_shard(ShardIdFull shard) const {
  for (ShardIdFull our_shard : shards_) {
    if (shard_intersects(shard, our_shard)) {
      return true;
    }
  }
  return false;
}

}  // namespace validator

}  // namespace ton
