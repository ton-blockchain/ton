#include <chrono>
#include <thread>

#include "td/utils/Time.h"
#include "td/utils/tests.h"

namespace td {
namespace {

using namespace std::chrono_literals;

TEST(Time, SteadyClockBasics) {
  static_assert(SteadyClock::is_steady);
  static_assert(std::same_as<SteadyClock::duration, std::chrono::nanoseconds>);

  auto t1 = SteadyClock::now();
  auto t2 = SteadyClock::now();
  EXPECT(t1 <= t2);

  auto ts = SteadyClock::from_double_ts(1.5);
  EXPECT_EQ(static_cast<int64>(1'500'000'000), ts.time_since_epoch().count());
}

TEST(Time, UTCClockBasics) {
  static_assert(!UTCClock::is_steady);
  static_assert(std::same_as<UTCClock::duration, std::chrono::nanoseconds>);

  auto utc = std::chrono::duration<double>(UTCClock::now().time_since_epoch()).count();
  EXPECT(utc > 1'600'000'000.0);
}

TEST(Time, ClockConversionRoundTrip) {
  Time::FreezeGuard time_freeze;

  auto utc = UTCClock::now();
  auto steady = to_steady_time(utc);
  auto utc_back = to_utc_time(steady);
  EXPECT_EQ(utc, utc_back);
}

TEST(Time, TimestampNeverAndBool) {
  Timestamp never;
  EXPECT(!static_cast<bool>(never));
  EXPECT(!static_cast<bool>(Timestamp::never()));

  auto now = Timestamp::now();
  EXPECT(static_cast<bool>(now));
}

TEST(Time, TimestampAtAndAtRoundTrip) {
  auto ts = Timestamp::at(123.456);
  EXPECT_EQ(123.456, ts.at());

  auto ts2 = Timestamp::at(0.0);
  EXPECT(!static_cast<bool>(ts2));
}

TEST(Time, TimestampAtUnixRoundTrip) {
  auto ts = Timestamp::at_unix(1'700'000'000.5);
  EXPECT_APPROX(1'700'000'000.5, ts.at_unix());
}

TEST(Time, TimestampInDouble) {
  auto base = Timestamp::now();
  auto later = Timestamp::in(2.5, base);
  EXPECT_EQ(2.5, later - base);
}

TEST(Time, TimestampInChrono) {
  auto base = Timestamp::now();
  EXPECT_EQ(2.5, Timestamp::in(2500ms, base) - base);
  EXPECT_EQ(2.5, Timestamp::in(2'500'000us, base) - base);
  EXPECT_EQ(2.5, Timestamp::in(2'500'000'000ns, base) - base);
  EXPECT_EQ(3.0, Timestamp::in(3s, base) - base);
}

template <typename T>
concept TimestampInTakes = requires(T v) { Timestamp::in(v); };

template <typename T>
concept TimestampPlusEqTakes = requires(Timestamp t, T v) { t += v; };

template <typename T>
concept TimestampMinusEqTakes = requires(Timestamp t, T v) { t -= v; };

TEST(Time, TimestampInTypeGate) {
  // Integer-rep chrono and raw double seconds are accepted...
  static_assert(TimestampInTakes<double>);
  static_assert(TimestampInTakes<std::chrono::nanoseconds>);
  static_assert(TimestampInTakes<std::chrono::milliseconds>);
  static_assert(TimestampInTakes<std::chrono::seconds>);
  // ...but duration<double> is rejected (lossy → forced opt-in via round).
  static_assert(!TimestampInTakes<std::chrono::duration<double>>);
}

TEST(Time, TimestampOrdering) {
  auto a = Timestamp::at(10.0);
  auto b = Timestamp::at(20.0);
  auto c = Timestamp::at(10.0);
  EXPECT(a < b);
  EXPECT(b > a);
  EXPECT(a == c);
  EXPECT(!(a == b));
  EXPECT(a <= c);
  EXPECT(a >= c);
}

TEST(Time, TimestampArithmeticDouble) {
  auto t = Timestamp::at(100.0);
  t += 5.0;
  EXPECT_EQ(105.0, t.at());
  t -= 2.5;
  EXPECT_EQ(102.5, t.at());

  EXPECT_EQ(103.5, (t + 1.0).at());
  EXPECT_EQ(101.5, (t - 1.0).at());
}

TEST(Time, TimestampArithmeticChrono) {
  auto t = Timestamp::at(100.0);
  t += 500ms;
  EXPECT_EQ(100.5, t.at());
  t -= 250ms;
  EXPECT_EQ(100.25, t.at());

  EXPECT_EQ(101.25, (t + 1s).at());
  EXPECT_EQ(100.2499, (t - 100us).at());
}

TEST(Time, TimestampArithmeticTypeGate) {
  static_assert(TimestampPlusEqTakes<double>);
  static_assert(TimestampPlusEqTakes<std::chrono::milliseconds>);
  static_assert(TimestampMinusEqTakes<double>);
  static_assert(TimestampMinusEqTakes<std::chrono::milliseconds>);
  // duration<double> is rejected — caller must round explicitly.
  static_assert(!TimestampPlusEqTakes<std::chrono::duration<double>>);
  static_assert(!TimestampMinusEqTakes<std::chrono::duration<double>>);
}

TEST(Time, TimestampDifference) {
  auto base = Timestamp::at(1e9);
  auto a = Timestamp::in(100.0, base);
  auto b = Timestamp::in(102.5, base);
  EXPECT_EQ(2.5, b - a);
  EXPECT_EQ(-2.5, a - b);
}

TEST(Time, TimestampIsInPast) {
  auto past = Timestamp::at(1.0);
  auto future = Timestamp::in(60.0);
  EXPECT(past.is_in_past());
  EXPECT(!future.is_in_past());

  auto pivot = Timestamp::at(50.0);
  EXPECT(Timestamp::at(40.0).is_in_past(pivot));
  EXPECT(!Timestamp::at(60.0).is_in_past(pivot));
  // Boundary: equal counts as in-past.
  EXPECT(Timestamp::at(50.0).is_in_past(pivot));
}

TEST(Time, TimestampRelax) {
  auto t = Timestamp::never();
  t.relax(Timestamp::at(100.0));
  EXPECT_EQ(100.0, t.at());

  t.relax(Timestamp::at(150.0));
  EXPECT_EQ(100.0, t.at());  // relax keeps the smaller

  t.relax(Timestamp::at(50.0));
  EXPECT_EQ(50.0, t.at());

  t.relax(Timestamp::never());
  EXPECT_EQ(50.0, t.at());  // never() must be a no-op
}

TEST(Time, TimestampGet) {
  auto base = Timestamp::at(100.0);
  auto returned = base.get();
  static_assert(std::same_as<decltype(returned), SteadyTime>);
  EXPECT_EQ(static_cast<int64>(100'000'000'000), returned.time_since_epoch().count());
}

TEST(Time, TimeNowMonotonic) {
  auto a = Time::now();
  std::this_thread::sleep_for(1ms);
  auto b = Time::now();
  EXPECT(b >= a);
  EXPECT(b - a >= 0.0005);
}

TEST(Time, JumpInFutureDouble) {
  auto target = Time::now() + 10.0;
  Time::jump_in_future(target);
  EXPECT(Time::now() >= target);
}

TEST(Time, JumpInFutureSteadyTime) {
  auto target = SteadyClock::now() + 10s;
  Time::jump_in_future(target);
  EXPECT(SteadyClock::now() >= target);
}

TEST(Time, FrozenJumpToSteadyTimeIsExact) {
  Time::FreezeGuard time_freeze;

  auto t0 = Timestamp::now().get();
  auto target = t0 + 5s + 250ms + 137ns;

  Time::jump_in_future(target);

  EXPECT_EQ(target, SteadyClock::now());
  EXPECT_EQ(target, Timestamp::now().get());
  EXPECT_EQ(5'250'000'137ns, SteadyClock::now() - t0);
}

TEST(Time, FreezeAndUnfreeze) {
  double t3;
  {
    Time::FreezeGuard time_freeze;
    auto t1 = Time::now();
    std::this_thread::sleep_for(2ms);
    auto t2 = Time::now();
    EXPECT_EQ(t1, t2);

    auto sys1 = Time::system_now();
    std::this_thread::sleep_for(2ms);
    auto sys2 = Time::system_now();
    EXPECT_EQ(sys1, sys2);

    Time::jump_in_future(t1 + 7.0);
    t3 = Time::now();
    auto sys3 = Time::system_now();
    EXPECT_EQ(t3 - t1, 7.0);
    EXPECT_EQ(sys3 - sys1, 7.0);
  }

  auto after = Time::now();
  EXPECT(after >= t3);
}

TEST(Time, StoreParseRoundTrip) {
  double original_unix = 1'700'000'000.25;
  auto ts = Timestamp::at_unix(original_unix);
  EXPECT_APPROX(original_unix, ts.at_unix());
}

TEST(Time, Overflow) {
  constexpr SteadyTime max{std::chrono::years{100}};
  constexpr SteadyTime min{-std::chrono::years{100}};

  auto ts_max1 = Timestamp::at(1. / 0);
  auto ts_max2 = Timestamp::in(1e13, ts_max1);
  EXPECT_EQ(ts_max1.get(), max);
  EXPECT_EQ(ts_max2.get(), SteadyTime{std::chrono::years{200}});
  auto ts_min = Timestamp::at(-1. / 0);
  EXPECT_EQ(ts_min.get(), min);
  auto ts_nan = Timestamp::at(0. / 0);
  EXPECT_EQ(ts_nan.get(), SteadyTime{});
}

}  // namespace
}  // namespace td
