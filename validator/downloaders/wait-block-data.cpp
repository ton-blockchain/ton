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

    Copyright 2017-2019 Telegram Systems LLP
*/
#include "wait-block-data.hpp"
#include "fabric.h"
#include "adnl/utils.hpp"
#include "ton/ton-io.hpp"
#include "common/delay.h"

namespace ton {

namespace validator {

void WaitBlockData::alarm() {
  abort_query(td::Status::Error(ErrorCode::timeout, "timeout"));
}

void WaitBlockData::abort_query(td::Status reason) {
  if (promise_) {
    if (priority_ > 0 || (reason.code() != ErrorCode::timeout && reason.code() != ErrorCode::notready)) {
      LOG(WARNING) << "aborting wait block data query for " << handle_->id() << " priority=" << priority_ << ": "
                   << reason;
    } else {
      LOG(DEBUG) << "aborting wait block data query for " << handle_->id() << " priority=" << priority_ << ": "
                 << reason;
    }
    promise_.set_error(reason.move_as_error_prefix(PSTRING() << "failed to download " << handle_->id() << ": "));
  }
  stop();
}

void WaitBlockData::finish_query() {
  CHECK(handle_->received());
  if (promise_) {
    promise_.set_result(data_);
  }
  stop();
}

void WaitBlockData::start_up() {
  alarm_timestamp() = timeout_;

  CHECK(handle_);
  if (!handle_->id().is_masterchain()) {
    start();
  } else {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<bool> R) {
      R.ensure();
      auto value = R.move_as_ok();
      td::actor::send_closure(SelfId, &WaitBlockData::set_is_hardfork, value);
    });
    td::actor::send_closure(manager_, &ValidatorManager::check_is_hardfork, handle_->id(), std::move(P));
  }
}

void WaitBlockData::set_is_hardfork(bool value) {
  is_hardfork_ = value;
  start();
}

void WaitBlockData::start() {
  if (reading_from_db_) {
    return;
  }
  if (handle_->received() &&
      (handle_->id().is_masterchain() ? handle_->inited_proof() : handle_->inited_proof_link())) {
    reading_from_db_ = true;

    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<BlockData>> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &WaitBlockData::abort_query, R.move_as_error_prefix("db get error: "));
      } else {
        td::actor::send_closure(SelfId, &WaitBlockData::got_block_data_from_db, R.move_as_ok());
      }
    });

    td::actor::send_closure(manager_, &ValidatorManager::get_block_data_from_db, handle_, std::move(P));
  } else if (try_read_static_file_.is_in_past() && (is_hardfork_ || !handle_->id().is_masterchain())) {
    try_read_static_file_ = td::Timestamp::in(30.0);

    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &WaitBlockData::start);
      } else {
        td::actor::send_closure(SelfId, &WaitBlockData::got_static_file, R.move_as_ok());
      }
    });

    td::actor::send_closure(manager_, &ValidatorManager::try_get_static_file, handle_->id().file_hash, std::move(P));
  } else {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<ReceivedBlock> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &WaitBlockData::failed_to_get_block_data_from_net,
                                R.move_as_error_prefix("net error: "));
      } else {
        td::actor::send_closure(SelfId, &WaitBlockData::got_block_data_from_net, R.move_as_ok());
      }
    });

    td::actor::send_closure(manager_, &ValidatorManager::send_get_block_request, handle_->id(), priority_,
                            std::move(P));
  }
}

void WaitBlockData::got_block_data_from_db(td::Ref<BlockData> data) {
  data_ = std::move(data);
  finish_query();
}

void WaitBlockData::failed_to_get_block_data_from_net(td::Status reason) {
  if (reason.code() == ErrorCode::notready) {
    LOG(DEBUG) << "failed to get block " << handle_->id() << " data from net: " << reason;
  } else {
    LOG(WARNING) << "failed to get block " << handle_->id() << " data from net: " << reason;
  }

  delay_action([SelfId = actor_id(this)]() mutable { td::actor::send_closure(SelfId, &WaitBlockData::start); },
               td::Timestamp::in(0.1));
}

void WaitBlockData::got_block_data_from_net(ReceivedBlock block) {
  auto X = create_block(std::move(block));
  if (X.is_error()) {
    failed_to_get_block_data_from_net(X.move_as_error_prefix("bad block from net: "));
    return;
  }
  data_ = X.move_as_ok();
  CHECK(handle_->id().is_masterchain() ? handle_->inited_proof() : handle_->inited_proof_link());
  if (!handle_->received()) {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &WaitBlockData::abort_query, R.move_as_error_prefix("db set error: "));
      } else {
        td::actor::send_closure(SelfId, &WaitBlockData::finish_query);
      }
    });

    td::actor::send_closure(manager_, &ValidatorManager::set_block_data, handle_, data_, std::move(P));
  } else {
    finish_query();
  }
}

void WaitBlockData::force_read_from_db() {
  if (reading_from_db_) {
    return;
  }
  CHECK(handle_->id().is_masterchain() ? handle_->inited_proof() : handle_->inited_proof_link());
  CHECK(handle_->received());
  reading_from_db_ = true;

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<BlockData>> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &WaitBlockData::abort_query, R.move_as_error_prefix("db read error: "));
    } else {
      td::actor::send_closure(SelfId, &WaitBlockData::got_block_data_from_db, R.move_as_ok());
    }
  });

  td::actor::send_closure(manager_, &ValidatorManager::get_block_data_from_db, handle_, std::move(P));
}

void WaitBlockData::got_static_file(td::BufferSlice data) {
  CHECK(td::sha256_bits256(data.as_slice()) == handle_->id().file_hash);

  auto R = create_block(handle_->id(), std::move(data));
  if (R.is_error()) {
    LOG(ERROR) << "bad static file block: " << R.move_as_error();
    start();
    return;
  }
  data_ = R.move_as_ok();

  CHECK(is_hardfork_ || !handle_->id().is_masterchain());

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &WaitBlockData::abort_query, R.move_as_error_prefix("bad static file block: "));
    } else {
      td::actor::send_closure(SelfId, &WaitBlockData::finish_query);
    }
  });
  run_hardfork_accept_block_query(handle_->id(), data_, manager_, std::move(P));
}

}  // namespace validator

}  // namespace ton
