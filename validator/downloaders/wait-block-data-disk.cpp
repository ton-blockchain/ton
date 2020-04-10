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
#include "wait-block-data-disk.hpp"
#include "fabric.h"
#include "adnl/utils.hpp"
#include "ton/ton-io.hpp"

namespace ton {

namespace validator {

void WaitBlockDataDisk::alarm() {
  abort_query(td::Status::Error(ErrorCode::timeout, "timeout"));
}

void WaitBlockDataDisk::abort_query(td::Status reason) {
  if (promise_) {
    LOG(WARNING) << "aborting wait block data (disk) query for block " << handle_->id() << ": " << reason;
    promise_.set_error(reason.move_as_error_prefix(PSTRING() << "failed to download (disk) " << handle_->id() << ": "));
  }
  stop();
}

void WaitBlockDataDisk::finish_query() {
  CHECK(handle_->received());
  if (promise_) {
    promise_.set_result(data_);
  }
  stop();
}

void WaitBlockDataDisk::start_up() {
  alarm_timestamp() = timeout_;

  CHECK(handle_);

  start();
}

void WaitBlockDataDisk::start() {
  if (handle_->received() && (handle_->id().is_masterchain() ? handle_->inited_proof() : handle_->inited_proof_link())) {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<BlockData>> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &WaitBlockDataDisk::abort_query, R.move_as_error_prefix("db error: "));
      } else {
        td::actor::send_closure(SelfId, &WaitBlockDataDisk::got_block_data_from_db, R.move_as_ok());
      }
    });

    td::actor::send_closure(manager_, &ValidatorManager::get_block_data_from_db, handle_, std::move(P));
  } else {
    abort_query(td::Status::Error(ErrorCode::notready, "not in db"));
  }
}

void WaitBlockDataDisk::got_block_data_from_db(td::Ref<BlockData> data) {
  data_ = std::move(data);
  finish_query();
}

}  // namespace validator

}  // namespace ton
