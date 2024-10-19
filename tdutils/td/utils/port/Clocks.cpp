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
#include "td/utils/port/Clocks.h"

#include <chrono>
#include <ctime>

namespace td {
int64 Clocks::monotonic_nano() {
  auto duration = std::chrono::steady_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
}

double Clocks::monotonic() {
  // TODO write system specific functions, because std::chrono::steady_clock is steady only under Windows
  auto duration = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count()) * 1e-9;
}

double Clocks::system() {
  auto duration = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count()) * 1e-9;
}

int Clocks::tz_offset() {
  // not thread-safe on POSIX, so calculate the offset only once
  static int offset = [] {
    auto now = std::time(nullptr);

    auto time_ptr = std::localtime(&now);
    if (time_ptr == nullptr) {
      return 0;
    }
    auto local_time = *time_ptr;

    time_ptr = std::gmtime(&now);
    if (time_ptr == nullptr) {
      return 0;
    }
    auto utc_time = *time_ptr;

    int minute_offset = local_time.tm_min - utc_time.tm_min;
    int hour_offset = local_time.tm_hour - utc_time.tm_hour;
    int day_offset = local_time.tm_mday - utc_time.tm_mday;
    if (day_offset >= 20) {
      day_offset = -1;
    } else if (day_offset <= -20) {
      day_offset = 1;
    }
    int sec_offset = day_offset * 86400 + hour_offset * 3600 + minute_offset * 60;
    if (sec_offset >= 15 * 3600 || sec_offset <= -15 * 3600) {
      return 0;
    }
    return sec_offset / 900 * 900;  // round to 900 just in case
  }();
  return offset;
}

static int init_tz_offset = Clocks::tz_offset();

}  // namespace td
