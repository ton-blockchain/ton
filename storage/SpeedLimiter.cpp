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

#include "SpeedLimiter.h"

namespace ton {

SpeedLimiter::SpeedLimiter(double max_speed) : max_speed_(max_speed) {
}

void SpeedLimiter::set_max_speed(double max_speed) {
  max_speed_ = max_speed;
  auto old_queue = std::move(queue_);
  unlock_at_ = (queue_.empty() ? td::Timestamp::now() : queue_.front().execute_at_);
  queue_ = {};
  while (!old_queue.empty()) {
    auto &e = old_queue.front();
    enqueue(e.size_, e.timeout_, std::move(e.promise_));
    old_queue.pop();
  }
  process_queue();
}

void SpeedLimiter::enqueue(double size, td::Timestamp timeout, td::Promise<td::Unit> promise) {
  if (max_speed_ < 0.0) {
    promise.set_result(td::Unit());
    return;
  }
  if (max_speed_ == 0.0) {
    promise.set_error(td::Status::Error("Speed limit is 0"));
    return;
  }
  if (timeout < unlock_at_) {
    promise.set_error(td::Status::Error("Timeout caused by speed limit"));
    return;
  }
  if (queue_.empty() && unlock_at_.is_in_past()) {
    unlock_at_ = td::Timestamp::now();
    promise.set_result(td::Unit());
  } else {
    queue_.push({unlock_at_, size, timeout, std::move(promise)});
  }
  unlock_at_ = td::Timestamp::in(size / max_speed_, unlock_at_);
  if (!queue_.empty()) {
    alarm_timestamp() = queue_.front().execute_at_;
  }
}

void SpeedLimiter::alarm() {
  process_queue();
}

void SpeedLimiter::process_queue() {
  while (!queue_.empty()) {
    auto &e = queue_.front();
    if (e.execute_at_.is_in_past()) {
      e.promise_.set_result(td::Unit());
      queue_.pop();
    } else {
      break;
    }
  }
  if (!queue_.empty()) {
    alarm_timestamp() = queue_.front().execute_at_;
  }
}

}  // namespace ton
