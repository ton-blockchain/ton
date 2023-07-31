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
#include "out-msg-queue-proof.hpp"
#include "interfaces/proof.h"
#include "shard.hpp"
#include "vm/cells/MerkleProof.h"
#include "common/delay.h"
#include "interfaces/validator-manager.h"
#include "block/block-parse.h"
#include "block/block-auto.h"
#include "output-queue-merger.h"

namespace ton {

namespace validator {

static td::Status check_no_prunned(const Ref<vm::Cell>& cell) {
  if (cell.is_null()) {
    return td::Status::OK();
  }
  TRY_RESULT(loaded_cell, cell->load_cell());
  if (loaded_cell.data_cell->get_level() > 0) {
    return td::Status::Error("prunned branch");
  }
  return td::Status::OK();
}

static td::Status check_no_prunned(const vm::CellSlice& cs) {
  for (unsigned i = 0; i < cs.size_refs(); ++i) {
    TRY_STATUS(check_no_prunned(cs.prefetch_ref(i)));
  }
  return td::Status::OK();
}

static td::Result<std::vector<td::int32>> process_queue(
    ShardIdFull dst_shard, std::vector<std::pair<BlockIdExt, block::gen::OutMsgQueueInfo::Record>> blocks,
    block::ImportedMsgQueueLimits limits) {
  td::uint64 estimated_proof_size = 0;

  td::HashSet<vm::Cell::Hash> visited;
  std::function<void(const vm::CellSlice&)> dfs_cs;
  auto dfs = [&](const Ref<vm::Cell>& cell) {
    if (cell.is_null() || !visited.insert(cell->get_hash()).second) {
      return;
    }
    dfs_cs(vm::CellSlice(vm::NoVm(), cell));
  };
  dfs_cs = [&](const vm::CellSlice& cs) {
    // Based on BlockLimitStatus::estimate_block_size
    estimated_proof_size += 12 + (cs.size() + 7) / 8 + cs.size_refs() * 3;
    for (unsigned i = 0; i < cs.size_refs(); i++) {
      dfs(cs.prefetch_ref(i));
    }
  };
  std::vector<block::OutputQueueMerger::Neighbor> neighbors;
  for (auto& b : blocks) {
    TRY_STATUS_PREFIX(check_no_prunned(*b.second.proc_info), "invalid proc_info proof: ")
    TRY_STATUS_PREFIX(check_no_prunned(*b.second.ihr_pending), "invalid ihr_pending proof: ")
    dfs_cs(*b.second.proc_info);
    dfs_cs(*b.second.ihr_pending);
    neighbors.emplace_back(b.first, b.second.out_queue->prefetch_ref());
  }

  block::OutputQueueMerger queue_merger{dst_shard, std::move(neighbors)};
  std::vector<td::int32> msg_count(blocks.size());
  td::int32 msg_count_total = 0;
  bool limit_reached = false;

  while (!queue_merger.is_eof()) {
    auto kv = queue_merger.extract_cur();
    queue_merger.next();
    block::EnqueuedMsgDescr enq;
    auto msg = kv->msg;
    if (!enq.unpack(msg.write())) {
      return td::Status::Error("cannot unpack EnqueuedMsgDescr");
    }
    if (limit_reached) {
      break;
    }
    ++msg_count[kv->source];
    ++msg_count_total;

    // TODO: Get processed_upto from destination shard (in request?)
    /*
    // Parse message to check if it was processed (as in Collator::process_inbound_message)
    ton::LogicalTime enqueued_lt = kv->msg->prefetch_ulong(64);
    auto msg_env = kv->msg->prefetch_ref();
    block::tlb::MsgEnvelope::Record_std env;
    if (!tlb::unpack_cell(msg_env, env)) {
      return td::Status::Error("cannot unpack MsgEnvelope of an internal message");
    }
    vm::CellSlice cs{vm::NoVmOrd{}, env.msg};
    block::gen::CommonMsgInfo::Record_int_msg_info info;
    if (!tlb::unpack(cs, info)) {
      return td::Status::Error("cannot unpack CommonMsgInfo of an internal message");
    }
    auto src_prefix = block::tlb::MsgAddressInt::get_prefix(info.src);
    auto dest_prefix = block::tlb::MsgAddressInt::get_prefix(info.dest);
    auto cur_prefix = block::interpolate_addr(src_prefix, dest_prefix, env.cur_addr);
    auto next_prefix = block::interpolate_addr(src_prefix, dest_prefix, env.next_addr);
    block::EnqueuedMsgDescr descr{cur_prefix, next_prefix, kv->lt, enqueued_lt, env.msg->get_hash().bits()};
    if (dst_processed_upto->already_processed(descr)) {
    } else {
    }*/

    dfs_cs(*kv->msg);
    TRY_STATUS_PREFIX(check_no_prunned(*kv->msg), "invalid message proof: ")
    if (estimated_proof_size >= limits.max_bytes || msg_count_total >= (long long)limits.max_msgs) {
      limit_reached = true;
    }
  }
  if (!limit_reached) {
    std::fill(msg_count.begin(), msg_count.end(), -1);
  }
  return msg_count;
}

td::Result<tl_object_ptr<ton_api::tonNode_outMsgQueueProof>> OutMsgQueueProof::build(
    ShardIdFull dst_shard, std::vector<OneBlock> blocks, block::ImportedMsgQueueLimits limits) {
  if (!dst_shard.is_valid_ext()) {
    return td::Status::Error("invalid shard");
  }
  if (blocks.empty()) {
    return create_tl_object<ton_api::tonNode_outMsgQueueProof>(td::BufferSlice{}, td::BufferSlice{},
                                                               std::vector<td::int32>{});
  }

  std::vector<td::Ref<vm::Cell>> block_state_proofs;
  for (auto& block : blocks) {
    if (block.id.seqno() != 0) {
      if (block.block_root.is_null()) {
        return td::Status::Error("block is null");
      }
      TRY_RESULT(proof, create_block_state_proof(block.block_root));
      block_state_proofs.push_back(std::move(proof));
    }
    if (!block::ShardConfig::is_neighbor(dst_shard, block.id.shard_full())) {
      return td::Status::Error("shards are not neighbors");
    }
  }
  TRY_RESULT(block_state_proof, vm::std_boc_serialize_multi(block_state_proofs));

  vm::Dictionary states_dict_pure{32};
  for (size_t i = 0; i < blocks.size(); ++i) {
    if (blocks[i].state_root.is_null()) {
      return td::Status::Error("state is null");
    }
    states_dict_pure.set_ref(td::BitArray<32>{(long long)i}, blocks[i].state_root);
  }

  vm::MerkleProofBuilder mpb{states_dict_pure.get_root_cell()};
  vm::Dictionary states_dict{mpb.root(), 32};
  std::vector<std::pair<BlockIdExt, block::gen::OutMsgQueueInfo::Record>> data(blocks.size());
  for (size_t i = 0; i < blocks.size(); ++i) {
    data[i].first = blocks[i].id;
    TRY_RESULT(state, ShardStateQ::fetch(blocks[i].id, {}, states_dict.lookup_ref(td::BitArray<32>{(long long)i})));
    TRY_RESULT(outq_descr, state->message_queue());
    block::gen::OutMsgQueueInfo::Record qinfo;
    if (!tlb::unpack_cell(outq_descr->root_cell(), data[i].second)) {
      return td::Status::Error("invalid message queue");
    }
  }
  TRY_RESULT(msg_count, process_queue(dst_shard, std::move(data), limits));

  TRY_RESULT(proof, mpb.extract_proof());
  vm::Dictionary states_dict_proof{vm::CellSlice{vm::NoVm(), proof}.prefetch_ref(), 32};
  std::vector<td::Ref<vm::Cell>> state_proofs;
  for (size_t i = 0; i < blocks.size(); ++i) {
    td::Ref<vm::Cell> proof_raw = states_dict_proof.lookup_ref(td::BitArray<32>{(long long)i});
    CHECK(proof_raw.not_null());
    state_proofs.push_back(vm::CellBuilder::create_merkle_proof(proof_raw));
  }
  TRY_RESULT(queue_proof, vm::std_boc_serialize_multi(state_proofs));
  return create_tl_object<ton_api::tonNode_outMsgQueueProof>(std::move(queue_proof), std::move(block_state_proof),
                                                             std::move(msg_count));
}

td::Result<std::vector<td::Ref<OutMsgQueueProof>>> OutMsgQueueProof::fetch(ShardIdFull dst_shard,
                                                                           std::vector<BlockIdExt> blocks,
                                                                           block::ImportedMsgQueueLimits limits,
                                                                           const ton_api::tonNode_outMsgQueueProof& f) {
  try {
    std::vector<td::Ref<OutMsgQueueProof>> res;
    TRY_RESULT(queue_proofs, vm::std_boc_deserialize_multi(f.queue_proofs_, (int)blocks.size()));
    TRY_RESULT(block_state_proofs, vm::std_boc_deserialize_multi(f.block_state_proofs_, (int)blocks.size()));
    if (queue_proofs.size() != blocks.size()) {
      return td::Status::Error("invalid size of queue_proofs");
    }
    if (f.msg_counts_.size() != blocks.size()) {
      return td::Status::Error("invalid size of msg_counts");
    }
    size_t j = 0;
    std::vector<std::pair<BlockIdExt, block::gen::OutMsgQueueInfo::Record>> data(blocks.size());
    for (size_t i = 0; i < blocks.size(); ++i) {
      td::Bits256 state_root_hash;
      Ref<vm::Cell> block_state_proof = {};
      if (blocks[i].seqno() == 0) {
        state_root_hash = blocks[i].root_hash;
      } else {
        if (j == block_state_proofs.size()) {
          return td::Status::Error("invalid size of block_state_proofs");
        }
        block_state_proof = block_state_proofs[j++];
        TRY_RESULT_ASSIGN(state_root_hash, unpack_block_state_proof(blocks[i], block_state_proof));
      }
      auto state_root = vm::MerkleProof::virtualize(queue_proofs[i], 1);
      if (state_root->get_hash().as_slice() != state_root_hash.as_slice()) {
        return td::Status::Error("state root hash mismatch");
      }
      res.emplace_back(true, blocks[i], state_root, block_state_proof, f.msg_counts_[i]);

      data[i].first = blocks[i];
      TRY_RESULT(state, ShardStateQ::fetch(blocks[i], {}, state_root));
      TRY_RESULT(outq_descr, state->message_queue());
      block::gen::OutMsgQueueInfo::Record qinfo;
      if (!tlb::unpack_cell(outq_descr->root_cell(), data[i].second)) {
        return td::Status::Error("invalid message queue");
      }
    }
    if (j != block_state_proofs.size()) {
      return td::Status::Error("invalid size of block_state_proofs");
    }
    TRY_RESULT(msg_count, process_queue(dst_shard, std::move(data), limits));
    if (msg_count != f.msg_counts_) {
      return td::Status::Error("incorrect msg_count");
    }
    return res;
  } catch (vm::VmVirtError& err) {
    return td::Status::Error(PSTRING() << "invalid proof: " << err.get_msg());
  }
}

void OutMsgQueueImporter::new_masterchain_block_notification(td::Ref<MasterchainState> state,
                                                             std::set<ShardIdFull> collating_shards) {
  last_masterchain_state_ = state;
  if (collating_shards.empty() || state->get_unix_time() < (td::uint32)td::Clocks::system() - 20) {
    return;
  }
  auto can_collate_shard = [&](const ShardIdFull& shard) -> bool {
    return std::any_of(collating_shards.begin(), collating_shards.end(),
                       [&](ShardIdFull our_shard) { return shard_intersects(shard, our_shard); });
  };
  auto shards = state->get_shards();
  auto process_dst_shard = [&](const ShardIdFull& dst_shard) {
    if (!can_collate_shard(dst_shard)) {
      return;
    }
    std::vector<BlockIdExt> top_blocks;
    for (const auto& shard : shards) {
      if (block::ShardConfig::is_neighbor(dst_shard, shard->shard())) {
        top_blocks.push_back(shard->top_block_id());
      }
    }
    get_neighbor_msg_queue_proofs(dst_shard, std::move(top_blocks), td::Timestamp::in(15.0),
                                  [](td::Result<std::map<BlockIdExt, td::Ref<OutMsgQueueProof>>>) {});
  };
  for (const auto& shard : shards) {
    if (shard->before_merge()) {
      if (is_left_child(shard->shard())) {
        process_dst_shard(shard_parent(shard->shard()));
      }
    } else if (shard->before_split()) {
      process_dst_shard(shard_child(shard->shard(), true));
      process_dst_shard(shard_child(shard->shard(), false));
    } else {
      process_dst_shard(shard->shard());
    }
  }
}

void OutMsgQueueImporter::get_neighbor_msg_queue_proofs(
    ShardIdFull dst_shard, std::vector<BlockIdExt> blocks, td::Timestamp timeout,
    td::Promise<std::map<BlockIdExt, td::Ref<OutMsgQueueProof>>> promise) {
  std::sort(blocks.begin(), blocks.end());
  auto entry = cache_[{dst_shard, blocks}];
  if (entry) {
    if (entry->done) {
      promise.set_result(entry->result);
      alarm_timestamp().relax(entry->timeout = td::Timestamp::in(CACHE_TTL));
    } else {
      entry->timeout = std::max(entry->timeout, timeout);
      entry->promises.emplace_back(std::move(promise), timeout);
      alarm_timestamp().relax(timeout);
    }
    return;
  }

  LOG(DEBUG) << "Importing neighbor msg queues for shard " << dst_shard.to_str() << ", " << blocks.size() << " blocks";

  cache_[{dst_shard, blocks}] = entry = std::make_shared<CacheEntry>();
  entry->dst_shard = dst_shard;
  entry->blocks = blocks;
  entry->promises.emplace_back(std::move(promise), timeout);
  alarm_timestamp().relax(entry->timeout = timeout);

  std::map<ShardIdFull, std::vector<BlockIdExt>> new_queries;
  for (const BlockIdExt& block : blocks) {
    if (opts_->need_monitor(block.shard_full(), last_masterchain_state_)) {
      ++entry->pending;
      get_proof_local(entry, block);
    } else {
      ShardIdFull prefix = block.shard_full();
      int min_split = last_masterchain_state_->monitor_min_split_depth(prefix.workchain);
      if (prefix.pfx_len() > min_split) {
        prefix = shard_prefix(prefix, min_split);
      }
      new_queries[prefix].push_back(block);
    }
  };
  auto limits = last_masterchain_state_->get_imported_msg_queue_limits(dst_shard.workchain);
  for (auto& p : new_queries) {
    ++entry->pending;
    for (size_t i = 0; i < p.second.size(); i += 16) {
      size_t j = std::min(i + 16, p.second.size());
      get_proof_import(entry, std::vector<BlockIdExt>(p.second.begin() + i, p.second.begin() + j), limits);
    }
  }
  if (entry->pending == 0) {
    finish_query(entry);
  }
}

void OutMsgQueueImporter::get_proof_local(std::shared_ptr<CacheEntry> entry, BlockIdExt block) {
  if (!check_timeout(entry)) {
    return;
  }
  td::actor::send_closure(
      manager_, &ValidatorManager::wait_block_state_short, block, 0, entry->timeout,
      [=, SelfId = actor_id(this), manager = manager_, timeout = entry->timeout,
       retry_after = td::Timestamp::in(0.5)](td::Result<Ref<ShardState>> R) mutable {
        if (R.is_error()) {
          LOG(DEBUG) << "Failed to get block state for " << block.to_str() << ": " << R.move_as_error();
          delay_action([=]() { td::actor::send_closure(SelfId, &OutMsgQueueImporter::get_proof_local, entry, block); },
                       retry_after);
          return;
        }
        auto state = R.move_as_ok();
        if (block.seqno() == 0) {
          std::vector<td::Ref<OutMsgQueueProof>> proof = {
              td::Ref<OutMsgQueueProof>(true, block, state->root_cell(), td::Ref<vm::Cell>{})};
          td::actor::send_closure(SelfId, &OutMsgQueueImporter::got_proof, entry, std::move(proof));
          return;
        }
        td::actor::send_closure(
            manager, &ValidatorManager::wait_block_data_short, block, 0, timeout,
            [=](td::Result<Ref<BlockData>> R) mutable {
              if (R.is_error()) {
                LOG(DEBUG) << "Failed to get block data for " << block.to_str() << ": " << R.move_as_error();
                delay_action(
                    [=]() { td::actor::send_closure(SelfId, &OutMsgQueueImporter::get_proof_local, entry, block); },
                    retry_after);
                return;
              }
              Ref<vm::Cell> block_state_proof = create_block_state_proof(R.ok()->root_cell()).move_as_ok();
              std::vector<td::Ref<OutMsgQueueProof>> proof = {
                  td::Ref<OutMsgQueueProof>(true, block, state->root_cell(), std::move(block_state_proof))};
              td::actor::send_closure(SelfId, &OutMsgQueueImporter::got_proof, entry, std::move(proof));
            });
      });
}

void OutMsgQueueImporter::get_proof_import(std::shared_ptr<CacheEntry> entry, std::vector<BlockIdExt> blocks,
                                           block::ImportedMsgQueueLimits limits) {
  if (!check_timeout(entry)) {
    return;
  }
  td::actor::send_closure(
      manager_, &ValidatorManager::send_get_out_msg_queue_proof_request, entry->dst_shard, blocks, limits,
      [=, SelfId = actor_id(this), retry_after = td::Timestamp::in(0.5),
       dst_shard = entry->dst_shard](td::Result<std::vector<td::Ref<OutMsgQueueProof>>> R) {
        if (R.is_error()) {
          LOG(DEBUG) << "Failed to get out msg queue for " << dst_shard.to_str() << ": " << R.move_as_error();
          delay_action(
              [=]() {
                td::actor::send_closure(SelfId, &OutMsgQueueImporter::get_proof_import, entry, std::move(blocks),
                                        limits);
              },
              retry_after);
          return;
        }
        td::actor::send_closure(SelfId, &OutMsgQueueImporter::got_proof, entry, R.move_as_ok());
      });
}

void OutMsgQueueImporter::got_proof(std::shared_ptr<CacheEntry> entry, std::vector<td::Ref<OutMsgQueueProof>> proofs) {
  if (!check_timeout(entry)) {
    return;
  }
  for (auto& p : proofs) {
    entry->result[p->block_id_] = std::move(p);
  }
  CHECK(entry->pending > 0);
  if (--entry->pending == 0) {
    finish_query(entry);
  }
}

void OutMsgQueueImporter::finish_query(std::shared_ptr<CacheEntry> entry) {
  LOG(DEBUG) << "Done importing neighbor msg queues for shard " << entry->dst_shard.to_str() << ", "
             << entry->blocks.size() << " blocks in " << entry->timer.elapsed() << "s";
  entry->done = true;
  alarm_timestamp().relax(entry->timeout = td::Timestamp::in(CACHE_TTL));
  for (auto& p : entry->promises) {
    p.first.set_result(entry->result);
  }
  entry->promises.clear();
}

bool OutMsgQueueImporter::check_timeout(std::shared_ptr<CacheEntry> entry) {
  if (entry->timeout.is_in_past()) {
    LOG(DEBUG) << "Aborting importing neighbor msg queues for shard " << entry->dst_shard.to_str() << ", "
               << entry->blocks.size() << " blocks: timeout";
    for (auto& p : entry->promises) {
      p.first.set_error(td::Status::Error(ErrorCode::timeout, "timeout"));
    }
    entry->promises.clear();
    auto it = cache_.find({entry->dst_shard, entry->blocks});
    if (it != cache_.end() && it->second == entry) {
      cache_.erase(it);
    }
    return false;
  }
  return true;
}

void OutMsgQueueImporter::alarm() {
  auto it = cache_.begin();
  while (it != cache_.end()) {
    auto& promises = it->second->promises;
    if (it->second->timeout.is_in_past()) {
      if (!it->second->done) {
        LOG(DEBUG) << "Aborting importing neighbor msg queues for shard " << it->second->dst_shard.to_str() << ", "
                   << it->second->blocks.size() << " blocks: timeout";
        for (auto& p : promises) {
          p.first.set_error(td::Status::Error(ErrorCode::timeout, "timeout"));
        }
        promises.clear();
      }
      it = cache_.erase(it);
      continue;
    }
    alarm_timestamp().relax(it->second->timeout);
    size_t j = 0;
    for (auto& p : promises) {
      if (p.second.is_in_past()) {
        p.first.set_error(td::Status::Error(ErrorCode::timeout, "timeout"));
      } else {
        alarm_timestamp().relax(p.second);
        promises[j++] = std::move(p);
      }
    }
    promises.resize(j);
    ++it;
  }
}

void BuildOutMsgQueueProof::abort_query(td::Status reason) {
  if (promise_) {
    LOG(DEBUG) << "failed to build msg queue proof to " << dst_shard_.to_str() << ": " << reason;
    promise_.set_error(
        reason.move_as_error_prefix(PSTRING() << "failed to build msg queue proof to " << dst_shard_.to_str() << ": "));
  }
  stop();
}

void BuildOutMsgQueueProof::start_up() {
  for (size_t i = 0; i < blocks_.size(); ++i) {
    BlockIdExt id = blocks_[i].id;
    ++pending;
    td::actor::send_closure(manager_, &ValidatorManagerInterface::get_shard_state_from_db_short, id,
                            [SelfId = actor_id(this), i](td::Result<Ref<ShardState>> R) {
                              if (R.is_error()) {
                                td::actor::send_closure(SelfId, &BuildOutMsgQueueProof::abort_query,
                                                        R.move_as_error_prefix("failed to get shard state: "));
                              } else {
                                td::actor::send_closure(SelfId, &BuildOutMsgQueueProof::got_state_root, i,
                                                        R.move_as_ok()->root_cell());
                              }
                            });
    if (id.seqno() != 0) {
      ++pending;
      td::actor::send_closure(manager_, &ValidatorManagerInterface::get_block_data_from_db_short, id,
                              [SelfId = actor_id(this), i](td::Result<Ref<BlockData>> R) {
                                if (R.is_error()) {
                                  td::actor::send_closure(SelfId, &BuildOutMsgQueueProof::abort_query,
                                                          R.move_as_error_prefix("failed to get block data: "));
                                } else {
                                  td::actor::send_closure(SelfId, &BuildOutMsgQueueProof::got_block_root, i,
                                                          R.move_as_ok()->root_cell());
                                }
                              });
    }
  }
  if (pending == 0) {
    build_proof();
  }
}

void BuildOutMsgQueueProof::got_state_root(size_t i, Ref<vm::Cell> root) {
  blocks_[i].state_root = std::move(root);
  if (--pending == 0) {
    build_proof();
  }
}

void BuildOutMsgQueueProof::got_block_root(size_t i, Ref<vm::Cell> root) {
  blocks_[i].block_root = std::move(root);
  if (--pending == 0) {
    build_proof();
  }
}

void BuildOutMsgQueueProof::build_proof() {
  auto result = OutMsgQueueProof::build(dst_shard_, std::move(blocks_), limits_);
  if (result.is_error()) {
    LOG(ERROR) << "Failed to build msg queue proof: " << result.error();
  }
  promise_.set_result(std::move(result));
  stop();
}

}  // namespace validator
}  // namespace ton
