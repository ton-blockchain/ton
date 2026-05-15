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

#include <chrono>

#include "td/utils/common.h"

namespace td {

namespace detail {

inline auto to_ns_duration(double value) {
  return std::chrono::round<std::chrono::nanoseconds>(std::chrono::duration<double>(value));
}

inline double duration_to_s(std::chrono::duration<double> duration) {
  return duration.count();
}

}  // namespace detail

struct SteadyClock {
  using rep = td::int64;
  using period = std::nano;
  using duration = std::chrono::duration<rep, period>;
  using time_point = std::chrono::time_point<SteadyClock>;

  static constexpr bool is_steady = true;

  static time_point now();

  static time_point from_double_ts(double ts) {
    return time_point{detail::to_ns_duration(ts)};
  }
};

using SteadyTime = SteadyClock::time_point;

struct UTCClock {
  using rep = td::int64;
  using period = std::nano;
  using duration = std::chrono::duration<rep, period>;
  using time_point = std::chrono::time_point<UTCClock>;

  static constexpr bool is_steady = false;

  static time_point now();

  static time_point from_double_ts(double ts) {
    return time_point{detail::to_ns_duration(ts)};
  }
};

using UTCTime = UTCClock::time_point;
using UTCMilliseconds = std::chrono::time_point<UTCClock, std::chrono::milliseconds>;

class StringBuilder;
StringBuilder &operator<<(StringBuilder &sb, td::UTCTime ts);
StringBuilder &operator<<(StringBuilder &sb, td::UTCMilliseconds ts);

inline SteadyTime to_steady_time(UTCTime ts) {
  auto steady_now = SteadyClock::now();
  auto utc_now = UTCClock::now();
  return steady_now + (ts - utc_now);
}

inline UTCTime to_utc_time(SteadyTime ts) {
  auto steady_now = SteadyClock::now();
  auto utc_now = UTCClock::now();
  return utc_now + (ts - steady_now);
}

class Time {
 public:
  static double now();

  static double now_cached() {
    // Temporary(?) use now in now_cached
    // Problem:
    //   thread A: check that now() > timestamp and notifies thread B
    //   thread B: must see that now() > timestamp()
    //
    //   now() and now_cached() must be monotonic
    //
    //   if a=now[_cached]() happens before b=now[_cached] than
    //     a <= b
    //
    // As an alternative we may say that now_cached is a thread local copy of now
    return now();
  }
  static double now_unadjusted();

  // Used for testing. After jump_in_future(at) is called, now() >= at.
  static void jump_in_future(double at);
  static void jump_in_future(SteadyTime ts);

  static void allow_freezes();  // must be sequenced before any now / jump_in_future call.

  // Freeze time: now() returns a fixed value that only changes via jump_in_future.
  static void freeze();
  static void unfreeze();

  static double system_now();

  class FreezeGuard {
   public:
    FreezeGuard() {
      Time::freeze();
    }
    ~FreezeGuard() {
      Time::unfreeze();
    }
    FreezeGuard(const FreezeGuard &) = delete;
    FreezeGuard &operator=(const FreezeGuard &) = delete;
    FreezeGuard(FreezeGuard &&) = delete;
    FreezeGuard &operator=(FreezeGuard &&) = delete;
  };
};

inline void relax_timeout_at(double *timeout, double new_timeout) {
  if (new_timeout == 0) {
    return;
  }
  if (*timeout == 0 || new_timeout < *timeout) {
    *timeout = new_timeout;
  }
}

class Timestamp {
  SteadyClock::time_point at_{};

 public:
  Timestamp() = default;

  static Timestamp never() {
    return Timestamp{};
  }
  static Timestamp now() {
    return Timestamp{SteadyClock::now()};
  }
  static Timestamp now_cached() {
    return Timestamp{SteadyClock::now()};
  }
  static Timestamp at(double ts) {
    return Timestamp{SteadyClock::from_double_ts(ts)};
  }
  static Timestamp at(SteadyTime ts) {
    return Timestamp{ts};
  }
  static Timestamp at_unix(double ts) {
    return Timestamp{to_steady_time(UTCClock::from_double_ts(ts))};
  }
  static Timestamp at_unix(UTCTime ts) {
    return Timestamp{to_steady_time(ts)};
  }

  static Timestamp in(auto timeout, td::Timestamp now = td::Timestamp::now_cached())
    requires requires {
      { now.at_ + timeout } -> std::convertible_to<SteadyTime>;
    }
  {
    return Timestamp{now.at_ + timeout};
  }

  static Timestamp in(double timeout, td::Timestamp now = td::Timestamp::now_cached()) {
    return Timestamp{now.at_ + detail::to_ns_duration(timeout)};
  }

  bool is_in_past(td::Timestamp now) const {
    return at_ <= now.at_;
  }
  bool is_in_past() const {
    return is_in_past(now_cached());
  }

  explicit operator bool() const {
    return at_ > SteadyTime{};
  }

  td::SteadyTime get() const {
    return at_;
  }
  double at() const {
    return detail::duration_to_s(at_.time_since_epoch());
  }
  double at_unix() const {
    return detail::duration_to_s(to_utc_time(at_).time_since_epoch());
  }

  double in() const {
    return detail::duration_to_s(at_ - SteadyClock::now());
  }

  void relax(const Timestamp &timeout) {
    if (!timeout) {
      return;
    }
    if (!*this || at_ > timeout.at_) {
      at_ = timeout.at_;
    }
  }

  std::strong_ordering operator<=>(const Timestamp &other) const = default;

  Timestamp &operator+=(auto value)
    requires requires {
      { at_ + value } -> std::convertible_to<SteadyTime>;
    }
  {
    at_ += value;
    return *this;
  }

  Timestamp &operator-=(auto value)
    requires requires {
      { at_ - value } -> std::convertible_to<SteadyTime>;
    }
  {
    at_ -= value;
    return *this;
  }

  Timestamp &operator+=(double value) {
    at_ += detail::to_ns_duration(value);
    return *this;
  }

  Timestamp &operator-=(double value) {
    at_ -= detail::to_ns_duration(value);
    return *this;
  }

  Timestamp operator+(auto b) const
    requires requires(Timestamp a) { a += b; }
  {
    Timestamp a = *this;
    a += b;
    return a;
  }

  Timestamp operator-(auto b) const
    requires requires(Timestamp a) { a -= b; }
  {
    Timestamp a = *this;
    a -= b;
    return a;
  }

  double operator-(const Timestamp &b) {
    return detail::duration_to_s(get() - b.get());
  }

 private:
  explicit Timestamp(SteadyClock::time_point timeout) : at_(timeout) {
  }
};

template <class StorerT>
void store(const Timestamp &timestamp, StorerT &storer) {
  storer.store_binary(timestamp.at_unix());
}

template <class ParserT>
void parse(Timestamp &timestamp, ParserT &parser) {
  timestamp = Timestamp::at_unix(parser.fetch_double());
}

}  // namespace td
