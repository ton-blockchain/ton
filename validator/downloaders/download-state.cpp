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
#include "download-state.hpp"
#include "validator/fabric.h"
#include "common/checksum.h"
#include "common/delay.h"
#include "ton/ton-io.hpp"

namespace ton {

namespace validator {

DownloadShardState::DownloadShardState(BlockIdExt block_id, BlockIdExt masterchain_block_id, td::uint32 priority,
                                       td::actor::ActorId<ValidatorManager> manager, td::Timestamp timeout,
                                       td::Promise<td::Ref<ShardState>> promise)
    : block_id_(block_id)
    , masterchain_block_id_(masterchain_block_id)
    , priority_(priority)
    , manager_(manager)
    , timeout_(timeout)
    , promise_(std::move(promise)) {
}

void DownloadShardState::start_up() {
  alarm_timestamp() = timeout_;

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<BlockHandle> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &DownloadShardState::got_block_handle, R.move_as_ok());
  });
  td::actor::send_closure(manager_, &ValidatorManager::get_block_handle, block_id_, true, std::move(P));
}

void DownloadShardState::got_block_handle(BlockHandle handle) {
  handle_ = std::move(handle);

  download_state();
}

void DownloadShardState::retry() {
  download_state();
}

void DownloadShardState::download_state() {
  if (handle_->id().seqno() == 0 || handle_->inited_proof() || handle_->inited_proof_link()) {
    checked_proof_link();
    return;
  }

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
    if (R.is_error()) {
      fail_handler(SelfId, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &DownloadShardState::downloaded_proof_link, R.move_as_ok());
    }
  });
  td::actor::send_closure(manager_, &ValidatorManager::send_get_block_proof_link_request, block_id_, priority_,
                          std::move(P));
}

void DownloadShardState::downloaded_proof_link(td::BufferSlice data) {
  auto pp = create_proof_link(block_id_, std::move(data));
  if (pp.is_error()) {
    fail_handler(actor_id(this), pp.move_as_error());
    return;
  }

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<BlockHandle> R) {
    if (R.is_error()) {
      fail_handler(SelfId, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &DownloadShardState::checked_proof_link);
    }
  });
  run_check_proof_link_query(block_id_, pp.move_as_ok(), manager_, td::Timestamp::in(60.0), std::move(P));
}

void DownloadShardState::checked_proof_link() {
  if (block_id_.seqno() == 0) {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &DownloadShardState::download_zero_state);
      } else {
        td::actor::send_closure(SelfId, &DownloadShardState::downloaded_zero_state, R.move_as_ok());
      }
    });
    td::actor::send_closure(manager_, &ValidatorManager::try_get_static_file, block_id_.file_hash, std::move(P));
  } else {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
      if (R.is_error()) {
        fail_handler(SelfId, R.move_as_error());
      } else {
        td::actor::send_closure(SelfId, &DownloadShardState::downloaded_shard_state, R.move_as_ok());
      }
    });
    CHECK(masterchain_block_id_.is_valid());
    CHECK(masterchain_block_id_.is_masterchain());
    td::actor::send_closure(manager_, &ValidatorManager::send_get_persistent_state_request, block_id_,
                            masterchain_block_id_, priority_, std::move(P));
  }
}

void DownloadShardState::download_zero_state() {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
    if (R.is_error()) {
      fail_handler(SelfId, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &DownloadShardState::downloaded_zero_state, R.move_as_ok());
    }
  });
  td::actor::send_closure(manager_, &ValidatorManager::send_get_zero_state_request, block_id_, priority_, std::move(P));
}

void DownloadShardState::downloaded_zero_state(td::BufferSlice data) {
  if (sha256_bits256(data.as_slice()) != block_id_.file_hash) {
    fail_handler(actor_id(this), td::Status::Error(ErrorCode::protoviolation, "bad zero state: file hash mismatch"));
    return;
  }

  data_ = std::move(data);
  auto S = create_shard_state(block_id_, data_.clone());
  S.ensure();
  state_ = S.move_as_ok();

  CHECK(state_->root_hash() == block_id_.root_hash);
  checked_shard_state();
}

void DownloadShardState::downloaded_shard_state(td::BufferSlice data) {
  auto S = create_shard_state(block_id_, data.clone());
  if (S.is_error()) {
    fail_handler(actor_id(this), S.move_as_error());
    return;
  }
  auto state = S.move_as_ok();
  if (state->root_hash() != handle_->state()) {
    fail_handler(actor_id(this),
                 td::Status::Error(ErrorCode::protoviolation, "bad persistent state: root hash mismatch"));
    return;
  }
  auto St = state->validate_deep();
  if (St.is_error()) {
    fail_handler(actor_id(this), St.move_as_error());
    return;
  }
  state_ = std::move(state);
  data_ = data.clone();
  checked_shard_state();
}

void DownloadShardState::checked_shard_state() {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &DownloadShardState::written_shard_state_file);
  });
  if (block_id_.seqno() == 0) {
    td::actor::send_closure(manager_, &ValidatorManager::store_zero_state_file, block_id_, std::move(data_),
                            std::move(P));
  } else {
    td::actor::send_closure(manager_, &ValidatorManager::store_persistent_state_file, block_id_, masterchain_block_id_,
                            std::move(data_), std::move(P));
  }
}

void DownloadShardState::written_shard_state_file() {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<ShardState>> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &DownloadShardState::written_shard_state, R.move_as_ok());
  });
  td::actor::send_closure(manager_, &ValidatorManager::set_block_state, handle_, std::move(state_), std::move(P));
}

void DownloadShardState::written_shard_state(td::Ref<ShardState> state) {
  state_ = std::move(state);
  handle_->set_unix_time(state_->get_unix_time());
  handle_->set_is_key_block(block_id_.is_masterchain());
  handle_->set_logical_time(state_->get_logical_time());
  handle_->set_applied();
  handle_->set_split(state_->before_split());
  if (!block_id_.is_masterchain()) {
    handle_->set_masterchain_ref_block(masterchain_block_id_.seqno());
  }

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), handle = handle_](td::Result<td::Unit> R) {
    CHECK(handle->handle_moved_to_archive());
    CHECK(handle->moved_to_archive())
    R.ensure();
    td::actor::send_closure(SelfId, &DownloadShardState::written_block_handle);
  });
  td::actor::send_closure(manager_, &ValidatorManager::archive, handle_, std::move(P));
}

void DownloadShardState::written_block_handle() {
  finish_query();
}

void DownloadShardState::finish_query() {
  if (promise_) {
    promise_.set_value(std::move(state_));
  }
  stop();
}

void DownloadShardState::alarm() {
  abort_query(td::Status::Error(ErrorCode::timeout, "timeout"));
}

void DownloadShardState::abort_query(td::Status reason) {
  if (promise_) {
    promise_.set_error(std::move(reason));
  }
  stop();
}

void DownloadShardState::fail_handler(td::actor::ActorId<DownloadShardState> SelfId, td::Status error) {
  LOG(WARNING) << "failed to download state : " << error;
  delay_action([=]() { td::actor::send_closure(SelfId, &DownloadShardState::retry); }, td::Timestamp::in(1.0));
}

}  // namespace validator

}  // namespace ton
