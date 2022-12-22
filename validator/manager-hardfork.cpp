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
#include "manager-hardfork.hpp"
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
#include "td/utils/filesystem.h"

namespace ton {

namespace validator {

void ValidatorManagerImpl::sync_complete(td::Promise<td::Unit> promise) {
  started_ = true;

  //ShardIdFull shard_id{masterchainId, shardIdAll};
  auto shard_id = shard_to_generate_;

  auto block_id = block_to_generate_;

  std::vector<BlockIdExt> prev{block_id};

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<BlockCandidate> R) {
    if (R.is_ok()) {
      auto v = R.move_as_ok();
      LOG(ERROR) << "created block " << v.id;
      td::actor::send_closure(SelfId, &ValidatorManagerImpl::created_candidate, std::move(v));
    } else {
      LOG(ERROR) << "failed to create block: " << R.move_as_error();
      std::exit(2);
    }
  });

  LOG(ERROR) << "running collate query";
  run_collate_hardfork(shard_id, block_id, prev, actor_id(this), td::Timestamp::in(10.0), std::move(P));
}

void ValidatorManagerImpl::created_candidate(BlockCandidate candidate) {
  td::write_file(db_root_ + "/static/" + candidate.id.file_hash.to_hex(), candidate.data.as_slice()).ensure();
  LOG(ERROR) << "success, block " << candidate.id << " = " << candidate.id.to_str() << " saved to disk";
  std::cout << candidate.id.to_str() << std::endl << std::flush;
  std::_Exit(0);
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

void ValidatorManagerImpl::get_block_proof_link(BlockHandle handle, td::Promise<td::BufferSlice> promise) {
  auto P = td::PromiseCreator::lambda(
      [promise = std::move(promise), handle, db = db_.get()](td::Result<td::Ref<ProofLink>> R) mutable {
        if (R.is_error()) {
          auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<td::Ref<Proof>> R) mutable {
            if (R.is_error()) {
              promise.set_error(R.move_as_error());
            } else {
              auto B = R.move_as_ok()->export_as_proof_link().move_as_ok();
              promise.set_value(B->data());
            }
          });

          td::actor::send_closure(db, &Db::get_block_proof, handle, std::move(P));
        } else {
          auto B = R.move_as_ok();
          promise.set_value(B->data());
        }
      });

  td::actor::send_closure(db_, &Db::get_block_proof_link, handle, std::move(P));
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
      [promise = std::move(promise), block_id, db = db_.get()](td::Result<td::Ref<ProofLink>> R) mutable {
        if (R.is_error()) {
          auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<td::Ref<Proof>> R) mutable {
            if (R.is_error()) {
              promise.set_error(R.move_as_error());
            } else {
              auto B = R.move_as_ok()->export_as_proof_link().move_as_ok();
              promise.set_value(B->data());
            }
          });

          td::actor::send_closure(db, &Db::get_key_block_proof, block_id, std::move(P));
        } else {
          auto B = R.move_as_ok();
          promise.set_value(B->data());
        }
      });

  td::actor::send_closure(db_, &Db::get_key_block_proof_link, block_id, std::move(P));
}

void ValidatorManagerImpl::new_external_message(td::BufferSlice data) {
  auto R = create_ext_message(std::move(data), block::SizeLimitsConfig::ExtMsgLimits());
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

void ValidatorManagerImpl::get_cell_db_reader(td::Promise<std::shared_ptr<vm::CellDbReader>> promise) {
  td::actor::send_closure(db_, &Db::get_cell_db_reader, std::move(promise));
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

void ValidatorManagerImpl::start_up() {
  db_ = create_db_actor(actor_id(this), db_root_);
}

void ValidatorManagerImpl::try_get_static_file(FileHash file_hash, td::Promise<td::BufferSlice> promise) {
  td::actor::send_closure(db_, &Db::try_get_static_file, file_hash, std::move(promise));
}

td::actor::ActorOwn<ValidatorManagerInterface> ValidatorManagerHardforkFactory::create(
    td::Ref<ValidatorManagerOptions> opts, ShardIdFull shard, BlockIdExt shard_top_block_id, std::string db_root) {
  return td::actor::create_actor<validator::ValidatorManagerImpl>("manager", std::move(opts), shard_top_block_id,
                                                                  db_root);
}

}  // namespace validator

}  // namespace ton
