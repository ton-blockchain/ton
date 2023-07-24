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

static td::Result<td::int32> process_queue(BlockIdExt block_id, ShardIdFull dst_shard,
                                           block::ImportedMsgQueueLimits limits,
                                           const block::gen::OutMsgQueueInfo::Record& qinfo) {
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
  TRY_STATUS_PREFIX(check_no_prunned(*qinfo.proc_info), "invalid proc_info proof: ")
  TRY_STATUS_PREFIX(check_no_prunned(*qinfo.ihr_pending), "invalid ihr_pending proof: ")
  dfs_cs(*qinfo.proc_info);
  dfs_cs(*qinfo.ihr_pending);

  block::OutputQueueMerger queue_merger{
      dst_shard, {block::OutputQueueMerger::Neighbor{block_id, qinfo.out_queue->prefetch_ref()}}};
  td::int32 msg_count = 0;
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
    ++msg_count;

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
    if (estimated_proof_size >= limits.max_bytes || msg_count >= (long long)limits.max_msgs) {
      limit_reached = true;
    }
  }
  return limit_reached ? msg_count : -1;
}

td::Result<tl_object_ptr<ton_api::tonNode_outMsgQueueProof>> OutMsgQueueProof::build(
    BlockIdExt block_id, ShardIdFull dst_shard,
    block::ImportedMsgQueueLimits limits, Ref<vm::Cell> state_root, Ref<vm::Cell> block_root) {
  if (!dst_shard.is_valid_ext()) {
    return td::Status::Error("invalid shard");
  }

  vm::MerkleProofBuilder mpb{std::move(state_root)};
  TRY_RESULT(state, ShardStateQ::fetch(block_id, {}, mpb.root()));
  TRY_RESULT(outq_descr, state->message_queue());
  block::gen::OutMsgQueueInfo::Record qinfo;
  if (!tlb::unpack_cell(outq_descr->root_cell(), qinfo)) {
    return td::Status::Error("invalid message queue");
  }
  TRY_RESULT(cnt, process_queue(block_id, dst_shard, limits, qinfo));

  TRY_RESULT(queue_proof, mpb.extract_proof_boc());
  td::BufferSlice block_state_proof;
  if (block_id.seqno() != 0) {
    TRY_RESULT(proof, create_block_state_proof(std::move(block_root)));
    TRY_RESULT_ASSIGN(block_state_proof, vm::std_boc_serialize(std::move(proof), 31));
  }
  return create_tl_object<ton_api::tonNode_outMsgQueueProof>(std::move(queue_proof), std::move(block_state_proof),
                                                             cnt);
}

td::Result<td::Ref<OutMsgQueueProof>> OutMsgQueueProof::fetch(BlockIdExt block_id, ShardIdFull dst_shard,
                                                              block::ImportedMsgQueueLimits limits,
                                                              const ton_api::tonNode_outMsgQueueProof& f) {
  try {
    Ref<vm::Cell> block_state_proof;
    td::Bits256 state_root_hash;
    if (block_id.seqno() == 0) {
      if (!f.block_state_proof_.empty()) {
        return td::Status::Error("expected empty block state proof");
      }
      state_root_hash = block_id.root_hash;
    } else {
      TRY_RESULT_ASSIGN(block_state_proof, vm::std_boc_deserialize(f.block_state_proof_.as_slice()));
      TRY_RESULT_ASSIGN(state_root_hash, unpack_block_state_proof(block_id, block_state_proof));
    }

    TRY_RESULT(queue_proof, vm::std_boc_deserialize(f.queue_proof_.as_slice()));
    auto virtual_root = vm::MerkleProof::virtualize(queue_proof, 1);
    if (virtual_root.is_null()) {
      return td::Status::Error("invalid queue proof");
    }
    if (virtual_root->get_hash().as_slice() != state_root_hash.as_slice()) {
      return td::Status::Error("state root hash mismatch");
    }

    // Validate proof
    TRY_RESULT_PREFIX(state, ShardStateQ::fetch(block_id, {}, virtual_root), "invalid proof: ");
    TRY_RESULT_PREFIX(outq_descr, state->message_queue(), "invalid proof: ");

    block::gen::OutMsgQueueInfo::Record qinfo;
    if (!tlb::unpack_cell(outq_descr->root_cell(), qinfo)) {
      return td::Status::Error("invalid proof: invalid message queue");
    }
    TRY_STATUS_PREFIX(check_no_prunned(qinfo.proc_info->prefetch_ref(0)), "invalid proc_info: ")
    TRY_STATUS_PREFIX(check_no_prunned(qinfo.ihr_pending->prefetch_ref(0)), "invalid ihr_pending: ")
    TRY_RESULT(cnt, process_queue(block_id, dst_shard, limits, qinfo));
    if (cnt != f.msg_count_) {
      return td::Status::Error(PSTRING() << "invalid msg_count: expected=" << f.msg_count_ << ", found=" << cnt);
    }
    return Ref<OutMsgQueueProof>(true, std::move(virtual_root), std::move(block_state_proof), cnt);
  } catch (vm::VmVirtError& err) {
    return td::Status::Error(PSTRING() << "invalid proof: " << err.get_msg());
  }
}

void WaitOutMsgQueueProof::alarm() {
  abort_query(td::Status::Error(ErrorCode::timeout, "timeout"));
}

void WaitOutMsgQueueProof::abort_query(td::Status reason) {
  if (promise_) {
    if (priority_ > 0 || (reason.code() != ErrorCode::timeout && reason.code() != ErrorCode::notready)) {
      LOG(WARNING) << "aborting wait msg queue query for " << block_id_.to_str() << " priority=" << priority_ << ": "
                   << reason;
    } else {
      LOG(DEBUG) << "aborting wait msg queue query for " << block_id_.to_str() << " priority=" << priority_ << ": "
                 << reason;
    }
    promise_.set_error(
        reason.move_as_error_prefix(PSTRING() << "failed to get msg queue for " << block_id_.to_str() << ": "));
  }
  stop();
}

void WaitOutMsgQueueProof::finish_query(Ref<OutMsgQueueProof> result) {
  promise_.set_result(std::move(result));
  stop();
}

void WaitOutMsgQueueProof::start_up() {
  alarm_timestamp() = timeout_;
  if (local_) {
    run_local();
  } else {
    run_net();
  }
}

void WaitOutMsgQueueProof::run_local() {
  ++pending;
  td::actor::send_closure(manager_, &ValidatorManager::wait_block_state_short, block_id_, priority_, timeout_,
                          [SelfId = actor_id(this)](td::Result<Ref<ShardState>> R) {
                            if (R.is_error()) {
                              td::actor::send_closure(SelfId, &WaitOutMsgQueueProof::abort_query,
                                                      R.move_as_error_prefix("failed to get shard state: "));
                            } else {
                              td::actor::send_closure(SelfId, &WaitOutMsgQueueProof::got_state_root,
                                                      R.move_as_ok()->root_cell());
                            }
                          });
  if (block_id_.seqno() != 0) {
    ++pending;
    td::actor::send_closure(manager_, &ValidatorManager::wait_block_data_short, block_id_, priority_, timeout_,
                            [SelfId = actor_id(this)](td::Result<Ref<BlockData>> R) {
                              if (R.is_error()) {
                                td::actor::send_closure(SelfId, &WaitOutMsgQueueProof::abort_query,
                                                        R.move_as_error_prefix("failed to get block data: "));
                              } else {
                                td::actor::send_closure(SelfId, &WaitOutMsgQueueProof::got_block_root,
                                                        R.move_as_ok()->root_cell());
                              }
                            });
  }
}

void WaitOutMsgQueueProof::got_state_root(Ref<vm::Cell> root) {
  state_root_ = std::move(root);
  if (--pending == 0) {
    run_local_cont();
  }
}

void WaitOutMsgQueueProof::got_block_root(Ref<vm::Cell> root) {
  block_root_ = std::move(root);
  if (--pending == 0) {
    run_local_cont();
  }
}

void WaitOutMsgQueueProof::run_local_cont() {
  Ref<vm::Cell> block_state_proof;
  if (block_id_.seqno() != 0) {
    auto R = create_block_state_proof(std::move(block_root_));
    if (R.is_error()) {
      abort_query(R.move_as_error_prefix("failed to create block state proof"));
      return;
    }
    block_state_proof = R.move_as_ok();
  }
  finish_query(td::Ref<OutMsgQueueProof>(true, std::move(state_root_), std::move(block_state_proof)));
}

void WaitOutMsgQueueProof::run_net() {
  auto P =
      td::PromiseCreator::lambda([SelfId = actor_id(this), block_id = block_id_](td::Result<Ref<OutMsgQueueProof>> R) {
        if (R.is_error()) {
          if (R.error().code() == ErrorCode::notready) {
            LOG(DEBUG) << "failed to get msg queue for " << block_id.to_str() << " from net: " << R.move_as_error();
          } else {
            LOG(WARNING) << "failed to get msg queue for " << block_id.to_str() << " from net: " << R.move_as_error();
          }
          delay_action([SelfId]() mutable { td::actor::send_closure(SelfId, &WaitOutMsgQueueProof::run_net); },
                       td::Timestamp::in(0.1));
        } else {
          td::actor::send_closure(SelfId, &WaitOutMsgQueueProof::finish_query, R.move_as_ok());
        }
      });

  td::actor::send_closure(manager_, &ValidatorManager::send_get_out_msg_queue_proof_request, block_id_, dst_shard_,
                          limits_, priority_, std::move(P));
}

void BuildOutMsgQueueProof::abort_query(td::Status reason) {
  if (promise_) {
    LOG(DEBUG) << "failed to build msg queue proof for " << block_id_.to_str() << ": " << reason;
    promise_.set_error(
        reason.move_as_error_prefix(PSTRING() << "failed to build msg queue proof for " << block_id_.to_str() << ": "));
  }
  stop();
}

void BuildOutMsgQueueProof::start_up() {
  ++pending;
  td::actor::send_closure(manager_, &ValidatorManagerInterface::get_shard_state_from_db_short, block_id_,
                          [SelfId = actor_id(this)](td::Result<Ref<ShardState>> R) {
                            if (R.is_error()) {
                              td::actor::send_closure(SelfId, &BuildOutMsgQueueProof::abort_query,
                                                      R.move_as_error_prefix("failed to get shard state: "));
                            } else {
                              td::actor::send_closure(SelfId, &BuildOutMsgQueueProof::got_state_root,
                                                      R.move_as_ok()->root_cell());
                            }
                          });
  if (block_id_.seqno() != 0) {
    ++pending;
    td::actor::send_closure(manager_, &ValidatorManagerInterface::get_block_data_from_db_short, block_id_,
                            [SelfId = actor_id(this)](td::Result<Ref<BlockData>> R) {
                              if (R.is_error()) {
                                td::actor::send_closure(SelfId, &BuildOutMsgQueueProof::abort_query,
                                                        R.move_as_error_prefix("failed to get block data: "));
                              } else {
                                td::actor::send_closure(SelfId, &BuildOutMsgQueueProof::got_block_root,
                                                        R.move_as_ok()->root_cell());
                              }
                            });
  }
}

void BuildOutMsgQueueProof::got_state_root(Ref<vm::Cell> root) {
  state_root_ = std::move(root);
  if (--pending == 0) {
    build_proof();
  }
}

void BuildOutMsgQueueProof::got_block_root(Ref<vm::Cell> root) {
  block_root_ = std::move(root);
  if (--pending == 0) {
    build_proof();
  }
}

void BuildOutMsgQueueProof::build_proof() {
  auto result = OutMsgQueueProof::build(block_id_, dst_shard_, limits_, std::move(state_root_), std::move(block_root_));
  if (result.is_error()) {
    LOG(ERROR) << "Failed to build msg queue proof: " << result.error();
  }
  promise_.set_result(std::move(result));
  stop();
}

}  // namespace validator
}  // namespace ton
