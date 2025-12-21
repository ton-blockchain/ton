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
#include "impl/collator-impl.h"
#include "impl/shard.hpp"
#include "td/utils/lz4.h"
#include "ton/ton-tl.hpp"

#include "block-auto.h"
#include "block-db.h"
#include "checksum.h"
#include "collator-node.hpp"
#include "fabric.h"
#include "utils.hpp"

namespace ton::validator {

CollatorNode::CollatorNode(adnl::AdnlNodeIdShort local_id, td::Ref<ValidatorManagerOptions> opts,
                           td::actor::ActorId<ValidatorManager> manager, td::actor::ActorId<adnl::Adnl> adnl,
                           td::actor::ActorId<rldp2::Rldp> rldp)
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
                          adnl::Adnl::int_to_bytestring(ton_api::collatorNode_generateBlockOptimistic::ID),
                          std::make_unique<Cb>(actor_id(this)));
  td::actor::send_closure(adnl_, &adnl::Adnl::subscribe, local_id_,
                          adnl::Adnl::int_to_bytestring(ton_api::collatorNode_ping::ID),
                          std::make_unique<Cb>(actor_id(this)));
  td::actor::send_closure(rldp_, &rldp2::Rldp::add_id, adnl::AdnlNodeIdShort(local_id_));
}

void CollatorNode::tear_down() {
  td::actor::send_closure(adnl_, &adnl::Adnl::unsubscribe, local_id_,
                          adnl::Adnl::int_to_bytestring(ton_api::collatorNode_generateBlock::ID));
  td::actor::send_closure(adnl_, &adnl::Adnl::unsubscribe, local_id_,
                          adnl::Adnl::int_to_bytestring(ton_api::collatorNode_generateBlockOptimistic::ID));
  td::actor::send_closure(adnl_, &adnl::Adnl::unsubscribe, local_id_,
                          adnl::Adnl::int_to_bytestring(ton_api::collatorNode_ping::ID));
}

void CollatorNode::add_shard(ShardIdFull shard) {
  CHECK(shard.is_valid_ext() && !shard.is_masterchain());
  if (std::find(collating_shards_.begin(), collating_shards_.end(), shard) != collating_shards_.end()) {
    return;
  }
  LOG(INFO) << "Collator node: local_id=" << local_id_ << " , shard=" << shard.to_str();
  collating_shards_.push_back(shard);
  if (last_masterchain_state_.is_null()) {
    return;
  }
  for (auto& [group_shard, validator_group] : validator_groups_) {
    if (validator_group.actor.empty() && shard_intersects(shard, group_shard)) {
      validator_group.actor = td::actor::create_actor<CollatorNodeSession>(
          PSTRING() << "collatornode" << shard.to_str(), shard, validator_group.prev,
          last_masterchain_state_->get_validator_set(group_shard), last_masterchain_state_->get_block_id(),
          can_generate(), last_masterchain_state_, local_id_, opts_, manager_, adnl_, rldp_);
    }
  }
}

void CollatorNode::del_shard(ShardIdFull shard) {
  auto it = std::find(collating_shards_.begin(), collating_shards_.end(), shard);
  if (it != collating_shards_.end()) {
    collating_shards_.erase(it);
  }
  for (auto& [group_shard, validator_group] : validator_groups_) {
    if (!validator_group.actor.empty() && shard_intersects(shard, group_shard) && !can_collate_shard(group_shard)) {
      validator_group.actor = {};
    }
  }
}

void CollatorNode::update_options(td::Ref<ValidatorManagerOptions> opts) {
  for (auto& [_, shard] : validator_groups_) {
    if (!shard.actor.empty()) {
      td::actor::send_closure(shard.actor, &CollatorNodeSession::update_options, opts);
    }
  }
  opts_ = std::move(opts);
}

void CollatorNode::new_masterchain_block_notification(td::Ref<MasterchainState> state) {
  last_masterchain_state_ = state;

  if (state->last_key_block_id().seqno() != last_key_block_seqno_) {
    last_key_block_seqno_ = state->last_key_block_id().seqno();
    mc_config_status_ = check_mc_config();
    if (mc_config_status_.is_error()) {
      LOG(ERROR) << "Cannot validate masterchain config (possibly outdated software): " << mc_config_status_;
    }

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
    for (auto& [_, group] : validator_groups_) {
      if (!group.actor.empty()) {
        td::actor::send_closure(group.actor, &CollatorNodeSession::update_masterchain_config, state);
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
  for (auto it = validator_groups_.begin(); it != validator_groups_.end();) {
    if (new_shards.contains(it->first)) {
      ++it;
    } else {
      it = validator_groups_.erase(it);
    }
  }
  for (auto& [shard, prev] : new_shards) {
    auto validator_set = state->get_validator_set(shard);
    CatchainSeqno cc_seqno = validator_set->get_catchain_seqno();
    auto [it, created] = validator_groups_.emplace(shard, ValidatorGroupInfo{});
    it->second.prev = std::move(prev);
    if (created || it->second.cc_seqno != cc_seqno) {
      it->second.cc_seqno = cc_seqno;
      if (can_collate_shard(shard)) {
        it->second.actor = td::actor::create_actor<CollatorNodeSession>(
            PSTRING() << "collatornode" << shard.to_str(), shard, it->second.prev, validator_set,
            last_masterchain_state_->get_block_id(), can_generate(), last_masterchain_state_, local_id_, opts_,
            manager_, adnl_, rldp_);
      }
    } else if (!it->second.actor.empty() && prev.size() == 1) {
      td::actor::send_closure(it->second.actor, &CollatorNodeSession::new_shard_block_accepted, prev[0],
                              can_generate());
    }
    auto it2 = future_validator_groups_.find({shard, cc_seqno});
    if (it2 != future_validator_groups_.end()) {
      FutureValidatorGroup& future_group = it2->second;
      if (!it->second.actor.empty()) {
        for (const BlockIdExt& block_id : future_group.pending_blocks) {
          td::actor::send_closure(it->second.actor, &CollatorNodeSession::new_shard_block_accepted, block_id,
                                  can_generate());
        }
      }
      for (auto& promise : future_group.promises) {
        promise.set_value(td::Unit());
      }
      future_validator_groups_.erase(it2);
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

void CollatorNode::new_shard_block_accepted(BlockIdExt block_id, CatchainSeqno cc_seqno) {
  if (!can_collate_shard(block_id.shard_full())) {
    return;
  }
  auto it = validator_groups_.find(block_id.shard_full());
  if (it == validator_groups_.end() || it->second.cc_seqno != cc_seqno) {
    auto future_group = get_future_validator_group(block_id.shard_full(), cc_seqno);
    if (future_group.is_error()) {
      LOG(DEBUG) << "Dropping new shard block " << block_id.to_str() << " cc_seqno=" << cc_seqno << " : "
                 << future_group.error();
    } else {
      LOG(DEBUG) << "New shard block in future validator group " << block_id.to_str() << " cc_seqno=" << cc_seqno;
      future_group.ok()->pending_blocks.push_back(block_id);
    }
    return;
  }
  if (!it->second.actor.empty()) {
    td::actor::send_closure(it->second.actor, &CollatorNodeSession::new_shard_block_accepted, block_id, can_generate());
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
    return td::Status::Error(PSTRING() << "cc_seqno " << cc_seqno << " for shard " << shard.to_str()
                                       << " is outdated (current is " << it->second.cc_seqno << ")");
  }
  if (cc_seqno - it->second.cc_seqno > 1) {  // future validator group, cc_seqno too big
    return td::Status::Error(PSTRING() << "cc_seqno " << cc_seqno << " for shard " << shard.to_str()
                                       << " is too big (currently known is " << it->second.cc_seqno << ")");
  }
  // future validator group
  return &future_validator_groups_[{shard, cc_seqno}];
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

  bool is_optimistic = false;
  ShardIdFull shard;
  CatchainSeqno cc_seqno;
  std::vector<BlockIdExt> prev_blocks;
  BlockCandidatePriority priority;
  Ed25519_PublicKey creator;
  if (auto R = fetch_tl_object<ton_api::collatorNode_generateBlock>(data, true); R.is_ok()) {
    auto f = R.move_as_ok();
    shard = create_shard_id(f->shard_);
    cc_seqno = f->cc_seqno_;
    for (const auto& b : f->prev_blocks_) {
      prev_blocks.push_back(create_block_id(b));
    }
    priority = BlockCandidatePriority{.round = static_cast<td::uint32>(f->round_),
                                      .first_block_round = static_cast<td::uint32>(f->first_block_round_),
                                      .priority = f->priority_};
    creator = Ed25519_PublicKey(f->creator_);
  } else if (auto R = fetch_tl_object<ton_api::collatorNode_generateBlockOptimistic>(data, true); R.is_ok()) {
    is_optimistic = true;
    auto f = R.move_as_ok();
    shard = create_shard_id(f->shard_);
    cc_seqno = f->cc_seqno_;
    for (const auto& b : f->prev_blocks_) {
      prev_blocks.push_back(create_block_id(b));
    }
    priority = BlockCandidatePriority{.round = static_cast<td::uint32>(f->round_),
                                      .first_block_round = static_cast<td::uint32>(f->first_block_round_),
                                      .priority = f->priority_};
    creator = Ed25519_PublicKey(f->creator_);
  } else {
    promise.set_error(td::Status::Error("cannot parse request"));
    return;
  }
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
  new_promise = [new_promise = std::move(new_promise), creator, local_id = local_id_,
                 manager = manager_](td::Result<BlockCandidate> R) mutable {
    TRY_RESULT_PROMISE(new_promise, block, std::move(R));

    CollatorNodeResponseStats stats;
    stats.self = local_id.pubkey_hash();
    stats.validator_id = PublicKey(pubkeys::Ed25519(creator)).compute_short_id();
    stats.original_block_id = block.id;
    stats.collated_data_hash = block.collated_file_hash;

    CatchainSeqno cc_seqno;
    td::uint32 val_set_hash;
    block = change_creator(std::move(block), creator, cc_seqno, val_set_hash);

    stats.block_id = block.id;
    stats.timestamp = td::Clocks::system();
    td::actor::send_closure(manager, &ValidatorManager::log_collator_node_response_stats, std::move(stats));

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
  LOG(INFO) << "got adnl query from " << src << ": shard=" << shard.to_str() << ", cc_seqno=" << cc_seqno
            << (is_optimistic ? ", optimistic" : "");
  process_generate_block_query(src, shard, cc_seqno, std::move(prev_blocks), priority, is_optimistic,
                               td::Timestamp::in(10.0), std::move(new_promise));
}

void CollatorNode::process_generate_block_query(adnl::AdnlNodeIdShort src, ShardIdFull shard, CatchainSeqno cc_seqno,
                                                std::vector<BlockIdExt> prev_blocks, BlockCandidatePriority priority,
                                                bool is_optimistic, td::Timestamp timeout,
                                                td::Promise<BlockCandidate> promise) {
  if (last_masterchain_state_.is_null()) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "not ready"));
    return;
  }
  if (timeout.is_in_past()) {
    promise.set_error(td::Status::Error(ErrorCode::timeout));
    return;
  }
  auto it = validator_groups_.find(shard);
  if (it == validator_groups_.end() || it->second.cc_seqno != cc_seqno) {
    TRY_RESULT_PROMISE(promise, future_validator_group, get_future_validator_group(shard, cc_seqno));
    future_validator_group->promises.push_back([=, SelfId = actor_id(this), prev_blocks = std::move(prev_blocks),
                                                promise = std::move(promise)](td::Result<td::Unit> R) mutable {
      TRY_STATUS_PROMISE(promise, R.move_as_status());
      td::actor::send_closure(SelfId, &CollatorNode::process_generate_block_query, src, shard, cc_seqno,
                              std::move(prev_blocks), std::move(priority), is_optimistic, timeout, std::move(promise));
    });
    return;
  }
  ValidatorGroupInfo& validator_group_info = it->second;
  if (validator_group_info.actor.empty()) {
    promise.set_error(td::Status::Error(PSTRING() << "cannot collate shard " << shard.to_str()));
    return;
  }
  td::actor::send_closure(validator_group_info.actor, &CollatorNodeSession::process_request, src,
                          std::move(prev_blocks), priority, is_optimistic, timeout, std::move(promise));
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
      block::ConfigInfo::extract_config(last_masterchain_state_->root_cell(), last_masterchain_state_->get_block_id(),
                                        block::ConfigInfo::needCapabilities),
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
  auto pong = create_tl_object<ton_api::collatorNode_pong>();
  if (ping.flags_ & ton_api::collatorNode_pong::VERSION_MASK) {
    pong->flags_ |= ton_api::collatorNode_pong::VERSION_MASK;
    pong->version_ = COLLATOR_NODE_VERSION;
  }
  promise.set_result(serialize_tl_object(pong, true));
}

bool CollatorNode::can_collate_shard(ShardIdFull shard) const {
  return std::any_of(collating_shards_.begin(), collating_shards_.end(),
                     [&](const ShardIdFull& our_shard) { return shard_intersects(shard, our_shard); });
}

}  // namespace ton::validator
