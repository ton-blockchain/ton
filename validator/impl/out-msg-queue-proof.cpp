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
