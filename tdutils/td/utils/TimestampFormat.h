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

#include <chrono>
#include <cstddef>

#include "td/utils/Slice.h"
#include "td/utils/common.h"
#include "td/utils/date.h"

namespace td {

// Maximum bytes format_system_clock can write: "YYYY-MM-DD HH:MM:SS" + '.' + up to 9 fractional digits.
constexpr size_t TIMESTAMP_BUF_SIZE = 32;

// Formats `tp` exactly as date::format("%F %T", tp) does in the default (C) locale, i.e.
// "YYYY-MM-DD HH:MM:SS.ffffff" — but with NO std::ostringstream, NO std::locale, and NO heap allocation
// (date::format does all three per call, which is slow and, because of the shared-locale refcount and the
// per-line malloc, does not scale across threads). The fractional-second width adapts to system_clock's
// period at compile time (6 digits on libc++/µs, 9 on libstdc++/ns), matching date::format on each platform.
//
// `out` must be at least TIMESTAMP_BUF_SIZE bytes (CHECKed); returns the written prefix as a Slice.
// Byte-identical for years 1000..9999.
inline Slice format_system_clock(MutableSlice out, std::chrono::system_clock::time_point tp) {
  CHECK(out.size() >= TIMESTAMP_BUF_SIZE);
  using sys_dur = std::chrono::system_clock::duration;
  auto dp = date::floor<date::days>(tp);
  date::year_month_day ymd{dp};
  date::hh_mm_ss<sys_dur> hms{tp - dp};

  auto put2 = [](char *q, unsigned v) {
    q[0] = static_cast<char>('0' + v / 10);
    q[1] = static_cast<char>('0' + v % 10);
  };

  char *p = out.data();
  int y = static_cast<int>(ymd.year());
  *p++ = static_cast<char>('0' + (y / 1000) % 10);
  *p++ = static_cast<char>('0' + (y / 100) % 10);
  *p++ = static_cast<char>('0' + (y / 10) % 10);
  *p++ = static_cast<char>('0' + y % 10);
  *p++ = '-';
  put2(p, static_cast<unsigned>(ymd.month()));
  p += 2;
  *p++ = '-';
  put2(p, static_cast<unsigned>(ymd.day()));
  p += 2;
  *p++ = ' ';
  put2(p, static_cast<unsigned>(hms.hours().count()));
  p += 2;
  *p++ = ':';
  put2(p, static_cast<unsigned>(hms.minutes().count()));
  p += 2;
  *p++ = ':';
  put2(p, static_cast<unsigned>(hms.seconds().count()));
  p += 2;

  constexpr unsigned width = date::hh_mm_ss<sys_dur>::fractional_width;
  if (width > 0) {
    *p++ = '.';
    auto sub = hms.subseconds().count();
    for (int i = static_cast<int>(width) - 1; i >= 0; i--) {
      p[i] = static_cast<char>('0' + sub % 10);
      sub /= 10;
    }
    p += width;
  }
  return Slice(out.data(), static_cast<size_t>(p - out.data()));
}

}  // namespace td
