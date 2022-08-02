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

namespace ton {

namespace validator {

td::Result<td::Ref<OutMsgQueueProof>> OutMsgQueueProof::fetch(BlockIdExt block_id, ShardIdFull dst_shard,
                                                              const ton_api::tonNode_outMsgQueueProof& f) {
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
  auto state_root = vm::CellSlice(vm::NoVm(), queue_proof).prefetch_ref(0);
  TRY_RESULT_PREFIX(state, ShardStateQ::fetch(block_id, {}, state_root), "invalid proof: ");
  TRY_RESULT_PREFIX(queue, state->message_queue(), "invalid proof: ");
  auto queue_root = queue->root_cell();
  if (queue_root->get_level() != 0) {
    return td::Status::Error("invalid proof: msg queue has prunned branches");
  }

  return Ref<OutMsgQueueProof>(true, std::move(virtual_root), std::move(block_state_proof));
}

td::Result<tl_object_ptr<ton_api::tonNode_outMsgQueueProof>> OutMsgQueueProof::serialize(BlockIdExt block_id,
                                                                                         ShardIdFull dst_shard,
                                                                                         Ref<vm::Cell> state_root,
                                                                                         Ref<vm::Cell> block_root) {
  vm::MerkleProofBuilder mpb{std::move(state_root)};
  TRY_RESULT(state, ShardStateQ::fetch(block_id, {}, mpb.root()));
  TRY_RESULT(outq_descr, state->message_queue());

  // TODO: add only required part of msg queue
  td::HashSet<vm::Cell::Hash> visited;
  std::function<void(Ref<vm::Cell>)> dfs = [&](Ref<vm::Cell> cell) {
    if (!visited.insert(cell->get_hash()).second) {
      return;
    }
    vm::CellSlice cs(vm::NoVm(), cell);
    for (unsigned i = 0; i < cs.size_refs(); i++) {
      dfs(cs.prefetch_ref(i));
    }
  };
  dfs(outq_descr->root_cell());

  TRY_RESULT(queue_proof, vm::std_boc_serialize(mpb.extract_proof()));

  td::BufferSlice block_state_proof;
  if (block_id.seqno() != 0) {
    TRY_RESULT(proof, create_block_state_proof(std::move(block_root)));
    TRY_RESULT_ASSIGN(block_state_proof, vm::std_boc_serialize(std::move(proof), 31));
  }

  return create_tl_object<ton_api::tonNode_outMsgQueueProof>(std::move(queue_proof), std::move(block_state_proof));
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
                                                      R.move_as_error_prefix("failed to get shard state"));
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
                                                        R.move_as_error_prefix("failed to get block data"));
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
          LOG(DEBUG) << "failed to get msg queue for " << block_id.to_str() << " from net: " << R.move_as_error();
          delay_action([SelfId]() mutable { td::actor::send_closure(SelfId, &WaitOutMsgQueueProof::run_net); },
                       td::Timestamp::in(0.1));
        } else {
          td::actor::send_closure(SelfId, &WaitOutMsgQueueProof::finish_query, R.move_as_ok());
        }
      });

  td::actor::send_closure(manager_, &ValidatorManager::send_get_out_msg_queue_proof_request, block_id_, dst_shard_,
                          priority_, std::move(P));
}

void BuildOutMsgQueueProof::abort_query(td::Status reason) {
  if (promise_) {
    LOG(WARNING) << "failed to build msg queue proof for " << block_id_.to_str() << ": " << reason;
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
                                                      R.move_as_error_prefix("failed to get shard state"));
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
                                                        R.move_as_error_prefix("failed to get block data"));
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
  promise_.set_result(
      OutMsgQueueProof::serialize(block_id_, dst_shard_, std::move(state_root_), std::move(block_root_)));
  stop();
}

}  // namespace validator
}  // namespace ton
