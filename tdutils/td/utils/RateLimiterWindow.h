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
#include <deque>

#include "td/utils/Time.h"

namespace td {

struct RateLimiterWindow {
  RateLimiterWindow() = default;
  RateLimiterWindow(double size, size_t limit) : size(size), limit(limit) {
  }

  bool check(Timestamp time, size_t cost = 1);
  void insert(Timestamp time, size_t cost = 1);

 private:
  struct Entry {
    Timestamp time;
    size_t cost;
  };

  void gc(Timestamp time);

  double size = 0.0;
  size_t limit = 0;
  std::deque<Entry> entries = {};
  size_t used_cost = 0;
};

inline void RateLimiterWindow::gc(Timestamp time) {
  if (size == 0) {
    return;
  }
  while (!entries.empty() && time - entries.back().time > size) {
    used_cost -= entries.back().cost;
    entries.pop_back();
  }
}

inline bool RateLimiterWindow::check(Timestamp time, size_t cost) {
  if (size == 0) {
    return true;
  }
  if (limit == 0) {
    return false;
  }
  gc(time);
  return cost <= limit && used_cost + cost <= limit;
}

inline void RateLimiterWindow::insert(Timestamp time, size_t cost) {
  if (size == 0) {
    return;
  }
  gc(time);
  entries.push_front(Entry{time, cost});
  used_cost += cost;
}

}  // namespace td
