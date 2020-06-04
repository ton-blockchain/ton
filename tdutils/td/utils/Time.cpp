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
#include "td/utils/Time.h"

#include <cmath>
#include <atomic>

namespace td {

bool operator==(Timestamp a, Timestamp b) {
  return std::abs(a.at() - b.at()) < 1e-6;
}
namespace {
std::atomic<double> time_diff;
}
double Time::now() {
  return now_unadjusted() + time_diff.load(std::memory_order_relaxed);
}

double Time::now_unadjusted() {
  return Clocks::monotonic();
}

void Time::jump_in_future(double at) {
  auto old_time_diff = time_diff.load();

  while (true) {
    auto diff = at - now();
    if (diff < 0) {
      return;
    }
    if (time_diff.compare_exchange_strong(old_time_diff, old_time_diff + diff)) {
      return;
    }
  }
}

}  // namespace td
