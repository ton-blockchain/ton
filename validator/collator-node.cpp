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
#include "block-auto.h"
#include "block-db.h"
#include "td/utils/lz4.h"
#include "checksum.h"
#include "validator-session/candidate-serializer.h"

namespace ton::validator {

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
  last_masterchain_block_ = state->get_block_id();
  last_top_blocks_.clear();
  last_top_blocks_[ShardIdFull{masterchainId, shardIdAll}] = last_masterchain_block_;
  for (const auto& desc : state->get_shards()) {
    last_top_blocks_[desc->shard()] = desc->top_block_id();
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
  // Remove old cache entries
  auto it = cache_.begin();
  while (it != cache_.end()) {
    auto prev_block_id = std::get<2>(it->first)[0];
    auto top_block = get_shard_top_block(prev_block_id.shard_full());
    if (top_block && top_block.value().seqno() > prev_block_id.seqno()) {
      it = cache_.erase(it);
    } else {
      ++it;
    }
  }
}

static td::BufferSlice serialize_error(td::Status error) {
  return create_serialize_tl_object<ton_api::collatorNode_generateBlockError>(error.code(), error.message().c_str());
}

static BlockCandidate change_creator(BlockCandidate block, Ed25519_PublicKey creator) {
  CHECK(!block.id.is_masterchain());
  if (block.pubkey == creator) {
    return block;
  }
  auto root = vm::std_boc_deserialize(block.data).move_as_ok();
  block::gen::Block::Record blk;
  block::gen::BlockExtra::Record extra;
  CHECK(tlb::unpack_cell(root, blk));
  CHECK(tlb::unpack_cell(blk.extra, extra));
  extra.created_by = creator.as_bits256();
  CHECK(tlb::pack_cell(blk.extra, extra));
  CHECK(tlb::pack_cell(root, blk));
  block.data = vm::std_boc_serialize(root, 31).move_as_ok();

  block.id.root_hash = root->get_hash().bits();
  block.id.file_hash = block::compute_file_hash(block.data.as_slice());
  block.pubkey = creator;
  return block;
}

void CollatorNode::receive_query(adnl::AdnlNodeIdShort src, td::BufferSlice data,
                                 td::Promise<td::BufferSlice> promise) {
  td::Promise<BlockCandidate> new_promise = [promise = std::move(promise), src](td::Result<BlockCandidate> R) mutable {
    if (R.is_error()) {
      LOG(WARNING) << "Query from " << src << ", error: " << R.error();
      promise.set_result(serialize_error(R.move_as_error()));
    } else {
      LOG(INFO) << "Query from " << src << ", success";
      promise.set_result(create_serialize_tl_object<ton_api::collatorNode_generateBlockSuccess>(
          serialize_candidate(R.move_as_ok(), true)));
    }
  };
  if (!last_masterchain_block_.is_valid()) {
    new_promise.set_error(td::Status::Error("not ready"));
    return;
  }
  if (!validators_.count(src)) {
    new_promise.set_error(td::Status::Error("src is not a validator"));
  }
  TRY_RESULT_PROMISE(new_promise, f, fetch_tl_object<ton_api::collatorNode_generateBlock>(std::move(data), true));
  ShardIdFull shard(f->workchain_, f->shard_);
  BlockIdExt min_mc_id = create_block_id(f->min_mc_id_);
  LOG(INFO) << "Got query from " << src << ": shard=" << shard.to_str() << ", min_mc_seqno=" << min_mc_id.seqno();
  if (!shard.is_valid_ext()) {
    new_promise.set_error(td::Status::Error(PSTRING() << "invalid shard " << shard.to_str()));
    return;
  }
  if (!can_collate_shard(shard)) {
    new_promise.set_error(td::Status::Error(PSTRING() << "this node doesn't collate shard " << shard.to_str()));
    return;
  }
  if (f->prev_blocks_.size() != 1 && f->prev_blocks_.size() != 2) {
    new_promise.set_error(td::Status::Error(PSTRING() << "invalid size of prev_blocks: " << f->prev_blocks_.size()));
    return;
  }
  if (!min_mc_id.is_masterchain_ext()) {
    new_promise.set_error(td::Status::Error("min_mc_id is not form masterchain"));
    return;
  }
  std::vector<BlockIdExt> prev_blocks;
  for (const auto& b : f->prev_blocks_) {
    auto id = create_block_id(b);
    if (!id.is_valid_full()) {
      new_promise.set_error(td::Status::Error("invalid prev_block"));
      return;
    }
    auto top_block = get_shard_top_block(id.shard_full());
    if (top_block && top_block.value().seqno() > id.seqno()) {
      new_promise.set_error(td::Status::Error("cannot collate block: already exists in blockchain"));
      return;
    }
    prev_blocks.push_back(id);
  }
  Ed25519_PublicKey creator(f->creator_);
  if (!shard.is_masterchain()) {
    // Collation of masterchain cannot be cached because changing "created_by" in masterchain is hard
    // It does not really matter because validators can collate masterchain themselves
    new_promise = [promise = std::move(new_promise), creator](td::Result<BlockCandidate> R) mutable {
      if (R.is_error()) {
        promise.set_error(R.move_as_error());
      } else {
        promise.set_result(change_creator(R.move_as_ok(), creator));
      }
    };
    auto cache_key = std::make_tuple(min_mc_id.seqno(), shard, prev_blocks);
    auto cache_entry = cache_[cache_key];
    if (cache_entry == nullptr) {
      cache_entry = cache_[cache_key] = std::make_shared<CacheEntry>();
    }
    if (cache_entry->result) {
      LOG(INFO) << "Using cached result";
      new_promise.set_result(cache_entry->result.value().clone());
      return;
    }
    cache_entry->promises.push_back(std::move(new_promise));
    if (cache_entry->started) {
      LOG(INFO) << "Collating of this block is already in progress, waiting";
      return;
    }
    cache_entry->started = true;
    new_promise = [SelfId = actor_id(this), cache_entry](td::Result<BlockCandidate> R) mutable {
      td::actor::send_closure(SelfId, &CollatorNode::process_result, cache_entry, std::move(R));
    };
  }

  auto P = td::PromiseCreator::lambda([=, SelfId = actor_id(this), prev_blocks = std::move(prev_blocks),
                                       promise = std::move(new_promise)](td::Result<td::Ref<ShardState>> R) mutable {
    if (R.is_error()) {
      promise.set_error(R.move_as_error_prefix("failed to get masterchain state: "));
    } else {
      td::Ref<MasterchainState> state(R.move_as_ok());
      if (state.is_null()) {
        promise.set_error(R.move_as_error_prefix("failed to get masterchain state: "));
        return;
      }
      td::actor::send_closure(SelfId, &CollatorNode::receive_query_cont, shard, std::move(state),
                              std::move(prev_blocks), creator, std::move(promise));
    }
  });
  td::actor::send_closure(manager_, &ValidatorManager::wait_block_state_short, min_mc_id, 1, td::Timestamp::in(5.0),
                          std::move(P));
}

void CollatorNode::receive_query_cont(ShardIdFull shard, td::Ref<MasterchainState> min_mc_state,
                                      std::vector<BlockIdExt> prev_blocks, Ed25519_PublicKey creator,
                                      td::Promise<BlockCandidate> promise) {
  run_collate_query(shard, min_mc_state->get_block_id(), std::move(prev_blocks), creator,
                    min_mc_state->get_validator_set(shard), manager_, td::Timestamp::in(10.0), std::move(promise),
                    CollateMode::skip_store_candidate);
}

void CollatorNode::process_result(std::shared_ptr<CacheEntry> cache_entry, td::Result<BlockCandidate> R) {
  if (R.is_error()) {
    cache_entry->started = false;
    for (auto& p : cache_entry->promises) {
      p.set_error(R.error().clone());
    }
  } else {
    cache_entry->result = R.move_as_ok();
    for (auto& p : cache_entry->promises) {
      p.set_result(cache_entry->result.value().clone());
    }
  }
  cache_entry->promises.clear();
}

bool CollatorNode::can_collate_shard(ShardIdFull shard) const {
  return std::any_of(shards_.begin(), shards_.end(),
                     [&](const ShardIdFull& our_shard) { return shard_intersects(shard, our_shard); });
}

tl_object_ptr<ton_api::collatorNode_Candidate> CollatorNode::serialize_candidate(const BlockCandidate& block,
                                                                                 bool compress) {
  if (!compress) {
    return create_tl_object<ton_api::collatorNode_candidate>(
        PublicKey{pubkeys::Ed25519{block.pubkey.as_bits256()}}.tl(), create_tl_block_id(block.id), block.data.clone(),
        block.collated_data.clone());
  }
  size_t decompressed_size;
  td::BufferSlice compressed =
      validatorsession::compress_candidate_data(block.data, block.collated_data, decompressed_size).move_as_ok();
  return create_tl_object<ton_api::collatorNode_compressedCandidate>(
      0, PublicKey{pubkeys::Ed25519{block.pubkey.as_bits256()}}.tl(), create_tl_block_id(block.id),
      (int)decompressed_size, std::move(compressed));
}

td::Result<BlockCandidate> CollatorNode::deserialize_candidate(tl_object_ptr<ton_api::collatorNode_Candidate> f,
                                                               int max_decompressed_data_size) {
  td::Result<BlockCandidate> res;
  ton_api::downcast_call(*f, td::overloaded(
                                 [&](ton_api::collatorNode_candidate& c) {
                                   res = [&]() -> td::Result<BlockCandidate> {
                                     auto hash = td::sha256_bits256(c.collated_data_);
                                     auto key = ton::PublicKey{c.source_};
                                     if (!key.is_ed25519()) {
                                       return td::Status::Error("invalid pubkey");
                                     }
                                     auto e_key = Ed25519_PublicKey{key.ed25519_value().raw()};
                                     return BlockCandidate{e_key, create_block_id(c.id_), hash, std::move(c.data_),
                                                           std::move(c.collated_data_)};
                                   }();
                                 },
                                 [&](ton_api::collatorNode_compressedCandidate& c) {
                                   res = [&]() -> td::Result<BlockCandidate> {
                                     if (c.decompressed_size_ <= 0) {
                                       return td::Status::Error("invalid decompressed size");
                                     }
                                     if (c.decompressed_size_ > max_decompressed_data_size) {
                                       return td::Status::Error("decompressed size is too big");
                                     }
                                     TRY_RESULT(
                                         p, validatorsession::decompress_candidate_data(c.data_, c.decompressed_size_));
                                     auto collated_data_hash = td::sha256_bits256(p.second);
                                     auto key = ton::PublicKey{c.source_};
                                     if (!key.is_ed25519()) {
                                       return td::Status::Error("invalid pubkey");
                                     }
                                     auto e_key = Ed25519_PublicKey{key.ed25519_value().raw()};
                                     return BlockCandidate{e_key, create_block_id(c.id_), collated_data_hash,
                                                           std::move(p.first), std::move(p.second)};
                                   }();
                                 }));
  return res;
}

}  // namespace ton::validator
