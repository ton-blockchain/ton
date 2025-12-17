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
#include "block/block-auto.h"
#include "block/block-parse.h"
#include "common/delay.h"
#include "interfaces/proof.h"
#include "interfaces/validator-manager.h"
#include "vm/cells/MerkleProof.h"

#include "out-msg-queue-proof.hpp"
#include "output-queue-merger.h"
#include "shard.hpp"

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
    dfs_cs(*b.second.proc_info);
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
      auto state_root = vm::MerkleProof::virtualize(queue_proofs[i]);
      if (state_root->get_hash().as_slice() != state_root_hash.as_slice()) {
        return td::Status::Error("state root hash mismatch");
      }
      res.emplace_back(true, blocks[i], state_root, block_state_proof, false, f.msg_counts_[i]);

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
  if (blocks.empty()) {
    promise.set_value({});
    return;
  }
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

  FLOG(DEBUG) {
    sb << "Importing neighbor msg queues for shard " << dst_shard.to_str() << ", " << blocks.size() << " blocks:";
    for (const BlockIdExt& block : blocks) {
      sb << " " << block.id.to_str();
    }
  };

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

      LOG(DEBUG) << "search for out msg queue proof " << prefix.to_str() << " " << block.to_str();
      auto& small_entry = small_cache_[std::make_pair(dst_shard, block)];
      if (!small_entry.result.is_null()) {
        entry->result[block] = small_entry.result;
        entry->from_small_cache++;
        alarm_timestamp().relax(small_entry.timeout = td::Timestamp::in(CACHE_TTL));
      } else {
        small_entry.pending_entries.push_back(entry);
        ++entry->pending;
        new_queries[prefix].push_back(block);
      }
    }
  };
  auto limits = last_masterchain_state_->get_imported_msg_queue_limits(dst_shard.workchain);
  for (auto& p : new_queries) {
    for (size_t i = 0; i < p.second.size(); i += 16) {
      size_t j = std::min(i + 16, p.second.size());
      get_proof_import(entry, std::vector<BlockIdExt>(p.second.begin() + i, p.second.begin() + j),
                       limits * (td::uint32)(j - i));
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
      manager_, &ValidatorManager::wait_block_state_short, block, 0, entry->timeout, false,
      [=, SelfId = actor_id(this), manager = manager_, timeout = entry->timeout,
       retry_after = td::Timestamp::in(0.1)](td::Result<Ref<ShardState>> R) mutable {
        if (R.is_error()) {
          LOG(DEBUG) << "Failed to get block state for " << block.to_str() << ": " << R.move_as_error();
          delay_action([=]() { td::actor::send_closure(SelfId, &OutMsgQueueImporter::get_proof_local, entry, block); },
                       retry_after);
          return;
        }
        auto state = R.move_as_ok();
        if (block.seqno() == 0) {
          std::vector<td::Ref<OutMsgQueueProof>> proof = {
              td::Ref<OutMsgQueueProof>(true, block, state->root_cell(), td::Ref<vm::Cell>{}, true)};
          td::actor::send_closure(SelfId, &OutMsgQueueImporter::got_proof, entry, std::move(proof), ProofSource::Local);
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
                  td::Ref<OutMsgQueueProof>(true, block, state->root_cell(), std::move(block_state_proof), true)};
              td::actor::send_closure(SelfId, &OutMsgQueueImporter::got_proof, entry, std::move(proof),
                                      ProofSource::Local);
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
      [=, SelfId = actor_id(this), retry_after = td::Timestamp::in(0.1),
       dst_shard = entry->dst_shard](td::Result<std::vector<td::Ref<OutMsgQueueProof>>> R) {
        if (R.is_error()) {
          FLOG(DEBUG) {
            sb << "Failed to get out msg queue for " << dst_shard.to_str() << " from";
            for (const BlockIdExt& block : blocks) {
              sb << " " << block.id.to_str();
            }
            sb << ": " << R.move_as_error();
          };
          delay_action(
              [=]() {
                td::actor::send_closure(SelfId, &OutMsgQueueImporter::get_proof_import, entry, std::move(blocks),
                                        limits);
              },
              retry_after);
          return;
        }
        td::actor::send_closure(SelfId, &OutMsgQueueImporter::got_proof, entry, R.move_as_ok(), ProofSource::Query);
      });
}

void OutMsgQueueImporter::got_proof(std::shared_ptr<CacheEntry> entry, std::vector<td::Ref<OutMsgQueueProof>> proofs,
                                    ProofSource proof_source) {
  if (!check_timeout(entry)) {
    return;
  }
  // TODO: maybe save proof to small cache? It would allow other queries to reuse this result
  for (auto& p : proofs) {
    auto block_id = p->block_id_;
    if (entry->result.emplace(block_id, std::move(p)).second) {
      CHECK(entry->pending > 0);
      switch (proof_source) {
        case ProofSource::SmallCache:
          entry->from_small_cache++;
          break;
        case ProofSource::Broadcast:
          entry->from_broadcast++;
          break;
        case ProofSource::Query:
          entry->from_query++;
          break;
        case ProofSource::Local:
          entry->from_local++;
          break;
      }
      if (--entry->pending == 0) {
        finish_query(entry);
      }
    }
  }
}

void OutMsgQueueImporter::finish_query(std::shared_ptr<CacheEntry> entry) {
  FLOG(INFO) {
    sb << "Done importing neighbor msg queues for shard " << entry->dst_shard.to_str() << " from";
    for (const BlockIdExt& block : entry->blocks) {
      sb << " " << block.id.to_str();
    }
    sb << " in " << entry->timer.elapsed() << "s";
    sb << " sources{";
    if (entry->from_broadcast) {
      sb << " broadcast=" << entry->from_broadcast;
    }
    if (entry->from_small_cache) {
      sb << " small_cache=" << entry->from_small_cache;
    }
    if (entry->from_local) {
      sb << " local=" << entry->from_local;
    }
    if (entry->from_query) {
      sb << " query=" << entry->from_query;
    }
    sb << "}";

    if (!small_cache_.empty()) {
      sb << " small_cache_size=" << small_cache_.size();
    }
    if (!cache_.empty()) {
      sb << " cache_size=" << cache_.size();
    }
  };

  entry->done = true;
  CHECK(entry->blocks.size() == entry->result.size());
  alarm_timestamp().relax(entry->timeout = td::Timestamp::in(CACHE_TTL));
  for (auto& p : entry->promises) {
    p.first.set_result(entry->result);
  }
  entry->promises.clear();
}

bool OutMsgQueueImporter::check_timeout(std::shared_ptr<CacheEntry> entry) {
  if (entry->timeout.is_in_past()) {
    FLOG(DEBUG) {
      sb << "Aborting importing neighbor msg queues for shard " << entry->dst_shard.to_str() << " from";
      for (const BlockIdExt& block : entry->blocks) {
        sb << " " << block.id.to_str();
      }
      sb << ": timeout";
    };
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
  for (auto it = cache_.begin(); it != cache_.end();) {
    auto& promises = it->second->promises;
    if (it->second->timeout.is_in_past()) {
      if (!it->second->done) {
        FLOG(DEBUG) {
          sb << "Aborting importing neighbor msg queues for shard " << it->second->dst_shard.to_str() << " from";
          for (const BlockIdExt& block : it->second->blocks) {
            sb << " " << block.id.to_str();
          }
          sb << ": timeout";
        };
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

  for (auto it = small_cache_.begin(); it != small_cache_.end();) {
    td::remove_if(it->second.pending_entries,
                  [](const std::shared_ptr<CacheEntry>& entry) { return entry->done || entry->promises.empty(); });
    if (it->second.timeout.is_in_past()) {
      if (it->second.pending_entries.empty()) {
        it = small_cache_.erase(it);
      } else {
        ++it;
      }
    } else {
      alarm_timestamp().relax(it->second.timeout);
      ++it;
    }
  }
}

void OutMsgQueueImporter::add_out_msg_queue_proof(ShardIdFull dst_shard, td::Ref<OutMsgQueueProof> proof) {
  LOG(INFO) << "add out msg queue proof " << dst_shard.to_str() << " " << proof->block_id_.to_str();
  auto& small_entry = small_cache_[std::make_pair(dst_shard, proof->block_id_)];
  if (!small_entry.result.is_null()) {
    return;
  }
  alarm_timestamp().relax(small_entry.timeout = td::Timestamp::in(CACHE_TTL));
  small_entry.result = proof;
  CHECK(proof.not_null());
  auto pending_entries = std::move(small_entry.pending_entries);
  for (auto& entry : pending_entries) {
    got_proof(entry, {proof}, ProofSource::Broadcast);
  }
}

void BuildOutMsgQueueProof::abort_query(td::Status reason) {
  if (promise_) {
    FLOG(DEBUG) {
      sb << "failed to build msg queue proof to " << dst_shard_.to_str() << " from";
      for (const auto& block : blocks_) {
        sb << " " << block.id.id.to_str();
      }
      sb << ": " << reason;
    };
    promise_.set_error(
        reason.move_as_error_prefix(PSTRING() << "failed to build msg queue proof to " << dst_shard_.to_str() << ": "));
  }
  stop();
}

void BuildOutMsgQueueProof::start_up() {
  if (blocks_.size() > 16) {
    abort_query(td::Status::Error("too many blocks"));
    return;
  }
  td::actor::send_closure(manager_, &ValidatorManagerInterface::get_top_masterchain_state,
                          [SelfId = actor_id(this)](td::Result<Ref<MasterchainState>> R) {
                            if (R.is_error()) {
                              td::actor::send_closure(SelfId, &BuildOutMsgQueueProof::abort_query,
                                                      R.move_as_error_prefix("failed to get masterchain state: "));
                            } else {
                              td::actor::send_closure(SelfId, &BuildOutMsgQueueProof::got_masterchain_state,
                                                      R.move_as_ok());
                            }
                          });
}

void BuildOutMsgQueueProof::got_masterchain_state(Ref<MasterchainState> mc_state) {
  auto config_limits = mc_state->get_imported_msg_queue_limits(dst_shard_.is_masterchain());
  if ((td::uint64)config_limits.max_msgs * blocks_.size() < limits_.max_msgs) {
    abort_query(td::Status::Error("too big max_msgs"));
    return;
  }
  if ((td::uint64)config_limits.max_bytes * blocks_.size() < limits_.max_bytes) {
    abort_query(td::Status::Error("too big max_bytes"));
    return;
  }

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
