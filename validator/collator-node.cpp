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
#include "impl/shard.hpp"
#include "validator-session/candidate-serializer.h"

namespace ton::validator {

CollatorNode::CollatorNode(adnl::AdnlNodeIdShort local_id, td::Ref<ValidatorManagerOptions> opts,
                           td::actor::ActorId<ValidatorManager> manager, td::actor::ActorId<adnl::Adnl> adnl,
                           td::actor::ActorId<rldp::Rldp> rldp)
    : local_id_(local_id)
    , opts_(std::move(opts))
    , manager_(std::move(manager))
    , adnl_(std::move(adnl))
    , rldp_(std::move(rldp)) {
}

void CollatorNode::start_up() {
  class Cb : public adnl::Adnl::Callback {
   public:
    explicit Cb(td::actor::ActorId<CollatorNode> id) : id_(std::move(id)) {
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
  CHECK(shard.is_valid_ext() && !shard.is_masterchain());
  if (std::find(collating_shards_.begin(), collating_shards_.end(), shard) != collating_shards_.end()) {
    return;
  }
  LOG(INFO) << "Collator node: local_id=" << local_id_ << " , shard=" << shard.to_str();
  collating_shards_.push_back(shard);
}

void CollatorNode::del_shard(ShardIdFull shard) {
  auto it = std::find(collating_shards_.begin(), collating_shards_.end(), shard);
  if (it != collating_shards_.end()) {
    collating_shards_.erase(it);
  }
}

void CollatorNode::new_masterchain_block_notification(td::Ref<MasterchainState> state) {
  last_masterchain_state_ = state;
  if (validator_adnl_ids_.empty() || state->is_key_state()) {
    validator_adnl_ids_.clear();
    for (int next : {-1, 0, 1}) {
      td::Ref<ValidatorSet> vals = state->get_total_validator_set(next);
      if (vals.not_null()) {
        for (const ValidatorDescr& descr : vals->export_vector()) {
          if (descr.addr.is_zero()) {
            validator_adnl_ids_.insert(
                adnl::AdnlNodeIdShort(PublicKey(pubkeys::Ed25519{descr.key.as_bits256()}).compute_short_id()));
          } else {
            validator_adnl_ids_.insert(adnl::AdnlNodeIdShort(descr.addr));
          }
        }
      }
    }
  }

  std::map<ShardIdFull, std::vector<BlockIdExt>> new_shards;
  for (auto& v : state->get_shards()) {
    auto shard = v->shard();
    if (v->before_split()) {
      CHECK(!v->before_merge());
      new_shards.emplace(shard_child(shard, true), std::vector{v->top_block_id()});
      new_shards.emplace(shard_child(shard, false), std::vector{v->top_block_id()});
    } else if (v->before_merge()) {
      ShardIdFull p_shard = shard_parent(shard);
      auto it = new_shards.find(p_shard);
      if (it == new_shards.end()) {
        new_shards.emplace(p_shard, std::vector<BlockIdExt>(2));
      }
      bool left = shard_child(p_shard.shard, true) == shard.shard;
      new_shards[p_shard][left ? 0 : 1] = v->top_block_id();
    } else {
      new_shards.emplace(shard, std::vector{v->top_block_id()});
    }
  }

  for (auto& [shard, prev] : new_shards) {
    CatchainSeqno cc_seqno = state->get_validator_set(shard)->get_catchain_seqno();
    auto it = validator_groups_.emplace(shard, ValidatorGroupInfo{});
    ValidatorGroupInfo& info = it.first->second;
    if (it.second || info.cc_seqno != cc_seqno) {
      info.cleanup();
      info.cc_seqno = cc_seqno;
    }
  }
  for (auto it = validator_groups_.begin(); it != validator_groups_.end();) {
    if (new_shards.count(it->first)) {
      ++it;
    } else {
      it->second.cleanup();
      it = validator_groups_.erase(it);
    }
  }
  for (auto& [shard, prev] : new_shards) {
    ValidatorGroupInfo& info = validator_groups_[shard];
    update_validator_group_info(shard, std::move(prev), info.cc_seqno);
    auto it = future_validator_groups_.find({shard, info.cc_seqno});
    if (it != future_validator_groups_.end()) {
      for (auto& new_prev : it->second.pending_blocks) {
        update_validator_group_info(shard, std::move(new_prev), info.cc_seqno);
      }
      for (auto& promise : it->second.promises) {
        promise.set_value(td::Unit());
      }
      future_validator_groups_.erase(it);
    }
  }

  for (auto it = future_validator_groups_.begin(); it != future_validator_groups_.end();) {
    if (get_future_validator_group(it->first.first, it->first.second).is_ok()) {
      ++it;
    } else {
      auto& future_group = it->second;
      for (auto& promise : future_group.promises) {
        promise.set_error(td::Status::Error("validator group is outdated"));
      }
      it = future_validator_groups_.erase(it);
    }
  }
}

void CollatorNode::update_validator_group_info(ShardIdFull shard, std::vector<BlockIdExt> prev,
                                               CatchainSeqno cc_seqno) {
  if (!can_collate_shard(shard)) {
    return;
  }
  CHECK(prev.size() == 1 || prev.size() == 2);
  BlockSeqno next_block_seqno = prev[0].seqno() + 1;
  if (prev.size() == 2) {
    next_block_seqno = std::max(next_block_seqno, prev[1].seqno() + 1);
  }
  auto it = validator_groups_.find(shard);
  if (it != validator_groups_.end()) {
    ValidatorGroupInfo& info = it->second;
    if (info.cc_seqno == cc_seqno) {  // block from currently known validator group
      if (info.next_block_seqno < next_block_seqno) {
        LOG(DEBUG) << "updated validator group info: shard=" << shard.to_str() << ", cc_seqno=" << cc_seqno
                   << ", next_block_seqno=" << next_block_seqno;
        info.next_block_seqno = next_block_seqno;
        info.prev = std::move(prev);
        for (auto cache_it = info.cache.begin(); cache_it != info.cache.end();) {
          auto& [cached_prev, cache_entry] = *cache_it;
          if (cache_entry->block_seqno < info.next_block_seqno) {
            cache_entry->cancel(td::Status::Error(PSTRING() << "next block seqno " << cache_entry->block_seqno
                                                            << " is too small, expected " << info.next_block_seqno));
            cache_it = info.cache.erase(cache_it);
            continue;
          }
          if (cache_entry->block_seqno == info.next_block_seqno && cached_prev != info.prev) {
            cache_entry->cancel(td::Status::Error("invalid prev blocks"));
            cache_it = info.cache.erase(cache_it);
            continue;
          }
          ++cache_it;
        }
        generate_block(shard, cc_seqno, info.prev, td::Timestamp::in(10.0), [](td::Result<BlockCandidate>) {});
      }
      return;
    }
  }
  auto future_validator_group = get_future_validator_group(shard, cc_seqno);
  if (future_validator_group.is_ok()) {
    // future validator group, remember for later
    future_validator_group.ok()->pending_blocks.push_back(std::move(prev));
  }
}

td::Result<CollatorNode::FutureValidatorGroup*> CollatorNode::get_future_validator_group(ShardIdFull shard,
                                                                                         CatchainSeqno cc_seqno) {
  auto it = validator_groups_.find(shard);
  if (it == validator_groups_.end() && shard.pfx_len() != 0) {
    it = validator_groups_.find(shard_parent(shard));
  }
  if (it == validator_groups_.end() && shard.pfx_len() < max_shard_pfx_len) {
    it = validator_groups_.find(shard_child(shard, true));
  }
  if (it == validator_groups_.end() && shard.pfx_len() < max_shard_pfx_len) {
    it = validator_groups_.find(shard_child(shard, false));
  }
  if (it == validator_groups_.end()) {
    return td::Status::Error("no such shard");
  }
  if (cc_seqno < it->second.cc_seqno) {  // past validator group
    return td::Status::Error(PSTRING() << "cc_seqno " << cc_seqno << " is outdated (current is" << it->second.cc_seqno
                                       << ")");
  }
  if (cc_seqno - it->second.cc_seqno > 1) {  // future validator group, cc_seqno too big
    return td::Status::Error(PSTRING() << "cc_seqno " << cc_seqno << " is too big (currently known is"
                                       << it->second.cc_seqno << ")");
  }
  // future validator group
  return &future_validator_groups_[{shard, cc_seqno}];
}

void CollatorNode::ValidatorGroupInfo::cleanup() {
  prev.clear();
  next_block_seqno = 0;
  for (auto& [_, cache_entry] : cache) {
    cache_entry->cancel(td::Status::Error("validator group is outdated"));
  }
  cache.clear();
}

void CollatorNode::CacheEntry::cancel(td::Status reason) {
  for (auto& promise : promises) {
    promise.set_error(reason.clone());
  }
  promises.clear();
  cancellation_token_source.cancel();
}

static td::BufferSlice serialize_error(td::Status error) {
  return create_serialize_tl_object<ton_api::collatorNode_generateBlockError>(error.code(), error.message().c_str());
}

static BlockCandidate change_creator(BlockCandidate block, Ed25519_PublicKey creator, CatchainSeqno& cc_seqno,
                                     td::uint32& val_set_hash) {
  CHECK(!block.id.is_masterchain());
  if (block.pubkey == creator) {
    return block;
  }
  auto root = vm::std_boc_deserialize(block.data).move_as_ok();
  block::gen::Block::Record blk;
  block::gen::BlockExtra::Record extra;
  block::gen::BlockInfo::Record info;
  CHECK(tlb::unpack_cell(root, blk));
  CHECK(tlb::unpack_cell(blk.extra, extra));
  CHECK(tlb::unpack_cell(blk.info, info));
  extra.created_by = creator.as_bits256();
  CHECK(tlb::pack_cell(blk.extra, extra));
  CHECK(tlb::pack_cell(root, blk));
  block.data = vm::std_boc_serialize(root, 31).move_as_ok();

  block.id.root_hash = root->get_hash().bits();
  block.id.file_hash = block::compute_file_hash(block.data.as_slice());
  block.pubkey = creator;

  cc_seqno = info.gen_catchain_seqno;
  val_set_hash = info.gen_validator_list_hash_short;
  return block;
}

void CollatorNode::receive_query(adnl::AdnlNodeIdShort src, td::BufferSlice data,
                                 td::Promise<td::BufferSlice> promise) {
  td::Promise<BlockCandidate> new_promise = [promise = std::move(promise), src](td::Result<BlockCandidate> R) mutable {
    if (R.is_error()) {
      LOG(INFO) << "adnl query from " << src << ", error: " << R.error();
      if (R.error().code() == ErrorCode::timeout) {
        promise.set_error(R.move_as_error());
      } else {
        promise.set_result(serialize_error(R.move_as_error()));
      }
    } else {
      LOG(INFO) << "adnl query from " << src << ", success";
      promise.set_result(create_serialize_tl_object<ton_api::collatorNode_generateBlockSuccess>(
          serialize_candidate(R.move_as_ok(), true)));
    }
  };
  if (!validator_adnl_ids_.count(src)) {
    new_promise.set_error(td::Status::Error("src is not a validator"));
    return;
  }
  TRY_RESULT_PROMISE(new_promise, f, fetch_tl_object<ton_api::collatorNode_generateBlock>(data, true));
  ShardIdFull shard = create_shard_id(f->shard_);
  CatchainSeqno cc_seqno = f->cc_seqno_;
  std::vector<BlockIdExt> prev_blocks;
  for (const auto& b : f->prev_blocks_) {
    prev_blocks.push_back(create_block_id(b));
  }
  Ed25519_PublicKey creator(f->creator_);
  new_promise = [new_promise = std::move(new_promise), creator,
                 manager = manager_](td::Result<BlockCandidate> R) mutable {
    TRY_RESULT_PROMISE(new_promise, block, std::move(R));
    CatchainSeqno cc_seqno;
    td::uint32 val_set_hash;
    block = change_creator(std::move(block), creator, cc_seqno, val_set_hash);
    td::Promise<td::Unit> P =
        new_promise.wrap([block = block.clone()](td::Unit&&) mutable -> BlockCandidate { return std::move(block); });
    td::actor::send_closure(manager, &ValidatorManager::set_block_candidate, block.id, std::move(block), cc_seqno,
                            val_set_hash, std::move(P));
  };
  if (!shard.is_valid_ext()) {
    new_promise.set_error(td::Status::Error(PSTRING() << "invalid shard " << shard.to_str()));
    return;
  }
  if (prev_blocks.size() != 1 && prev_blocks.size() != 2) {
    new_promise.set_error(td::Status::Error(PSTRING() << "invalid size of prev_blocks: " << prev_blocks.size()));
    return;
  }
  LOG(INFO) << "got adnl query from " << src << ": shard=" << shard.to_str() << ", cc_seqno=" << cc_seqno;
  generate_block(shard, cc_seqno, std::move(prev_blocks), td::Timestamp::in(10.0), std::move(new_promise));
}

void CollatorNode::generate_block(ShardIdFull shard, CatchainSeqno cc_seqno, std::vector<BlockIdExt> prev_blocks,
                                  td::Timestamp timeout, td::Promise<BlockCandidate> promise) {
  if (last_masterchain_state_.is_null()) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "not ready"));
    return;
  }
  if (!can_collate_shard(shard)) {
    promise.set_error(td::Status::Error(PSTRING() << "this node can't collate shard " << shard.to_str()));
    return;
  }
  auto it = validator_groups_.find(shard);
  if (it == validator_groups_.end() || it->second.cc_seqno != cc_seqno) {
    TRY_RESULT_PROMISE(promise, future_validator_group, get_future_validator_group(shard, cc_seqno));
    future_validator_group->promises.push_back([=, SelfId = actor_id(this), prev_blocks = std::move(prev_blocks),
                                                promise = std::move(promise)](td::Result<td::Unit> R) mutable {
      if (R.is_error()) {
        promise.set_error(R.move_as_error());
        return;
      }
      if (timeout.is_in_past()) {
        promise.set_error(td::Status::Error(ErrorCode::timeout));
        return;
      }
      td::actor::send_closure(SelfId, &CollatorNode::generate_block, shard, cc_seqno, std::move(prev_blocks), timeout,
                              std::move(promise));
    });
    return;
  }
  ValidatorGroupInfo& validator_group_info = it->second;
  BlockSeqno block_seqno = prev_blocks.at(0).seqno() + 1;
  if (prev_blocks.size() == 2) {
    block_seqno = std::max(block_seqno, prev_blocks.at(1).seqno() + 1);
  }
  if (validator_group_info.next_block_seqno > block_seqno) {
    promise.set_error(td::Status::Error(PSTRING() << "next block seqno " << block_seqno << " is too small, expected "
                                                  << validator_group_info.next_block_seqno));
    return;
  }
  if (validator_group_info.next_block_seqno == block_seqno && validator_group_info.prev != prev_blocks) {
    promise.set_error(td::Status::Error("invalid prev_blocks"));
    return;
  }

  auto cache_entry = validator_group_info.cache[prev_blocks];
  if (cache_entry == nullptr) {
    cache_entry = validator_group_info.cache[prev_blocks] = std::make_shared<CacheEntry>();
  }
  if (cache_entry->result) {
    LOG(INFO) << "generate block query"
              << ": shard=" << shard.to_str() << ", cc_seqno=" << cc_seqno << ", next_block_seqno=" << block_seqno
              << ": using cached result";
    promise.set_result(cache_entry->result.value().clone());
    return;
  }
  cache_entry->promises.push_back(std::move(promise));
  if (cache_entry->started) {
    LOG(INFO) << "generate block query"
              << ": shard=" << shard.to_str() << ", cc_seqno=" << cc_seqno << ", next_block_seqno=" << block_seqno
              << ": collation in progress, waiting";
    return;
  }
  LOG(INFO) << "generate block query"
            << ": shard=" << shard.to_str() << ", cc_seqno=" << cc_seqno << ", next_block_seqno=" << block_seqno
            << ": starting collation";
  cache_entry->started = true;
  cache_entry->block_seqno = block_seqno;
  run_collate_query(
      shard, last_masterchain_state_->get_block_id(), std::move(prev_blocks), Ed25519_PublicKey{td::Bits256::zero()},
      last_masterchain_state_->get_validator_set(shard), opts_->get_collator_options(), manager_, timeout,
      [=, SelfId = actor_id(this), timer = td::Timer{}](td::Result<BlockCandidate> R) {
        LOG(INFO) << "generate block result"
                  << ": shard=" << shard.to_str() << ", cc_seqno=" << cc_seqno << ", next_block_seqno=" << block_seqno
                  << ", time=" << timer.elapsed() << ": " << (R.is_ok() ? "OK" : R.error().to_string());
        td::actor::send_closure(SelfId, &CollatorNode::process_result, cache_entry, std::move(R));
      },
      cache_entry->cancellation_token_source.get_cancellation_token(), CollateMode::skip_store_candidate);
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
  return std::any_of(collating_shards_.begin(), collating_shards_.end(),
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
