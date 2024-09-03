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
#pragma once
#include "td/utils/int_types.h"

namespace td {

struct Clocks {
  static double monotonic();

  static double system();

  static int tz_offset();

#if defined(__i386__)
  static __inline__ td::uint64 rdtsc(void) {
    unsigned long long int x;
    __asm__ volatile("rdtsc" : "=A"(x));
    return x;
  }

  static constexpr td::uint64 rdtsc_frequency(void) {
    return 2000'000'000;
  }

  static constexpr double ticks_per_second() {
    return 2e9;
  }

  static constexpr double inv_ticks_per_second() {
    return 0.5e-9;
  }
#elif defined(__x86_64__)
  static __inline__ td::uint64 rdtsc(void) {
    unsigned hi, lo;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((unsigned long long)lo) | (((unsigned long long)hi) << 32);
  }
  static constexpr td::uint64 rdtsc_frequency(void) {
    return 2000'000'000;
  }

  static constexpr double ticks_per_second() {
    return 2e9;
  }

  static constexpr double inv_ticks_per_second() {
    return 0.5e-9;
  }
#elif defined(__aarch64__)
  static __inline__ td::uint64 rdtsc(void) {
    unsigned long long val;
    asm volatile("mrs %0, cntvct_el0" : "=r"(val));
    return val;
  }
  static __inline__ td::uint64 rdtsc_frequency(void) {
    unsigned long long val;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(val));
    return val;
  }

  static double ticks_per_second() {
    return static_cast<double>(rdtsc_frequency());
  }

  static double inv_ticks_per_second() {
    return 1.0 / static_cast<double>(rdtsc_frequency());
  }
#endif
};

}  // namespace td
