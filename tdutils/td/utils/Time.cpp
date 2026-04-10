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
#include <atomic>
#include <cmath>
#include <mutex>

#include "td/utils/Time.h"

namespace td {

bool operator==(Timestamp a, Timestamp b) {
  return std::abs(a.at() - b.at()) < 1e-6;
}
namespace {
std::atomic<double> time_diff;

bool freezes_allowed{false};
std::mutex time_mutex;
bool is_frozen{false};
double frozen_time;
double frozen_system;
}  // namespace

double Time::now() {
  if (freezes_allowed) {
    std::lock_guard lock(time_mutex);
    if (is_frozen) {
      return frozen_time;
    }
    return now_unadjusted() + time_diff;
  }

  return now_unadjusted() + time_diff.load(std::memory_order_relaxed);
}

double Time::now_unadjusted() {
  return Clocks::monotonic();
}

double Time::system_now() {
  if (freezes_allowed) {
    std::lock_guard lock(time_mutex);
    if (is_frozen) {
      return frozen_system;
    }
  }

  return Clocks::system();
}

void Time::jump_in_future(double at) {
  if (freezes_allowed) {
    std::lock_guard lock(time_mutex);
    if (is_frozen) {
      if (at > frozen_time) {
        frozen_system += at - frozen_time;
        frozen_time = at;
      }
    } else {
      time_diff = at - now_unadjusted();
      if (time_diff < 0) {
        time_diff = 0;
      }
    }
    return;
  }

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

void Time::allow_freezes() {
  freezes_allowed = true;
}

void Time::freeze() {
  CHECK(freezes_allowed);
  std::lock_guard lock(time_mutex);
  frozen_time = now_unadjusted() + time_diff.load(std::memory_order_relaxed);
  frozen_system = Clocks::system();
  is_frozen = true;
}

void Time::unfreeze() {
  CHECK(freezes_allowed);
  std::lock_guard lock(time_mutex);
  if (is_frozen) {
    time_diff.store(frozen_time - now_unadjusted(), std::memory_order_relaxed);
    is_frozen = false;
  }
}

}  // namespace td
