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
#include "wait-block-state-merge.hpp"
#include "wait-block-state.hpp"
#include "ton/ton-io.hpp"

namespace ton {

namespace validator {

void WaitBlockStateMerge::abort_query(td::Status reason) {
  if (promise_) {
    LOG(WARNING) << "aborting wait block state merge query for " << left_ << " and " << right_ << ": " << reason;
    promise_.set_error(
        reason.move_as_error_prefix(PSTRING() << "failed to download merge " << left_ << " and " << right_ << ": "));
  }
  stop();
}

void WaitBlockStateMerge::finish_query(td::Ref<ShardState> result) {
  if (promise_) {
    promise_.set_value(std::move(result));
  }
  stop();
}

void WaitBlockStateMerge::alarm() {
  abort_query(td::Status::Error(ErrorCode::timeout, "timeout"));
}

void WaitBlockStateMerge::start_up() {
  alarm_timestamp() = timeout_;

  auto P_l = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<ShardState>> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &WaitBlockStateMerge::abort_query, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &WaitBlockStateMerge::got_answer, true, R.move_as_ok());
    }
  });

  td::actor::send_closure(manager_, &ValidatorManager::wait_block_state_short, left_, priority_, timeout_,
                          std::move(P_l));

  auto P_r = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<ShardState>> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &WaitBlockStateMerge::abort_query, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &WaitBlockStateMerge::got_answer, false, R.move_as_ok());
    }
  });

  td::actor::send_closure(manager_, &ValidatorManager::wait_block_state_short, right_, priority_, timeout_,
                          std::move(P_r));
}

void WaitBlockStateMerge::got_answer(bool left, td::Ref<ShardState> state) {
  (left ? left_state_ : right_state_) = std::move(state);
  if (left_state_.not_null() && right_state_.not_null()) {
    auto R = left_state_->merge_with(*right_state_.get());
    if (R.is_error()) {
      abort_query(R.move_as_error_prefix("failed to merge states: "));
    } else {
      finish_query(R.move_as_ok());
    }
  }
}

}  // namespace validator

}  // namespace ton
