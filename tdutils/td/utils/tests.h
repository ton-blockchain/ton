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

#include <atomic>
#include <functional>
#include <optional>
#include <sstream>
#include <utility>

#include "td/utils/Context.h"
#include "td/utils/Random.h"
#include "td/utils/Slice.h"
#include "td/utils/Span.h"
#include "td/utils/Status.h"
#include "td/utils/common.h"
#include "td/utils/format.h"
#include "td/utils/logging.h"
#include "td/utils/port/thread.h"

#define REGISTER_TESTS(x)                \
  void TD_CONCAT(register_tests_, x)() { \
  }
#define DESC_TESTS(x) void TD_CONCAT(register_tests_, x)()
#define LOAD_TESTS(x) TD_CONCAT(register_tests_, x)()

namespace td {

class RandomSteps {
 public:
  struct Step {
    std::function<void()> func;
    td::uint32 weight;
  };
  RandomSteps(std::vector<Step> steps) : steps_(std::move(steps)) {
    for (auto &step : steps_) {
      steps_sum_ += step.weight;
    }
  }
  template <class Random>
  void step(Random &rnd) {
    auto w = rnd() % steps_sum_;
    for (auto &step : steps_) {
      if (w < step.weight) {
        step.func();
        break;
      }
      w -= step.weight;
    }
  }

 private:
  std::vector<Step> steps_;
  td::int32 steps_sum_ = 0;
};

class RegressionTester {
 public:
  virtual ~RegressionTester() = default;
  static void destroy(CSlice db_path);
  static unique_ptr<RegressionTester> create(string db_path, string db_cache_dir = "");

  virtual Status verify_test(Slice name, Slice result) = 0;
  virtual void save_db() = 0;
};

class Test {
 public:
  virtual ~Test() = default;
  virtual void run() {
    while (step()) {
    }
  }
  virtual bool step() {
    run();
    return false;
  }
  Test() = default;
  Test(const Test &) = delete;
  Test &operator=(const Test &) = delete;
  Test(Test &&) = delete;
  Test &operator=(Test &&) = delete;
};

class TestContext : public Context<TestContext> {
 public:
  virtual ~TestContext() = default;
  virtual Slice name() = 0;
  virtual Status verify(Slice data) = 0;
};

class TestsRunner : public TestContext {
 public:
  static TestsRunner &get_default();

  void add_test(string name, unique_ptr<Test> test);
  void add_substr_filter(string str);
  void set_stress_flag(bool flag);
  void run_all();
  bool run_all_step();
  void set_regression_tester(unique_ptr<RegressionTester> regression_tester);

 private:
  struct State {
    size_t it{0};
    bool is_running = false;
    double start{0};
    double start_unadjusted{0};
    size_t end{0};
  };
  bool stress_flag_{false};
  vector<string> substr_filters_;
  vector<std::pair<string, unique_ptr<Test>>> tests_;
  State state_;
  unique_ptr<RegressionTester> regression_tester_;

  Slice name() override;
  Status verify(Slice data) override;
};

template <class T>
class RegisterTest {
 public:
  explicit RegisterTest(string name, TestsRunner &runner = TestsRunner::get_default()) {
    runner.add_test(name, make_unique<T>());
  }
};

class Stage {
 public:
  void wait(uint64 need) {
    value_.fetch_add(1, std::memory_order_release);
    while (value_.load(std::memory_order_acquire) < need) {
      td::this_thread::yield();
    }
  };

 private:
  std::atomic<uint64> value_{0};
};

inline string rand_string(int from, int to, size_t len) {
  string res(len, '\0');
  for (auto &c : res) {
    c = static_cast<char>(Random::fast(from, to));
  }
  return res;
}

inline vector<string> rand_split(Slice str) {
  vector<string> res;
  size_t pos = 0;
  while (pos < str.size()) {
    size_t len;
    if (Random::fast(0, 1) == 1) {
      len = Random::fast(1, 10);
    } else {
      len = Random::fast(100, 200);
    }
    res.push_back(str.substr(pos, len).str());
    pos += len;
  }
  return res;
}

namespace detail {

std::optional<std::string> stringify(auto const &value) {
  if constexpr (requires(std::ostringstream builder) { builder << value; }) {
    std::ostringstream builder;
    builder << value;
    return builder.str();
  } else if constexpr (requires { PSTRING() << value; }) {
    return PSTRING() << value;
  }
  return std::nullopt;
}

inline std::optional<std::string> check(bool condition, char const *msg) {
  if (condition) {
    return std::nullopt;
  }

  return PSTRING() << "Expectation failed: " << msg << "!";
}

std::optional<std::string> check_eq(auto const &a_value, auto const &b_value, char const *a_expr, char const *b_expr) {
  if (a_value == b_value) {
    return std::nullopt;
  }

  std::ostringstream builder;
  builder << "Expectation failed: " << a_expr << " is not equal to " << b_expr;
  if (auto a_str = stringify(a_value), b_str = stringify(b_value); a_str.has_value() && b_str.has_value()) {
    builder << " (" << *a_str << " != " << *b_str << ")";
  }
  return builder.str();
}

}  // namespace detail

}  // namespace td

#define ASSERT_EQ(a, b)                                              \
  do {                                                               \
    if (auto error_message = ::td::detail::check_eq(a, b, #a, #b)) { \
      LOG(FATAL) << *error_message;                                  \
    }                                                                \
  } while (0)

#define ASSERT_TRUE(cond)                                                           \
  do {                                                                              \
    if (auto error_message = ::td::detail::check(static_cast<bool>(cond), #cond)) { \
      LOG(FATAL) << *error_message;                                                 \
    }                                                                               \
  } while (0)

#define ASSERT_STREQ(a, b)                                                                         \
  do {                                                                                             \
    if (auto error_message = ::td::detail::check_eq(::td::Slice((a)), ::td::Slice((b)), #a, #b)) { \
      LOG(FATAL) << *error_message;                                                                \
    }                                                                                              \
  } while (0)

#define REGRESSION_VERIFY(data) ::td::TestContext::get()->verify(data).ensure()

#define TEST_NAME(test_case_name, test_name) \
  TD_CONCAT(Test, TD_CONCAT(_, TD_CONCAT(test_case_name, TD_CONCAT(_, test_name))))

#define TEST(test_case_name, test_name) TEST_IMPL(TEST_NAME(test_case_name, test_name))

#define TEST_IMPL(test_name)                                                                                         \
  class test_name : public ::td::Test {                                                                              \
   public:                                                                                                           \
    using Test::Test;                                                                                                \
    void run() final;                                                                                                \
  };                                                                                                                 \
  ::td::RegisterTest<test_name> TD_CONCAT(test_instance_, TD_CONCAT(test_name, __LINE__))(TD_DEFINE_STR(test_name)); \
  void test_name::run()
