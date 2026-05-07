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
#include <mutex>

#include "td/utils/Time.h"

namespace td {

namespace {

std::atomic<SteadyClock::duration> time_diff{};
static_assert(time_diff.is_always_lock_free);

bool freezes_allowed{false};
std::mutex time_mutex;
bool is_frozen{false};
SteadyTime frozen_steady;
UTCTime frozen_utc;

SteadyTime steady_unadjusted() {
  return SteadyTime{std::chrono::steady_clock::now().time_since_epoch()};
}

UTCTime utc_unadjusted() {
  return UTCTime{std::chrono::system_clock::now().time_since_epoch()};
}

}  // namespace

SteadyTime SteadyClock::now() {
  if (freezes_allowed) {
    std::lock_guard lock(time_mutex);
    if (is_frozen) {
      return frozen_steady;
    }
    return steady_unadjusted() + time_diff.load();
  }

  return steady_unadjusted() + time_diff.load(std::memory_order_relaxed);
}

UTCTime UTCClock::now() {
  if (freezes_allowed) {
    std::lock_guard lock(time_mutex);
    if (is_frozen) {
      return frozen_utc;
    }
  }

  return utc_unadjusted();
}

double Time::now() {
  return detail::duration_to_s(SteadyClock::now().time_since_epoch());
}

double Time::now_unadjusted() {
  return detail::duration_to_s(steady_unadjusted().time_since_epoch());
}

double Time::system_now() {
  return detail::duration_to_s(UTCClock::now().time_since_epoch());
}

void Time::jump_in_future(SteadyTime ts) {
  if (freezes_allowed) {
    std::lock_guard lock(time_mutex);
    if (is_frozen) {
      if (ts > frozen_steady) {
        frozen_utc += (ts - frozen_steady);
        frozen_steady = ts;
      }
    } else {
      auto diff = ts - steady_unadjusted();
      if (diff < SteadyClock::duration::zero()) {
        diff = SteadyClock::duration::zero();
      }
      time_diff.store(diff);
    }
    return;
  }

  auto old_time_diff = time_diff.load();
  while (true) {
    auto diff = ts - SteadyClock::now();
    if (diff < SteadyClock::duration::zero()) {
      return;
    }
    if (time_diff.compare_exchange_strong(old_time_diff, old_time_diff + diff)) {
      return;
    }
  }
}

void Time::jump_in_future(double at) {
  jump_in_future(SteadyClock::from_double_ts(at));
}

void Time::allow_freezes() {
  freezes_allowed = true;
}

void Time::freeze() {
  CHECK(freezes_allowed);
  std::lock_guard lock(time_mutex);
  frozen_steady = steady_unadjusted() + time_diff.load(std::memory_order_relaxed);
  frozen_utc = utc_unadjusted();
  is_frozen = true;
}

void Time::unfreeze() {
  CHECK(freezes_allowed);
  std::lock_guard lock(time_mutex);
  if (is_frozen) {
    time_diff.store(frozen_steady - steady_unadjusted(), std::memory_order_relaxed);
    is_frozen = false;
  }
}

}  // namespace td
