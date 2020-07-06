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

#include "Pacer.h"
namespace ton {
namespace rldp2 {
Pacer::Pacer(Options options)
    : speed_(options.initial_speed)
    , capacity_(options.initial_capacity)
    , max_capacity_(options.max_capacity)
    , time_granularity_(options.time_granularity) {
}

td::Timestamp Pacer::wakeup_at() const {
  return wakeup_at_;
}

void Pacer::set_speed(double speed) {
  if (speed < 1) {
    speed = 1;
  }
  speed_ = speed;
}

td::optional<td::Timestamp> Pacer::send(double size, td::Timestamp now) {
  update_capacity(now);

  if (size < capacity_) {
    capacity_ -= size;
    return {};
  }

  size -= capacity_;
  capacity_ = 0;
  wakeup_at_ = td::Timestamp::in(size / speed_, now);
  capacity_at_ = wakeup_at_;
  return wakeup_at_;
}

void Pacer::update_capacity(td::Timestamp now) {
  if (capacity_at_ && capacity_at_.is_in_past(now)) {
    capacity_ += (now.at() - capacity_at_.at()) * speed_;
    capacity_ = td::min(capacity_, td::max(max_capacity_, speed_ * time_granularity_));
  }
  capacity_at_ = now;
}
}  // namespace rldp2
}  // namespace ton
