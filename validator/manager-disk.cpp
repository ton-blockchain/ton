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

    Copyright 2017-2020 Telegram Systems LLP
*/
#include "manager-disk.hpp"
#include "validator-group.hpp"
#include "adnl/utils.hpp"
#include "downloaders/wait-block-state.hpp"
#include "downloaders/wait-block-state-merge.hpp"
#include "downloaders/wait-block-data-disk.hpp"
#include "validator-group.hpp"
#include "fabric.h"
#include "manager.h"
#include "ton/ton-io.hpp"
#include "td/utils/overloaded.h"

namespace ton {

namespace validator {

void ValidatorManagerImpl::validate_block_is_next_proof(BlockIdExt prev_block_id, BlockIdExt next_block_id,
                                                        td::BufferSlice proof, td::Promise<td::Unit> promise) {
  UNREACHABLE();
}

void ValidatorManagerImpl::validate_block_proof(BlockIdExt block_id, td::BufferSlice proof,
                                                td::Promise<td::Unit> promise) {
  auto pp = create_proof(block_id, std::move(proof));
  if (pp.is_error()) {
    promise.set_error(pp.move_as_error());
    return;
  }

  auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<BlockHandle> R) mutable {
    if (R.is_error()) {
      promise.set_error(R.move_as_error());
    } else {
      promise.set_value(td::Unit());
    }
  });
  run_check_proof_query(block_id, pp.move_as_ok(), actor_id(this), td::Timestamp::in(2.0), std::move(P));
}

void ValidatorManagerImpl::validate_block_proof_link(BlockIdExt block_id, td::BufferSlice proof,
                                                     td::Promise<td::Unit> promise) {
  UNREACHABLE();
}

void ValidatorManagerImpl::validate_block(ReceivedBlock block, td::Promise<BlockHandle> promise) {
  UNREACHABLE();
}

void ValidatorManagerImpl::prevalidate_block(BlockBroadcast broadcast, td::Promise<td::Unit> promise) {
  UNREACHABLE();
}

void ValidatorManagerImpl::sync_complete(td::Promise<td::Unit> promise) {
  started_ = true;

  //ShardIdFull shard_id{masterchainId, shardIdAll};
  auto shard_id = shard_to_generate_;

  auto block_id = block_to_generate_;

  std::vector<BlockIdExt> prev;
  if (!block_id.is_valid()) {
    if (shard_id.is_masterchain()) {
      prev = {last_masterchain_block_id_};
    } else {
      auto S = last_masterchain_state_->get_shard_from_config(shard_id);
      if (S.not_null()) {
        prev = {S->top_block_id()};
      } else {
        S = last_masterchain_state_->get_shard_from_config(shard_parent(shard_id));
        if (S.not_null()) {
          CHECK(S->before_split());
          prev = {S->top_block_id()};
        } else {
          S = last_masterchain_state_->get_shard_from_config(shard_child(shard_id, true));
          CHECK(S.not_null());
          CHECK(S->before_merge());
          auto S2 = last_masterchain_state_->get_shard_from_config(shard_child(shard_id, false));
          CHECK(S2.not_null());
          CHECK(S2->before_merge());
          prev = {S->top_block_id(), S2->top_block_id()};
        }
      }
    }
  } else {
    CHECK(block_id.shard_full() == shard_id);
    prev = {block_id};
  }

  //LOG(DEBUG) << "before get_validator_set";
  auto val_set = last_masterchain_state_->get_validator_set(shard_id);
  //LOG(DEBUG) << "after get_validator_set: addr=" << (const void*)val_set.get();

  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), last = last_masterchain_block_id_, val_set, prev](td::Result<BlockCandidate> R) {
        if (R.is_ok()) {
          auto v = R.move_as_ok();
          LOG(ERROR) << "created block " << v.id;
          td::actor::send_closure(SelfId, &ValidatorManagerImpl::validate_fake, std::move(v), std::move(prev), last,
                                  val_set);
        } else {
          LOG(ERROR) << "failed to create block: " << R.move_as_error();
          std::exit(2);
        }
      });

  LOG(ERROR) << "running collate query";
  if (local_id_.is_zero()) {
    //td::as<td::uint32>(created_by_.data() + 32 - 4) = ((unsigned)std::time(nullptr) >> 8);
  }
  Ed25519_PublicKey created_by{td::Bits256::zero()};
  td::as<td::uint32>(created_by.as_bits256().data() + 32 - 4) = ((unsigned)std::time(nullptr) >> 8);
  run_collate_query(shard_id, 0, last_masterchain_block_id_, prev, created_by, val_set, actor_id(this),
                    td::Timestamp::in(10.0), std::move(P));
}

void ValidatorManagerImpl::validate_fake(BlockCandidate candidate, std::vector<BlockIdExt> prev, BlockIdExt last,
                                         td::Ref<ValidatorSet> val_set) {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), c = candidate.clone(), prev, last,
                                       val_set](td::Result<ValidateCandidateResult> R) mutable {
    if (R.is_ok()) {
      auto v = R.move_as_ok();
      v.visit(td::overloaded(
          [&](UnixTime ts) {
            td::actor::send_closure(SelfId, &ValidatorManagerImpl::write_fake, std::move(c), prev, last, val_set);
          },
          [&](CandidateReject reject) {
            LOG(ERROR) << "failed to create block: " << reject.reason;
            std::exit(2);
          }));
    } else {
      LOG(ERROR) << "failed to create block: " << R.move_as_error();
      std::exit(2);
    }
  });
  auto shard = candidate.id.shard_full();
  run_validate_query(shard, 0, last, prev, std::move(candidate), std::move(val_set), actor_id(this),
                     td::Timestamp::in(10.0), std::move(P), true /* fake */);
}

void ValidatorManagerImpl::write_fake(BlockCandidate candidate, std::vector<BlockIdExt> prev, BlockIdExt last,
                                      td::Ref<ValidatorSet> val_set) {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), id = candidate.id](td::Result<td::Unit> R) {
    if (R.is_ok()) {
      td::actor::send_closure(SelfId, &ValidatorManagerImpl::complete_fake, id);
    } else {
      LOG(ERROR) << "failed to create block: " << R.move_as_error();
      std::exit(2);
    }
  });
  auto data = create_block(candidate.id, std::move(candidate.data)).move_as_ok();

  run_fake_accept_block_query(candidate.id, data, prev, val_set, actor_id(this), std::move(P));
}

void ValidatorManagerImpl::complete_fake(BlockIdExt block_id) {
  LOG(ERROR) << "success, block " << block_id << " = " << block_id.to_str() << " saved to disk";
  std::exit(0);
}

void ValidatorManagerImpl::get_next_block(BlockIdExt block_id, td::Promise<BlockHandle> promise) {
  UNREACHABLE();
}

void ValidatorManagerImpl::get_block_data(BlockHandle handle, td::Promise<td::BufferSlice> promise) {
  auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<td::Ref<BlockData>> R) mutable {
    if (R.is_error()) {
      promise.set_error(R.move_as_error());
    } else {
      auto B = R.move_as_ok();
      promise.set_value(B->data());
    }
  });

  get_block_data_from_db(handle, std::move(P));
}

void ValidatorManagerImpl::check_zero_state_exists(BlockIdExt block_id, td::Promise<bool> promise) {
  td::actor::send_closure(db_, &Db::check_zero_state_file_exists, block_id, std::move(promise));
}
void ValidatorManagerImpl::get_zero_state(BlockIdExt block_id, td::Promise<td::BufferSlice> promise) {
  td::actor::send_closure(db_, &Db::get_zero_state_file, block_id, std::move(promise));
}

void ValidatorManagerImpl::check_persistent_state_exists(BlockIdExt block_id, BlockIdExt masterchain_block_id,
                                                         td::Promise<bool> promise) {
  td::actor::send_closure(db_, &Db::check_persistent_state_file_exists, block_id, masterchain_block_id,
                          std::move(promise));
}
void ValidatorManagerImpl::get_persistent_state(BlockIdExt block_id, BlockIdExt masterchain_block_id,
                                                td::Promise<td::BufferSlice> promise) {
  td::actor::send_closure(db_, &Db::get_persistent_state_file, block_id, masterchain_block_id, std::move(promise));
}

void ValidatorManagerImpl::get_block_proof(BlockHandle handle, td::Promise<td::BufferSlice> promise) {
  auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<td::Ref<Proof>> R) mutable {
    if (R.is_error()) {
      promise.set_error(R.move_as_error());
    } else {
      auto B = R.move_as_ok();
      promise.set_value(B->data());
    }
  });

  td::actor::send_closure(db_, &Db::get_block_proof, handle, std::move(P));
}

void ValidatorManagerImpl::get_key_block_proof(BlockIdExt block_id, td::Promise<td::BufferSlice> promise) {
  auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<td::Ref<Proof>> R) mutable {
    if (R.is_error()) {
      promise.set_error(R.move_as_error());
    } else {
      auto B = R.move_as_ok();
      promise.set_value(B->data());
    }
  });

  td::actor::send_closure(db_, &Db::get_key_block_proof, block_id, std::move(P));
}

void ValidatorManagerImpl::get_key_block_proof_link(BlockIdExt block_id, td::Promise<td::BufferSlice> promise) {
  auto P = td::PromiseCreator::lambda(
      [promise = std::move(promise), block_id, db = db_.get()](td::Result<td::Ref<Proof>> R) mutable {
        if (R.is_error()) {
          auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<td::Ref<Proof>> R) mutable {
            if (R.is_error()) {
              promise.set_error(R.move_as_error());
            } else {
              auto B = R.move_as_ok();
              promise.set_value(B->data());
            }
          });

          td::actor::send_closure(db, &Db::get_key_block_proof, block_id, std::move(P));
        } else {
          auto B = R.move_as_ok()->export_as_proof_link().move_as_ok();
          promise.set_value(B->data());
        }
      });

  td::actor::send_closure(db_, &Db::get_key_block_proof, block_id, std::move(P));
}

void ValidatorManagerImpl::new_external_message(td::BufferSlice data) {
  auto R = create_ext_message(std::move(data));
  if (R.is_ok()) {
    ext_messages_.emplace_back(R.move_as_ok());
  }
}

void ValidatorManagerImpl::new_ihr_message(td::BufferSlice data) {
  auto R = create_ihr_message(std::move(data));
  if (R.is_ok()) {
    ihr_messages_.emplace_back(R.move_as_ok());
  }
}

void ValidatorManagerImpl::new_shard_block(BlockIdExt block_id, CatchainSeqno cc_seqno, td::BufferSlice data) {
  if (!last_masterchain_block_handle_) {
    shard_blocks_raw_.push_back(std::move(data));
    return;
  }
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<ShardTopBlockDescription>> R) {
    if (R.is_error()) {
      LOG(WARNING) << "dropping invalid new shard block description: " << R.move_as_error();
      td::actor::send_closure(SelfId, &ValidatorManagerImpl::dec_pending_new_blocks);
    } else {
      // LOG(DEBUG) << "run_validate_shard_block_description() completed successfully";
      td::actor::send_closure(SelfId, &ValidatorManagerImpl::add_shard_block_description, R.move_as_ok());
    }
  });
  ++pending_new_shard_block_descr_;
  // LOG(DEBUG) << "new run_validate_shard_block_description()";
  run_validate_shard_block_description(std::move(data), last_masterchain_block_handle_, last_masterchain_state_,
                                       actor_id(this), td::Timestamp::in(2.0), std::move(P), true);
}

void ValidatorManagerImpl::add_shard_block_description(td::Ref<ShardTopBlockDescription> desc) {
  if (desc->may_be_valid(last_masterchain_block_handle_, last_masterchain_state_)) {
    shard_blocks_.insert(std::move(desc));
  }
  dec_pending_new_blocks();
}

void ValidatorManagerImpl::dec_pending_new_blocks() {
  if (!--pending_new_shard_block_descr_ && !waiting_new_shard_block_descr_.empty()) {
    std::vector<td::Ref<ShardTopBlockDescription>> res{shard_blocks_.begin(), shard_blocks_.end()};
    auto promises = std::move(waiting_new_shard_block_descr_);
    waiting_new_shard_block_descr_.clear();
    for (auto &promise : promises) {
      promise.set_result(res);
    }
  }
}

void ValidatorManagerImpl::wait_block_state(BlockHandle handle, td::uint32 priority, td::Timestamp timeout,
                                            td::Promise<td::Ref<ShardState>> promise) {
  auto it = wait_state_.find(handle->id());
  if (it == wait_state_.end()) {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), handle](td::Result<td::Ref<ShardState>> R) {
      td::actor::send_closure(SelfId, &ValidatorManagerImpl::finished_wait_state, handle->id(), std::move(R));
    });
    auto id = td::actor::create_actor<WaitBlockState>("waitstate", handle, 0, actor_id(this), td::Timestamp::in(10.0),
                                                      std::move(P))
                  .release();
    wait_state_[handle->id()].actor_ = id;
    it = wait_state_.find(handle->id());
  }

  it->second.waiting_.emplace_back(
      std::pair<td::Timestamp, td::Promise<td::Ref<ShardState>>>(timeout, std::move(promise)));
  td::actor::send_closure(it->second.actor_, &WaitBlockState::update_timeout, timeout, 0);
}

void ValidatorManagerImpl::wait_block_state_short(BlockIdExt block_id, td::uint32 priority, td::Timestamp timeout,
                                                  td::Promise<td::Ref<ShardState>> promise) {
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), timeout, promise = std::move(promise)](td::Result<BlockHandle> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
          return;
        }
        td::actor::send_closure(SelfId, &ValidatorManagerImpl::wait_block_state, R.move_as_ok(), 0, timeout,
                                std::move(promise));
      });
  get_block_handle(block_id, true, std::move(P));
}

void ValidatorManagerImpl::wait_block_data(BlockHandle handle, td::uint32 priority, td::Timestamp timeout,
                                           td::Promise<td::Ref<BlockData>> promise) {
  auto it = wait_block_data_.find(handle->id());
  if (it == wait_block_data_.end()) {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), handle](td::Result<td::Ref<BlockData>> R) {
      td::actor::send_closure(SelfId, &ValidatorManagerImpl::finished_wait_data, handle->id(), std::move(R));
    });
    auto id = td::actor::create_actor<WaitBlockDataDisk>("waitdata", handle, actor_id(this), td::Timestamp::in(10.0),
                                                         std::move(P))
                  .release();
    wait_block_data_[handle->id()].actor_ = id;
    it = wait_block_data_.find(handle->id());
  }

  it->second.waiting_.emplace_back(
      std::pair<td::Timestamp, td::Promise<td::Ref<BlockData>>>(timeout, std::move(promise)));
  td::actor::send_closure(it->second.actor_, &WaitBlockDataDisk::update_timeout, timeout);
}

void ValidatorManagerImpl::wait_block_data_short(BlockIdExt block_id, td::uint32 priority, td::Timestamp timeout,
                                                 td::Promise<td::Ref<BlockData>> promise) {
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), timeout, promise = std::move(promise)](td::Result<BlockHandle> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
          return;
        }
        td::actor::send_closure(SelfId, &ValidatorManagerImpl::wait_block_data, R.move_as_ok(), 0, timeout,
                                std::move(promise));
      });
  get_block_handle(block_id, true, std::move(P));
}

void ValidatorManagerImpl::wait_block_state_merge(BlockIdExt left_id, BlockIdExt right_id, td::uint32 priority,
                                                  td::Timestamp timeout, td::Promise<td::Ref<ShardState>> promise) {
  td::actor::create_actor<WaitBlockStateMerge>("merge", left_id, right_id, 0, actor_id(this), timeout,
                                               std::move(promise))
      .release();
}

void ValidatorManagerImpl::wait_prev_block_state(BlockHandle handle, td::uint32 priority, td::Timestamp timeout,
                                                 td::Promise<td::Ref<ShardState>> promise) {
  CHECK(handle);
  CHECK(!handle->is_zero());
  if (!handle->merge_before()) {
    auto shard = handle->id().shard_full();
    auto prev_shard = handle->one_prev(true).shard_full();
    if (shard == prev_shard) {
      wait_block_state_short(handle->one_prev(true), 0, timeout, std::move(promise));
    } else {
      CHECK(shard_parent(shard) == prev_shard);
      bool left = shard_child(prev_shard, true) == shard;
      auto P =
          td::PromiseCreator::lambda([promise = std::move(promise), left](td::Result<td::Ref<ShardState>> R) mutable {
            if (R.is_error()) {
              promise.set_error(R.move_as_error());
            } else {
              auto s = R.move_as_ok();
              auto r = s->split();
              if (r.is_error()) {
                promise.set_error(r.move_as_error());
              } else {
                auto v = r.move_as_ok();
                promise.set_value(left ? std::move(v.first) : std::move(v.second));
              }
            }
          });
      wait_block_state_short(handle->one_prev(true), 0, timeout, std::move(P));
    }
  } else {
    wait_block_state_merge(handle->one_prev(true), handle->one_prev(false), 0, timeout, std::move(promise));
  }
}

void ValidatorManagerImpl::wait_block_proof(BlockHandle handle, td::Timestamp timeout,
                                            td::Promise<td::Ref<Proof>> promise) {
  td::actor::send_closure(db_, &Db::get_block_proof, handle, std::move(promise));
}

void ValidatorManagerImpl::wait_block_proof_short(BlockIdExt block_id, td::Timestamp timeout,
                                                  td::Promise<td::Ref<Proof>> promise) {
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), timeout, promise = std::move(promise)](td::Result<BlockHandle> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
          return;
        }
        td::actor::send_closure(SelfId, &ValidatorManagerImpl::wait_block_proof, R.move_as_ok(), timeout,
                                std::move(promise));
      });
  get_block_handle(block_id, true, std::move(P));
}

void ValidatorManagerImpl::wait_block_proof_link(BlockHandle handle, td::Timestamp timeout,
                                                 td::Promise<td::Ref<ProofLink>> promise) {
  td::actor::send_closure(db_, &Db::get_block_proof_link, std::move(handle), std::move(promise));
}

void ValidatorManagerImpl::wait_block_proof_link_short(BlockIdExt block_id, td::Timestamp timeout,
                                                       td::Promise<td::Ref<ProofLink>> promise) {
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), timeout, promise = std::move(promise)](td::Result<BlockHandle> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
          return;
        }
        td::actor::send_closure(SelfId, &ValidatorManagerImpl::wait_block_proof_link, R.move_as_ok(), timeout,
                                std::move(promise));
      });
  get_block_handle(block_id, true, std::move(P));
}

void ValidatorManagerImpl::wait_block_signatures(BlockHandle handle, td::Timestamp timeout,
                                                 td::Promise<td::Ref<BlockSignatureSet>> promise) {
  td::actor::send_closure(db_, &Db::get_block_signatures, handle, std::move(promise));
}

void ValidatorManagerImpl::wait_block_signatures_short(BlockIdExt block_id, td::Timestamp timeout,
                                                       td::Promise<td::Ref<BlockSignatureSet>> promise) {
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), timeout, promise = std::move(promise)](td::Result<BlockHandle> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
          return;
        }
        td::actor::send_closure(SelfId, &ValidatorManagerImpl::wait_block_signatures, R.move_as_ok(), timeout,
                                std::move(promise));
      });
  get_block_handle(block_id, true, std::move(P));
}

void ValidatorManagerImpl::wait_block_message_queue(BlockHandle handle, td::uint32 priority, td::Timestamp timeout,
                                                    td::Promise<td::Ref<MessageQueue>> promise) {
  auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<td::Ref<ShardState>> R) mutable {
    if (R.is_error()) {
      promise.set_error(R.move_as_error());
    } else {
      auto state = R.move_as_ok();
      promise.set_result(state->message_queue());
    }
  });

  wait_block_state(handle, 0, timeout, std::move(P));
}

void ValidatorManagerImpl::wait_block_message_queue_short(BlockIdExt block_id, td::uint32 priority,
                                                          td::Timestamp timeout,
                                                          td::Promise<td::Ref<MessageQueue>> promise) {
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), timeout, promise = std::move(promise)](td::Result<BlockHandle> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
          return;
        }
        td::actor::send_closure(SelfId, &ValidatorManagerImpl::wait_block_message_queue, R.move_as_ok(), 0, timeout,
                                std::move(promise));
      });
  get_block_handle(block_id, true, std::move(P));
}

void ValidatorManagerImpl::get_external_messages(ShardIdFull shard,
                                                 td::Promise<std::vector<td::Ref<ExtMessage>>> promise) {
  promise.set_result(ext_messages_);
}

void ValidatorManagerImpl::get_ihr_messages(ShardIdFull shard, td::Promise<std::vector<td::Ref<IhrMessage>>> promise) {
  promise.set_result(ihr_messages_);
}

void ValidatorManagerImpl::get_shard_blocks(BlockIdExt masterchain_block_id,
                                            td::Promise<std::vector<td::Ref<ShardTopBlockDescription>>> promise) {
  if (!last_masterchain_block_handle_) {
    promise.set_result(std::vector<td::Ref<ShardTopBlockDescription>>{});
    return;
  }
  if (!shard_blocks_raw_.empty()) {
    for (auto &raw : shard_blocks_raw_) {
      new_shard_block(BlockIdExt{}, 0, std::move(raw));
    }
    shard_blocks_raw_.clear();
  }
  if (!pending_new_shard_block_descr_) {
    promise.set_result(std::vector<td::Ref<ShardTopBlockDescription>>{shard_blocks_.begin(), shard_blocks_.end()});
  } else {
    // LOG(DEBUG) << "postponed get_shard_blocks query because pending_new_shard_block_descr_=" << pending_new_shard_block_descr_;
    waiting_new_shard_block_descr_.push_back(std::move(promise));
  }
}

void ValidatorManagerImpl::complete_external_messages(std::vector<ExtMessage::Hash> to_delay,
                                                      std::vector<ExtMessage::Hash> to_delete) {
}

void ValidatorManagerImpl::complete_ihr_messages(std::vector<IhrMessage::Hash> to_delay,
                                                 std::vector<IhrMessage::Hash> to_delete) {
}

void ValidatorManagerImpl::get_block_data_from_db(ConstBlockHandle handle, td::Promise<td::Ref<BlockData>> promise) {
  td::actor::send_closure(db_, &Db::get_block_data, handle, std::move(promise));
}

void ValidatorManagerImpl::get_block_data_from_db_short(BlockIdExt block_id, td::Promise<td::Ref<BlockData>> promise) {
  auto P =
      td::PromiseCreator::lambda([db = db_.get(), promise = std::move(promise)](td::Result<BlockHandle> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
        } else {
          auto handle = R.move_as_ok();
          td::actor::send_closure(db, &Db::get_block_data, std::move(handle), std::move(promise));
        }
      });
  get_block_handle(block_id, false, std::move(P));
}

void ValidatorManagerImpl::get_shard_state_from_db(ConstBlockHandle handle, td::Promise<td::Ref<ShardState>> promise) {
  td::actor::send_closure(db_, &Db::get_block_state, handle, std::move(promise));
}

void ValidatorManagerImpl::get_shard_state_from_db_short(BlockIdExt block_id,
                                                         td::Promise<td::Ref<ShardState>> promise) {
  auto P =
      td::PromiseCreator::lambda([db = db_.get(), promise = std::move(promise)](td::Result<BlockHandle> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
        } else {
          auto handle = R.move_as_ok();
          td::actor::send_closure(db, &Db::get_block_state, std::move(handle), std::move(promise));
        }
      });
  get_block_handle(block_id, false, std::move(P));
}

void ValidatorManagerImpl::get_block_candidate_from_db(PublicKey source, BlockIdExt id,
                                                       FileHash collated_data_file_hash,
                                                       td::Promise<BlockCandidate> promise) {
  td::actor::send_closure(db_, &Db::get_block_candidate, source, id, collated_data_file_hash, std::move(promise));
}

void ValidatorManagerImpl::get_block_proof_from_db(ConstBlockHandle handle, td::Promise<td::Ref<Proof>> promise) {
  td::actor::send_closure(db_, &Db::get_block_proof, std::move(handle), std::move(promise));
}

void ValidatorManagerImpl::get_block_proof_from_db_short(BlockIdExt block_id, td::Promise<td::Ref<Proof>> promise) {
  auto P =
      td::PromiseCreator::lambda([db = db_.get(), promise = std::move(promise)](td::Result<BlockHandle> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
        } else {
          auto handle = R.move_as_ok();
          td::actor::send_closure(db, &Db::get_block_proof, std::move(handle), std::move(promise));
        }
      });
  get_block_handle(block_id, false, std::move(P));
}

void ValidatorManagerImpl::get_block_proof_link_from_db(ConstBlockHandle handle,
                                                        td::Promise<td::Ref<ProofLink>> promise) {
  td::actor::send_closure(db_, &Db::get_block_proof_link, std::move(handle), std::move(promise));
}

void ValidatorManagerImpl::get_block_proof_link_from_db_short(BlockIdExt block_id,
                                                              td::Promise<td::Ref<ProofLink>> promise) {
  auto P =
      td::PromiseCreator::lambda([db = db_.get(), promise = std::move(promise)](td::Result<BlockHandle> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
        } else {
          auto handle = R.move_as_ok();
          td::actor::send_closure(db, &Db::get_block_proof_link, std::move(handle), std::move(promise));
        }
      });
  get_block_handle(block_id, false, std::move(P));
}

void ValidatorManagerImpl::get_block_by_lt_from_db(AccountIdPrefixFull account, LogicalTime lt,
                                                   td::Promise<ConstBlockHandle> promise) {
  td::actor::send_closure(db_, &Db::get_block_by_lt, account, lt, std::move(promise));
}

void ValidatorManagerImpl::get_block_by_unix_time_from_db(AccountIdPrefixFull account, UnixTime ts,
                                                          td::Promise<ConstBlockHandle> promise) {
  td::actor::send_closure(db_, &Db::get_block_by_unix_time, account, ts, std::move(promise));
}

void ValidatorManagerImpl::get_block_by_seqno_from_db(AccountIdPrefixFull account, BlockSeqno seqno,
                                                      td::Promise<ConstBlockHandle> promise) {
  td::actor::send_closure(db_, &Db::get_block_by_seqno, account, seqno, std::move(promise));
}

void ValidatorManagerImpl::finished_wait_state(BlockIdExt block_id, td::Result<td::Ref<ShardState>> R) {
  auto it = wait_state_.find(block_id);
  if (it != wait_state_.end()) {
    if (R.is_error()) {
      auto S = R.move_as_error();
      for (auto &X : it->second.waiting_) {
        X.second.set_error(S.clone());
      }
    } else {
      auto r = R.move_as_ok();
      for (auto &X : it->second.waiting_) {
        X.second.set_result(r);
      }
    }
    wait_state_.erase(it);
  }
}

void ValidatorManagerImpl::finished_wait_data(BlockIdExt block_id, td::Result<td::Ref<BlockData>> R) {
  auto it = wait_block_data_.find(block_id);
  if (it != wait_block_data_.end()) {
    if (R.is_error()) {
      auto S = R.move_as_error();
      for (auto &X : it->second.waiting_) {
        X.second.set_error(S.clone());
      }
    } else {
      auto r = R.move_as_ok();
      for (auto &X : it->second.waiting_) {
        X.second.set_result(r);
      }
    }
    wait_block_data_.erase(it);
  }
}

void ValidatorManagerImpl::set_block_state(BlockHandle handle, td::Ref<ShardState> state,
                                           td::Promise<td::Ref<ShardState>> promise) {
  td::actor::send_closure(db_, &Db::store_block_state, handle, state, std::move(promise));
}

void ValidatorManagerImpl::get_cell_db_reader(td::Promise<std::shared_ptr<vm::CellDbReader>> promise) {
  td::actor::send_closure(db_, &Db::get_cell_db_reader, std::move(promise));
}

void ValidatorManagerImpl::store_persistent_state_file(BlockIdExt block_id, BlockIdExt masterchain_block_id,
                                                       td::BufferSlice state, td::Promise<td::Unit> promise) {
  td::actor::send_closure(db_, &Db::store_persistent_state_file, block_id, masterchain_block_id, std::move(state),
                          std::move(promise));
}

void ValidatorManagerImpl::store_persistent_state_file_gen(BlockIdExt block_id, BlockIdExt masterchain_block_id,
                                                           std::function<td::Status(td::FileFd&)> write_data,
                                                           td::Promise<td::Unit> promise) {
  td::actor::send_closure(db_, &Db::store_persistent_state_file_gen, block_id, masterchain_block_id,
                          std::move(write_data), std::move(promise));
}

void ValidatorManagerImpl::store_zero_state_file(BlockIdExt block_id, td::BufferSlice state,
                                                 td::Promise<td::Unit> promise) {
  td::actor::send_closure(db_, &Db::store_zero_state_file, block_id, std::move(state), std::move(promise));
}

void ValidatorManagerImpl::set_block_data(BlockHandle handle, td::Ref<BlockData> data, td::Promise<td::Unit> promise) {
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), promise = std::move(promise), handle](td::Result<td::Unit> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
        } else {
          handle->set_received();
          handle->flush(SelfId, handle, std::move(promise));
        }
      });

  td::actor::send_closure(db_, &Db::store_block_data, handle, std::move(data), std::move(P));
}

void ValidatorManagerImpl::set_block_proof(BlockHandle handle, td::Ref<Proof> proof, td::Promise<td::Unit> promise) {
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), promise = std::move(promise), handle](td::Result<td::Unit> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
        } else {
          promise.set_value(td::Unit());
        }
      });

  td::actor::send_closure(db_, &Db::store_block_proof, handle, std::move(proof), std::move(P));
}

void ValidatorManagerImpl::set_block_proof_link(BlockHandle handle, td::Ref<ProofLink> proof,
                                                td::Promise<td::Unit> promise) {
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), handle, promise = std::move(promise)](td::Result<td::Unit> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
        } else {
          promise.set_value(td::Unit());
        }
      });
  td::actor::send_closure(db_, &Db::store_block_proof_link, handle, std::move(proof), std::move(P));
}

void ValidatorManagerImpl::set_block_signatures(BlockHandle handle, td::Ref<BlockSignatureSet> signatures,
                                                td::Promise<td::Unit> promise) {
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), promise = std::move(promise), handle](td::Result<td::Unit> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
        } else {
          promise.set_value(td::Unit());
        }
      });

  td::actor::send_closure(db_, &Db::store_block_signatures, handle, std::move(signatures), std::move(P));
}

void ValidatorManagerImpl::set_next_block(BlockIdExt block_id, BlockIdExt next, td::Promise<td::Unit> promise) {
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), next, promise = std::move(promise)](td::Result<BlockHandle> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
        } else {
          auto handle = R.move_as_ok();
          handle->set_next(next);
          if (handle->need_flush()) {
            handle->flush(SelfId, handle, std::move(promise));
          } else {
            promise.set_value(td::Unit());
          }
        }
      });
  get_block_handle(block_id, true, std::move(P));
}

void ValidatorManagerImpl::set_block_candidate(BlockIdExt id, BlockCandidate candidate, td::Promise<td::Unit> promise) {
  td::actor::send_closure(db_, &Db::store_block_candidate, std::move(candidate), std::move(promise));
}

void ValidatorManagerImpl::write_handle(BlockHandle handle, td::Promise<td::Unit> promise) {
  td::actor::send_closure(db_, &Db::store_block_handle, std::move(handle), std::move(promise));
}

void ValidatorManagerImpl::new_block_cont(BlockHandle handle, td::Ref<ShardState> state,
                                          td::Promise<td::Unit> promise) {
  handle->set_processed();
  if (state->get_shard().is_masterchain() && handle->id().id.seqno > last_masterchain_seqno_) {
    CHECK(handle->id().id.seqno == last_masterchain_seqno_ + 1);
    last_masterchain_seqno_ = handle->id().id.seqno;
    last_masterchain_state_ = td::Ref<MasterchainState>{state};
    last_masterchain_block_id_ = handle->id();
    last_masterchain_block_handle_ = handle;

    update_shards();
    update_shard_blocks();

    td::actor::send_closure(db_, &Db::update_init_masterchain_block, last_masterchain_block_id_, std::move(promise));
  } else {
    promise.set_value(td::Unit());
  }
}

void ValidatorManagerImpl::new_block(BlockHandle handle, td::Ref<ShardState> state, td::Promise<td::Unit> promise) {
  if (handle->is_applied()) {
    return new_block_cont(std::move(handle), std::move(state), std::move(promise));
  } else {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), handle, state = std::move(state),
                                         promise = std::move(promise)](td::Result<td::Unit> R) mutable {
      if (R.is_error()) {
        promise.set_error(R.move_as_error());
      } else {
        td::actor::send_closure(SelfId, &ValidatorManagerImpl::new_block_cont, std::move(handle), std::move(state),
                                std::move(promise));
      }
    });
    td::actor::send_closure(db_, &Db::apply_block, handle, std::move(P));
  }
}

void ValidatorManagerImpl::get_block_handle(BlockIdExt id, bool force, td::Promise<BlockHandle> promise) {
  auto it = handles_.find(id);
  if (it != handles_.end()) {
    auto handle = it->second.lock();
    if (handle) {
      promise.set_value(std::move(handle));
      return;
    } else {
      handles_.erase(it);
    }
  }
  auto P = td::PromiseCreator::lambda(
      [id, force, promise = std::move(promise), SelfId = actor_id(this)](td::Result<BlockHandle> R) mutable {
        BlockHandle handle;
        if (R.is_error()) {
          auto S = R.move_as_error();
          if (S.code() == ErrorCode::notready && force) {
            handle = create_empty_block_handle(id);
          } else {
            promise.set_error(std::move(S));
            return;
          }
        } else {
          handle = R.move_as_ok();
        }
        td::actor::send_closure(SelfId, &ValidatorManagerImpl::register_block_handle, std::move(handle),
                                std::move(promise));
      });

  td::actor::send_closure(db_, &Db::get_block_handle, id, std::move(P));
}

void ValidatorManagerImpl::register_block_handle(BlockHandle handle, td::Promise<BlockHandle> promise) {
  auto it = handles_.find(handle->id());
  if (it != handles_.end()) {
    auto h = it->second.lock();
    if (h) {
      promise.set_value(std::move(h));
      return;
    }
    handles_.erase(it);
  }
  handles_.emplace(handle->id(), std::weak_ptr<BlockHandleInterface>(handle));
  promise.set_value(std::move(handle));
}

void ValidatorManagerImpl::get_top_masterchain_state(td::Promise<td::Ref<MasterchainState>> promise) {
  promise.set_result(last_masterchain_state_);
}

void ValidatorManagerImpl::get_top_masterchain_block(td::Promise<BlockIdExt> promise) {
  promise.set_result(last_masterchain_block_id_);
}

void ValidatorManagerImpl::get_top_masterchain_state_block(
    td::Promise<std::pair<td::Ref<MasterchainState>, BlockIdExt>> promise) {
  promise.set_result(
      std::pair<td::Ref<MasterchainState>, BlockIdExt>{last_masterchain_state_, last_masterchain_block_id_});
}

void ValidatorManagerImpl::send_get_block_request(BlockIdExt id, td::uint32 priority,
                                                  td::Promise<ReceivedBlock> promise) {
  UNREACHABLE();
}

void ValidatorManagerImpl::send_get_zero_state_request(BlockIdExt id, td::uint32 priority,
                                                       td::Promise<td::BufferSlice> promise) {
  UNREACHABLE();
}

void ValidatorManagerImpl::send_get_persistent_state_request(BlockIdExt id, BlockIdExt masterchain_block_id,
                                                             td::uint32 priority,
                                                             td::Promise<td::BufferSlice> promise) {
  UNREACHABLE();
}

void ValidatorManagerImpl::send_top_shard_block_description(td::Ref<ShardTopBlockDescription> desc) {
  callback_->send_shard_block_info(desc->block_id(), desc->catchain_seqno(), desc->serialize());
}

void ValidatorManagerImpl::start_up() {
  db_ = create_db_actor(actor_id(this), db_root_);

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<ValidatorManagerInitResult> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &ValidatorManagerImpl::started, R.move_as_ok());
  });

  validator_manager_init(opts_, actor_id(this), db_.get(), std::move(P));
}

void ValidatorManagerImpl::started(ValidatorManagerInitResult R) {
  LOG(DEBUG) << "started()";
  last_masterchain_block_handle_ = std::move(R.handle);
  last_masterchain_block_id_ = last_masterchain_block_handle_->id();
  last_masterchain_seqno_ = last_masterchain_block_id_.id.seqno;
  last_masterchain_state_ = std::move(R.state);

  //new_masterchain_block();

  callback_->initial_read_complete(last_masterchain_block_handle_);
}

void ValidatorManagerImpl::update_shards() {
}

void ValidatorManagerImpl::update_shard_blocks() {
  if (!last_masterchain_block_handle_) {
    return;
  }
  if (!shard_blocks_raw_.empty()) {
    for (auto &raw : shard_blocks_raw_) {
      new_shard_block(BlockIdExt{}, 0, std::move(raw));
    }
    shard_blocks_raw_.clear();
  }
  {
    auto it = shard_blocks_.begin();
    while (it != shard_blocks_.end()) {
      auto &B = *it;
      if (!B->may_be_valid(last_masterchain_block_handle_, last_masterchain_state_)) {
        auto it2 = it++;
        shard_blocks_.erase(it2);
      } else {
        ++it;
      }
    }
  }
  {
    auto it = out_shard_blocks_.begin();
    while (it != out_shard_blocks_.end()) {
      auto &B = *it;
      if (!B->may_be_valid(last_masterchain_block_handle_, last_masterchain_state_)) {
        auto it2 = it++;
        out_shard_blocks_.erase(it2);
      } else {
        ++it;
      }
    }
  }
}

ValidatorSessionId ValidatorManagerImpl::get_validator_set_id(ShardIdFull shard, td::Ref<ValidatorSet> val_set) {
  return create_hash_tl_object<ton_api::tonNode_sessionId>(shard.workchain, shard.shard, val_set->get_catchain_seqno(),
                                                           td::Bits256::zero());
}

void ValidatorManagerImpl::update_shard_client_state(BlockIdExt masterchain_block_id, td::Promise<td::Unit> promise) {
  td::actor::send_closure(db_, &Db::update_shard_client_state, masterchain_block_id, std::move(promise));
}

void ValidatorManagerImpl::get_shard_client_state(bool from_db, td::Promise<BlockIdExt> promise) {
  td::actor::send_closure(db_, &Db::get_shard_client_state, std::move(promise));
}

void ValidatorManagerImpl::try_get_static_file(FileHash file_hash, td::Promise<td::BufferSlice> promise) {
  td::actor::send_closure(db_, &Db::try_get_static_file, file_hash, std::move(promise));
}

td::actor::ActorOwn<ValidatorManagerInterface> ValidatorManagerDiskFactory::create(
    PublicKeyHash id, td::Ref<ValidatorManagerOptions> opts, ShardIdFull shard, BlockIdExt shard_top_block_id,
    std::string db_root) {
  return td::actor::create_actor<validator::ValidatorManagerImpl>("manager", id, std::move(opts), shard,
                                                                  shard_top_block_id, db_root);
}

}  // namespace validator

}  // namespace ton
