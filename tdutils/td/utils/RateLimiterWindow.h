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
#pragma once
#include <cmath>
#include <deque>

#include "td/utils/Time.h"

namespace td {

struct RateLimiterWindow {
  struct Params {
    double duration = 0.0;
    uint64 limit = 0;
    double granularity = 0.0;
  };

  RateLimiterWindow() = default;
  RateLimiterWindow(double duration, size_t limit, double granularity = 0.0)
      : duration_(duration), limit_(limit), granularity_(granularity) {
  }
  explicit RateLimiterWindow(Params params) : RateLimiterWindow(params.duration, params.limit, params.granularity) {
  }

  bool check(Timestamp time, size_t weight = 1);
  void insert(Timestamp time, size_t weight = 1);

  double duration() const {
    return duration_;
  }
  uint64 limit() const {
    return limit_;
  }

 private:
  struct Entry {
    Timestamp time;
    uint64 weight;
  };

  double duration_ = 0.0;
  size_t limit_ = 0;
  double granularity_ = 0.0;

  std::deque<Entry> entries_ = {};
  uint64 total_weight_ = 0;

  void gc(Timestamp time);

  Timestamp round_timestamp(Timestamp time) const {
    if (granularity_ == 0.0) {
      return time;
    }
    return Timestamp::at(std::floor(time.at() / granularity_) * granularity_);
  }
};

inline void RateLimiterWindow::gc(Timestamp time) {
  if (duration_ == 0.0) {
    return;
  }
  while (!entries_.empty() && time - entries_.back().time > duration_) {
    total_weight_ -= entries_.back().weight;
    entries_.pop_back();
  }
}

inline bool RateLimiterWindow::check(Timestamp time, size_t weight) {
  if (duration_ == 0) {
    return true;
  }
  if (limit_ == 0) {
    return false;
  }
  gc(time);
  return weight <= limit_ && total_weight_ + weight <= limit_;
}

inline void RateLimiterWindow::insert(Timestamp time, size_t weight) {
  if (duration_ == 0) {
    return;
  }
  gc(time);
  time = round_timestamp(time);
  if (!entries_.empty() && entries_.front().time == time) {
    entries_.front().weight += weight;
  } else {
    entries_.push_front(Entry{time, weight});
  }
  total_weight_ += weight;
}

}  // namespace td
