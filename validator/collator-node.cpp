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
#include "impl/collator-impl.h"
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
  td::actor::send_closure(adnl_, &adnl::Adnl::subscribe, local_id_,
                          adnl::Adnl::int_to_bytestring(ton_api::collatorNode_ping::ID),
                          std::make_unique<Cb>(actor_id(this)));
  td::actor::send_closure(rldp_, &rldp::Rldp::add_id, adnl::AdnlNodeIdShort(local_id_));
}

void CollatorNode::tear_down() {
  td::actor::send_closure(adnl_, &adnl::Adnl::unsubscribe, local_id_,
                          adnl::Adnl::int_to_bytestring(ton_api::collatorNode_generateBlock::ID));
  td::actor::send_closure(adnl_, &adnl::Adnl::unsubscribe, local_id_,
                          adnl::Adnl::int_to_bytestring(ton_api::collatorNode_ping::ID));
}

void CollatorNode::add_shard(ShardIdFull shard) {
  CHECK(shard.is_valid_ext() && !shard.is_masterchain());
  if (std::ranges::find(collating_shards_, shard) != collating_shards_.end()) {
    return;
  }
  LOG(INFO) << "Collator node: local_id=" << local_id_ << " , shard=" << shard.to_str();
  collating_shards_.push_back(shard);
}

void CollatorNode::del_shard(ShardIdFull shard) {
  auto it = std::ranges::find(collating_shards_, shard);
  if (it != collating_shards_.end()) {
    collating_shards_.erase(it);
  }
}

void CollatorNode::new_masterchain_block_notification(td::Ref<MasterchainState> state) {
  last_masterchain_state_ = state;

  if (state->last_key_block_id().seqno() != last_key_block_seqno_) {
    last_key_block_seqno_ = state->last_key_block_id().seqno();
    mc_config_status_ = check_mc_config();
    if (mc_config_status_.is_error()) {
      LOG(ERROR) << "Cannot validate masterchain config (possibly outdated software):" << mc_config_status_;
    }
  }

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
    if (new_shards.contains(it->first)) {
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

void CollatorNode::update_shard_client_handle(BlockHandle shard_client_handle) {
  shard_client_handle_ = shard_client_handle;
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
            if (!cache_entry->has_external_query_at && cache_entry->has_internal_query_at) {
              LOG(INFO) << "generate block query"
                        << ": shard=" << shard.to_str() << ", cc_seqno=" << cc_seqno
                        << ", next_block_seqno=" << cache_entry->block_seqno
                        << ": nobody asked for block, but we tried to generate it";
            }
            if (cache_entry->has_external_query_at && !cache_entry->has_internal_query_at) {
              LOG(INFO) << "generate block query"
                        << ": shard=" << shard.to_str() << ", cc_seqno=" << cc_seqno
                        << ", next_block_seqno=" << cache_entry->block_seqno
                        << ": somebody asked for block we didn't even tried to generate";
            }
            cache_it = info.cache.erase(cache_it);
            continue;
          }
          if (cache_entry->block_seqno == info.next_block_seqno && cached_prev != info.prev) {
            cache_entry->cancel(td::Status::Error("invalid prev blocks"));
            if (!cache_entry->has_external_query_at && cache_entry->has_internal_query_at) {
              LOG(INFO) << "generate block query"
                        << ": shard=" << shard.to_str() << ", cc_seqno=" << cc_seqno
                        << ", next_block_seqno=" << cache_entry->block_seqno
                        << ": nobody asked for block, but we tried to generate it";
            }
            if (cache_entry->has_external_query_at && !cache_entry->has_internal_query_at) {
              LOG(INFO) << "generate block query"
                        << ": shard=" << shard.to_str() << ", cc_seqno=" << cc_seqno
                        << ", next_block_seqno=" << cache_entry->block_seqno
                        << ": somebody asked for block we didn't even tried to generate";
            }
            cache_it = info.cache.erase(cache_it);
            continue;
          }
          ++cache_it;
        }
        auto S = check_out_of_sync();
        if (S.is_error()) {
          LOG(DEBUG) << "not generating block automatically: " << S;
          return;
        }
        if (mc_config_status_.is_error()) {
          LOG(DEBUG) << "not generating block automatically: unsupported mc config: " << mc_config_status_;
          return;
        }
        generate_block(shard, cc_seqno, info.prev, {}, td::Timestamp::in(10.0), [](td::Result<BlockCandidate>) {});
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
  return create_serialize_tl_object<ton_api::collatorNode_error>(error.code(), error.message().c_str());
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

  for (auto& broadcast_ref : block.out_msg_queue_proof_broadcasts) {
    auto block_state_proof = create_block_state_proof(root).move_as_ok();

    auto& broadcast = broadcast_ref.write();
    broadcast.block_id = block.id;
    broadcast.block_state_proofs = vm::std_boc_serialize(std::move(block_state_proof), 31).move_as_ok();
  }
  return block;
}

void CollatorNode::receive_query(adnl::AdnlNodeIdShort src, td::BufferSlice data,
                                 td::Promise<td::BufferSlice> promise) {
  promise = [promise = std::move(promise)](td::Result<td::BufferSlice> R) mutable {
    if (R.is_error()) {
      if (R.error().code() == ErrorCode::timeout) {
        promise.set_error(R.move_as_error());
      } else {
        promise.set_result(serialize_error(R.move_as_error()));
      }
    } else {
      promise.set_result(R.move_as_ok());
    }
  };
  if (!opts_->check_collator_node_whitelist(src)) {
    promise.set_error(td::Status::Error("not authorized"));
    return;
  }
  if (!validator_adnl_ids_.contains(src)) {
    promise.set_error(td::Status::Error("src is not a validator"));
    return;
  }
  auto r_ping = fetch_tl_object<ton_api::collatorNode_ping>(data, true);
  if (r_ping.is_ok()) {
    process_ping(src, *r_ping.ok_ref(), std::move(promise));
    return;
  }

  TRY_RESULT_PROMISE(promise, f, fetch_tl_object<ton_api::collatorNode_generateBlock>(data, true));
  ShardIdFull shard = create_shard_id(f->shard_);
  CatchainSeqno cc_seqno = f->cc_seqno_;
  std::vector<BlockIdExt> prev_blocks;
  for (const auto& b : f->prev_blocks_) {
    prev_blocks.push_back(create_block_id(b));
  }
  auto priority = BlockCandidatePriority{.round = static_cast<td::uint32>(f->round_),
                                         .first_block_round = static_cast<td::uint32>(f->first_block_round_),
                                         .priority = f->priority_};
  Ed25519_PublicKey creator(f->creator_);
  td::Promise<BlockCandidate> new_promise = [promise = std::move(promise), src,
                                             shard](td::Result<BlockCandidate> R) mutable {
    if (R.is_error()) {
      LOG(INFO) << "collate query from " << src << ", shard=" << shard.to_str() << ": error: " << R.error();
      promise.set_error(R.move_as_error());
    } else {
      LOG(INFO) << "collate query from " << src << ", shard=" << shard.to_str() << ": success";
      promise.set_result(serialize_tl_object(serialize_candidate(R.move_as_ok(), true), true));
    }
  };
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
  generate_block(shard, cc_seqno, std::move(prev_blocks), priority, td::Timestamp::in(10.0), std::move(new_promise));
}

void CollatorNode::generate_block(ShardIdFull shard, CatchainSeqno cc_seqno, std::vector<BlockIdExt> prev_blocks,
                                  std::optional<BlockCandidatePriority> o_priority, td::Timestamp timeout,
                                  td::Promise<BlockCandidate> promise) {
  bool is_external = !o_priority;
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
      td::actor::send_closure(SelfId, &CollatorNode::generate_block, shard, cc_seqno, std::move(prev_blocks),
                              std::move(o_priority), timeout, std::move(promise));
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

  static auto prefix_inner = [](auto& sb, auto& shard, auto cc_seqno, auto block_seqno,
                                const std::optional<BlockCandidatePriority>& o_priority) {
    sb << "generate block query"
       << ": shard=" << shard.to_str() << ", cc_seqno=" << cc_seqno << ", next_block_seqno=" << block_seqno;
    if (o_priority) {
      sb << " external{";
      sb << "round_offset=" << o_priority->round - o_priority->first_block_round
         << ",priority=" << o_priority->priority;
      sb << ",first_block_round=" << o_priority->first_block_round;
      sb << "}";
    } else {
      sb << " internal";
    }
  };
  auto prefix = [&](auto& sb) { prefix_inner(sb, shard, cc_seqno, block_seqno, o_priority); };

  auto cache_entry = validator_group_info.cache[prev_blocks];
  if (cache_entry == nullptr) {
    cache_entry = validator_group_info.cache[prev_blocks] = std::make_shared<CacheEntry>();
  }
  if (is_external && !cache_entry->has_external_query_at) {
    cache_entry->has_external_query_at = td::Timestamp::now();
    if (cache_entry->has_internal_query_at && cache_entry->has_external_query_at) {
      FLOG(INFO) {
        prefix(sb);
        sb << ": got external query "
           << cache_entry->has_external_query_at.at() - cache_entry->has_internal_query_at.at()
           << "s  after internal query [WON]";
      };
    }
  }
  if (!is_external && !cache_entry->has_internal_query_at) {
    cache_entry->has_internal_query_at = td::Timestamp::now();
    if (cache_entry->has_internal_query_at && cache_entry->has_external_query_at) {
      FLOG(INFO) {
        prefix(sb);
        sb << ": got internal query "
           << cache_entry->has_internal_query_at.at() - cache_entry->has_external_query_at.at()
           << "s after external query [LOST]";
      };
    }
  }
  if (cache_entry->result) {
    auto has_result_ago = td::Timestamp::now().at() - cache_entry->has_result_at.at();
    FLOG(INFO) {
      prefix(sb);
      sb << ": using cached result " << " generated " << has_result_ago << "s ago";
      sb << (is_external ? " for external query [WON]" : " for internal query ");
    };

    promise.set_result(cache_entry->result.value().clone());
    return;
  }
  cache_entry->promises.push_back(std::move(promise));

  if (cache_entry->started) {
    FLOG(INFO) {
      prefix(sb);
      sb << ": collation in progress, waiting";
    };
    return;
  }
  FLOG(INFO) {
    prefix(sb);
    sb << ": starting collation";
  };
  cache_entry->started = true;
  cache_entry->block_seqno = block_seqno;
  run_collate_query(
      shard, last_masterchain_state_->get_block_id(), std::move(prev_blocks), Ed25519_PublicKey{td::Bits256::zero()},
      last_masterchain_state_->get_validator_set(shard), opts_->get_collator_options(), manager_, timeout,
      [=, SelfId = actor_id(this), timer = td::Timer{}](td::Result<BlockCandidate> R) {
        FLOG(INFO) {
          prefix_inner(sb, shard, cc_seqno, block_seqno, o_priority);
          sb << timer.elapsed() << ": " << (R.is_ok() ? "OK" : R.error().to_string());
        };
        td::actor::send_closure(SelfId, &CollatorNode::process_result, cache_entry, std::move(R));
      },
      cache_entry->cancellation_token_source.get_cancellation_token(),
      CollateMode::skip_store_candidate | CollateMode::from_collator_node);
}

void CollatorNode::process_result(std::shared_ptr<CacheEntry> cache_entry, td::Result<BlockCandidate> R) {
  if (R.is_error()) {
    cache_entry->started = false;
    for (auto& p : cache_entry->promises) {
      p.set_error(R.error().clone());
    }
  } else {
    cache_entry->result = R.move_as_ok();
    cache_entry->has_result_at = td::Timestamp::now();
    for (auto& p : cache_entry->promises) {
      p.set_result(cache_entry->result.value().clone());
    }
  }
  cache_entry->promises.clear();
}

td::Status CollatorNode::check_out_of_sync() {
  if (last_masterchain_state_.is_null() || !shard_client_handle_) {
    return td::Status::Error("not inited");
  }
  auto now = (UnixTime)td::Clocks::system();
  if (last_masterchain_state_->get_unix_time() < now - 60 || shard_client_handle_->unix_time() < now - 60) {
    return td::Status::Error(PSTRING() << "out of sync: mc " << now - last_masterchain_state_->get_unix_time()
                                       << "s ago, shardclient " << now - shard_client_handle_->unix_time() << "s ago");
  }
  return td::Status::OK();
}

td::Status CollatorNode::check_mc_config() {
  if (last_masterchain_state_.is_null()) {
    return td::Status::Error("not inited");
  }
  TRY_RESULT_PREFIX(
      config,
      block::ConfigInfo::extract_config(last_masterchain_state_->root_cell(), block::ConfigInfo::needCapabilities),
      "cannot unpack masterchain config");
  if (config->get_global_version() > Collator::supported_version()) {
    return td::Status::Error(PSTRING() << "unsupported global version " << config->get_global_version()
                                       << " (supported: " << Collator::supported_version() << ")");
  }
  if (config->get_capabilities() & ~Collator::supported_capabilities()) {
    return td::Status::Error(PSTRING() << "unsupported capabilities " << config->get_capabilities()
                                       << " (supported: " << Collator::supported_capabilities() << ")");
  }
  td::Status S = td::Status::OK();
  config->foreach_config_param([&](int idx, td::Ref<vm::Cell> param) {
    if (idx < 0) {
      return true;
    }
    if (!block::gen::ConfigParam{idx}.validate_ref(1024, std::move(param))) {
      S = td::Status::Error(PSTRING() << "unknown ConfigParam " << idx);
      return false;
    }
    return true;
  });
  return S;
}

void CollatorNode::process_ping(adnl::AdnlNodeIdShort src, ton_api::collatorNode_ping& ping,
                                td::Promise<td::BufferSlice> promise) {
  LOG(DEBUG) << "got ping from " << src;
  TRY_STATUS_PROMISE(promise, check_out_of_sync());
  TRY_STATUS_PROMISE_PREFIX(promise, mc_config_status_.clone(), "unsupported mc config: ");
  promise.set_result(create_serialize_tl_object<ton_api::collatorNode_pong>(0));
}

bool CollatorNode::can_collate_shard(ShardIdFull shard) const {
  return std::ranges::any_of(collating_shards_,
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
