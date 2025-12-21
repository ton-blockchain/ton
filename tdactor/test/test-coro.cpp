#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "td/actor/actor.h"
#include "td/actor/coro.h"
#include "td/utils/Random.h"
#include "td/utils/port/sleep.h"
#include "td/utils/tests.h"

using namespace td::actor;

template <class T>
inline void expect_ok(const td::Result<T>& r, const char* msg) {
  LOG_CHECK(r.is_ok()) << msg;
}

template <class T, class U>
inline void expect_eq(const T& a, const U& b, const char* msg) {
  LOG_CHECK(a == b) << msg;
}

inline void expect_true(bool cond, const char* msg) {
  LOG_CHECK(cond) << msg;
}

inline void small_sleep_ms(int ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// Minimal custom awaitables used to validate await_transform branches
struct HandleReturningAwaitable {
  std::coroutine_handle<> stored_handle;
  int value;
  bool ready;

  explicit HandleReturningAwaitable(int v = 42, bool r = false) : value(v), ready(r) {
  }

  bool await_ready() const noexcept {
    return ready;
  }
  std::coroutine_handle<> await_suspend(std::coroutine_handle<> h) noexcept {
    stored_handle = h;
    return h;  // symmetric transfer form
  }
  int await_resume() noexcept {
    return value;
  }
};

struct BoolReturningAwaitable {
  std::coroutine_handle<> stored_handle;
  int value;
  bool ready;
  bool should_suspend;

  explicit BoolReturningAwaitable(int v = 43, bool r = false, bool suspend = true)
      : value(v), ready(r), should_suspend(suspend) {
  }

  bool await_ready() const noexcept {
    return ready;
  }
  bool await_suspend(std::coroutine_handle<> h) noexcept {
    stored_handle = h;
    if (should_suspend) {
      detail::SchedulerExecutor{}.schedule(h);
    }
    return should_suspend;
  }
  int await_resume() noexcept {
    return value;
  }
};

struct VoidReturningAwaitable {
  std::coroutine_handle<> stored_handle;
  int value;
  bool ready;

  explicit VoidReturningAwaitable(int v = 44, bool r = false) : value(v), ready(r) {
  }

  bool await_ready() const noexcept {
    return ready;
  }
  void await_suspend(std::coroutine_handle<> h) noexcept {
    stored_handle = h;
    detail::SchedulerExecutor{}.schedule(h);
  }
  int await_resume() noexcept {
    return value;
  }
};

// Simple utility actors used by tests
class TestLogger final : public td::actor::Actor {
 public:
  Task<td::Unit> log(std::string msg) {
    small_sleep_ms(10);
    LOG(INFO) << "[Logger] " << msg;
    co_return td::Unit();
  }

  void log_promise(std::string msg, td::Promise<td::Unit> promise) {
    small_sleep_ms(10);
    LOG(INFO) << "[Logger Promise] " << msg;
    promise.set_value(td::Unit());
  }

  Task<int> multiply2(int x) {
    co_return x * 2;
  }

  void multiply3(int x, td::Promise<int> promise) {
    promise.set_value(x * 3);
  }
};

class TestDatabase final : public td::actor::Actor {
 public:
  explicit TestDatabase(td::actor::ActorId<TestLogger> logger) : logger_(logger) {
  }

  Task<int> calcA(const std::string& key) {
    if (key.empty())
      co_return td::Status::Error("empty key");
    small_sleep_ms(5);
    co_return static_cast<int>(key.size()) * 10;
  }

  Task<size_t> square(size_t x) {
    co_return x* x;
  }

  Task<std::string> get(std::string key) {
    int ai = co_await calcA(key);  // Tasks propagate errors by default
    (void)co_await ask(logger_, &TestLogger::log, std::string("DB get ") + key);
    small_sleep_ms(5);
    if (key == "user")
      co_return std::string("Alice:") + std::to_string(ai);
    co_return td::Status::Error("not found");
  }

 private:
  td::actor::ActorId<TestLogger> logger_;
};

// 4) Tests grouped by topic
class CoroSpec final : public td::actor::Actor {
 public:
  void start_up() override {
    logger_ = td::actor::create_actor<TestLogger>("TestLogger").release();
    db_ = td::actor::create_actor<TestDatabase>("TestDatabase", logger_).release();
    [](Task<td::Unit> test) -> Task<td::Unit> {
      (co_await std::move(test).wrap()).ensure();
      co_await yield_on_current();
      td::actor::SchedulerContext::get()->stop();
      co_return td::Unit{};
    }(run_all())
                                   .start_immediate()
                                   .detach();
  }

  Task<td::Unit> unified_queries() {
    LOG(INFO) << "=== unified queries ===";

    using Value = std::unique_ptr<int>;
    static auto make_value = []() { return std::make_unique<int>(7); };
    class Uni : public td::actor::Actor {
     public:
      Value get_value() {
        return make_value();
      }
      td::Result<Value> get_result() {
        return make_value();
      }
      td::Result<Value> get_result_err() {
        return td::Status::Error("error");
      }
      Task<Value> get_task() {
        co_return get_value();
      }
      Task<Value> get_task_err() {
        co_return td::Status::Error("error");
      }
      void get_via_promise(td::Promise<Value> promise) {
        promise.set_value(get_value());
      }
      void get_via_promise_err(td::Promise<Value> promise) {
        promise.set_error(td::Status::Error("error"));
      }
      void get_void() {
      }
    };
    auto uni = td::actor::create_actor<Uni>("UnifiedResult");

    auto check = [](td::Result<Value> v) {
      CHECK(v.ok());
      CHECK(*v.ok() == 7);
    };
    auto check_value = [](Value v) { CHECK(*v == 7); };
    auto check_ok = [](td::Result<td::Unit> v) { v.ensure(); };
    auto check_err = [](td::Result<Value> v) { v.ensure_error(); };

    auto meta_ask = [&](auto&&... args) -> Task<td::Unit> {
      LOG(INFO) << "meta_ask: ask(args...)";
      check(co_await ask(args...).wrap());
      LOG(INFO) << "meta_ask: ask_immediate(args...)";
      check(co_await ask_immediate(args...).wrap());
      LOG(INFO) << "meta_ask: co_try(ask_immediate(args...))";
      check_value(co_await ask_immediate(args...));
      LOG(INFO) << "meta_ask: co_try(ask(args...))";
      check_value(co_await ask(args...));
      //check(co_await ask_new(args...));

      auto [bridge_task, bridge_promise] = StartedTask<td::Unit>::make_bridge();
      auto promise = [&](auto value) {
        check(std::move(value));
        bridge_promise.set_value(td::Unit{});
      };
      auto task = ask(args...);
      td::connect(std::move(promise), std::move(task));
      co_await std::move(bridge_task);

      co_return td::Unit{};
    };

    auto meta_ask_err = [&](auto&&... args) -> Task<td::Unit> {
      LOG(INFO) << "meta_ask_err: ask(args...)";
      check_err(co_await ask(args...).wrap());
      LOG(INFO) << "meta_ask_err: ask_immediate(args...)";
      check_err(co_await ask_immediate(args...).wrap());

      check_err(co_await [](auto&&... args) -> Task<Value> {
        LOG(INFO) << "meta_ask_err: co_try(ask_immediate(args...))";
        co_return co_await ask_immediate(args...);
      }(args...)
                                                   .wrap());
      check_err(co_await [](auto&&... args) -> Task<Value> {
        LOG(INFO) << "meta_ask_err: co_try(ask_immediate(args...))";
        co_return co_await ask_immediate(args...);
      }(args...)
                                                   .wrap());
      check_err(co_await [](auto&&... args) -> Task<Value> {
        LOG(INFO) << "meta_ask_err: co_try(ask_immediate(args...))";
        co_await ask(args...);
        co_return std::make_unique<int>(17);
      }(args...)
                                                   .wrap());

      LOG(INFO) << "meta_ask_err: co_try(ask(args...))";
      //check(co_await ask_new(args...));
      co_return td::Unit{};
    };

    // ask from coroutines
    check_ok(co_await meta_ask(uni, &Uni::get_result));
    check_ok(co_await meta_ask(uni, &Uni::get_task));
    check_ok(co_await meta_ask(uni, &Uni::get_via_promise));
    check_ok(co_await meta_ask(uni, &Uni::get_value));
    co_await ask(uni, &Uni::get_void);
    co_await ask_immediate(uni, &Uni::get_void);

    check_ok(co_await meta_ask_err(uni, &Uni::get_result_err));
    check_ok(co_await meta_ask_err(uni, &Uni::get_task_err));
    check_ok(co_await meta_ask_err(uni, &Uni::get_via_promise_err));

    check(co_await ask_new(uni, &Uni::get_task));

    static_assert(td::is_promise_interface<StartedTask<int>::ExternalPromise>());

    auto check_send_closure = [&](auto&& f) -> Task<td::Unit> {
      auto [task, task_promise] = StartedTask<Value>::make_bridge();
      td::Promise<Value> promise = [moved_task_promise = std::move(task_promise)](td::Result<Value> r) mutable {
        moved_task_promise.set_result(std::move(r));
      };
      f(std::move(promise));
      send_closure(uni, &Uni::get_value, std::move(promise));
      check(co_await std::move(task));
      co_return td::Unit{};
    };

    co_await check_send_closure([&](auto promise) { send_closure(uni, &Uni::get_result, std::move(promise)); });
    co_await check_send_closure([&](auto promise) { send_closure(uni, &Uni::get_value, std::move(promise)); });
    co_await check_send_closure([&](auto promise) { send_closure(uni, &Uni::get_via_promise, std::move(promise)); });
    co_await check_send_closure([&](auto promise) { send_closure(uni, &Uni::get_task, std::move(promise)); });

    bool done = false;
    LOG(INFO) << "Test send_closure_immediate";
    send_closure_immediate(uni, &Uni::get_void, [&](td::Result<td::Unit> r) {
      (void)r;
      done = true;
    });
    CHECK(done);

    co_return td::Unit();
  }

  // A. Awaitable branch coverage (handle/bool/void; ready/suspend)
  Task<td::Unit> awaitable_branches() {
    LOG(INFO) << "=== Awaitable branches ===";

    struct Case {
      const char* name;
      int expected;
      std::function<Task<int>()> run;
    };
    std::vector<Case> cases = {
        {"handle:not_ready", 100, [&]() -> Task<int> { co_return co_await HandleReturningAwaitable(100, false); }},
        {"handle:ready", 101, [&]() -> Task<int> { co_return co_await HandleReturningAwaitable(101, true); }},
        {"bool:suspend", 200, [&]() -> Task<int> { co_return co_await BoolReturningAwaitable(200, false, true); }},
        {"bool:immediate", 201, [&]() -> Task<int> { co_return co_await BoolReturningAwaitable(201, false, false); }},
        {"bool:ready", 202, [&]() -> Task<int> { co_return co_await BoolReturningAwaitable(202, true, true); }},
        {"void:not_ready", 300, [&]() -> Task<int> { co_return co_await VoidReturningAwaitable(300, false); }},
        {"void:ready", 301, [&]() -> Task<int> { co_return co_await VoidReturningAwaitable(301, true); }},
    };
    for (auto& c : cases) {
      auto r = co_await c.run();
      expect_eq(r, c.expected, c.name);
    }

    int sum = 0;
    sum += co_await HandleReturningAwaitable(10, false);
    sum += co_await BoolReturningAwaitable(20, false, true);
    sum += co_await BoolReturningAwaitable(30, false, false);
    sum += co_await VoidReturningAwaitable(40, false);
    expect_eq(sum, 100, "mixed awaitables sum");

    co_return td::Unit();
  }

  // B. Recursion via ask hop and direct recursion
  Task<int> rec_fast(int n) {
    if (n == 0)
      co_return 0;
    int r = co_await rec_fast(n - 1);  // Tasks propagate errors by default now
    co_return r + 1;
  }
  Task<int> rec_slow(int n) {
    if (n == 0)
      co_return 0;
    int r =
        co_await ask(actor_id(this), &CoroSpec::rec_slow, n - 1);  // ask returns StartedTask which propagates errors
    co_return r + 1;
  }
  Task<td::Unit> recursion() {
    LOG(INFO) << "=== Recursion ===";
    for (int depth : {5, 10}) {
      int a = co_await rec_slow(depth);  // Tasks propagate errors by default
      expect_eq(a, depth, "recursion via ask");
      int b = co_await rec_fast(depth);  // Tasks propagate errors by default
      expect_eq(b, depth, "direct recursion");
    }
    co_return td::Unit();
  }

  // C. ask()/ask_immediate and unified Task/Promise targets
  Task<td::Unit> asks() {
    LOG(INFO) << "=== ask / ask_immediate ===";

    auto delayed = ask(db_, &TestDatabase::square, 4);
    expect_true(!delayed.await_ready(), "delayed ask is not ready");
    expect_eq(co_await std::move(delayed), static_cast<size_t>(16), "delayed ask result");

    co_await Yield{};
    for (int i = 0; i < 16; i++) {
      auto immediate = ask_immediate(db_, &TestDatabase::square, 4);
      expect_true(immediate.await_ready(), "immediate ask is ready");
      expect_eq(immediate.await_resume().ok(), static_cast<size_t>(16), "immediate ask result");
    }

    auto user = co_await ask(db_, &TestDatabase::get, std::string("user"));
    LOG(INFO) << "User: " << user;

    (void)co_await ask(logger_, &TestLogger::log, std::string("unified Task target"));
    (void)co_await ask(logger_, &TestLogger::log_promise, std::string("unified Promise target"));
    co_return td::Unit();
  }

  // C2. Modifiers: Yield, ChangeOwner attach/detach, yield_on
  Task<td::Unit> modifiers() {
    LOG(INFO) << "=== Modifiers (Yield, ChangeOwner, yield_on) ===";

    auto self = actor_id(this);
    co_await attach_to_actor(self);  // just in case
    auto on_self = [self] {
      if (self != td::actor::detail::get_current_actor_id()) {
        return td::Status::Error("not on self");
      }
      return td::Status::OK();
    };
    auto on_none = [] {
      if (!td::actor::detail::get_current_actor_id().empty()) {
        return td::Status::Error("not on none");
      }
      return td::Status::OK();
    };

    // Yield sequencing
    {
      td::Timer timer;
      for (int i = 0; i < 1000; i++) {
        co_await yield_on_current();
        on_self();
      }
      LOG(INFO) << "yield_on_current (x100): " << timer.elapsed();
      timer = {};
      for (int i = 0; i < 1000; i++) {
        co_await attach_to_actor(self);  // noop
        on_self();
      }
      LOG(INFO) << "attach_to_actor (x100) : " << timer.elapsed();
    }

    // Attach to current actor and ensure suspended await resumes on same actor
    {
      co_await attach_to_actor(self);
      int v = co_await BoolReturningAwaitable(123, false, true);
      on_self();
      expect_eq(v, 123, "suspended await result");
    }

    // Detach (no specific owner), ensure we still resume and continue
    {
      co_await detach_from_actor();
      on_none();
      int v = co_await BoolReturningAwaitable(321, false, true);
      expect_eq(v, 321, "detached suspended await result");
      co_await attach_to_actor(self);
      on_self();
    }

    // Explicit yield_on to current actor
    {
      co_await yield_on_current();
      on_self();
    }

    co_return td::Unit();
  }

  // D. Concurrency and double-resumption surface
  Task<td::Unit> concurrency() {
    LOG(INFO) << "=== Concurrency ===";
    auto self = actor_id(this);
    co_await detach_from_actor();

    for (int round = 0; round < 100; round++) {
      co_await attach_to_actor(self);
      co_await detach_from_actor();
      auto task = [](int value) -> Task<int> {
        td::usleep_for(td::Random::fast(0, 1000));
        co_return value * 2;
      }(round)
                                       .start();
      td::usleep_for(td::Random::fast(0, 1000));
      auto result = co_await std::move(task);
      CHECK(result == round * 2);
    }

    for (int round = 0; round < 100; round++) {
      co_await attach_to_actor(self);
      co_await detach_from_actor();
      auto task = [](int value) -> Task<int> {
        td::usleep_for(td::Random::fast(0, 1000));
        co_return value * 2;
      }(round)
                                       .start();
      td::usleep_for(td::Random::fast(0, 1000));
      task.detach();
      td::usleep_for(100);
    }

    // Many parallel tasks + sum
    std::vector<StartedTask<size_t>> many;
    size_t expect = 0;
    for (size_t i = 0; i < 200; i++) {
      auto t = [](size_t v) -> Task<size_t> { co_return v; }(i).start();
      many.push_back(std::move(t));
      expect += i;
    }
    size_t got = 0;
    for (auto& t : many) {
      auto v = co_await std::move(t);  // Tasks propagate errors by default
      got += v;
    }
    expect_eq(got, expect, "many parallel sum");
    co_return td::Unit{};
  }
  Task<td::Unit> concurrency2() {
    LOG(INFO) << "=== Concurrency 2 ===";
    // A few shapes with spawn_coroutine_old
    std::vector<StartedTask<int>> shapes;
    for (int i = 0; i < 8000; i++) {
      int m = i % 4;
      if (m == 0) {
        //shapes.push_back(spawn_actor("immediate", []() -> Task<int> { co_return 1; }));
      } else if (m == 1) {
        shapes.push_back(spawn_actor("hop1", []() -> Task<int> {
          co_await spawn_actor("sub", []() -> Task<td::Unit> { co_return td::Unit(); }());
          co_return 2;
        }()));
      } else if (m == 2) {
        // intentionally left out heavy nested spawns variant
      } else {
        //shapes.push_back([]() -> Task<int> { co_return 2; }().start_immediate());
      }
    }
    int s = 0;
    for (auto& t : shapes) {
      auto v = co_await std::move(t);  // Tasks propagate errors by default
      s += v;
    }
    LOG(INFO) << "shapes sum: " << s;
    co_return td::Unit();
  }

  // F. Task lifecycle sanity (lazy start + await; explicit start)
  Task<td::Unit> lifecycle() {
    LOG(INFO) << "=== Task lifecycle ===";
    auto make_task = []() -> Task<int> { co_return 7; };

    // Await without explicit start
    {
      auto v = co_await make_task();  // Tasks propagate errors by default
      expect_eq(v, 7, "await without start");
    }
    // Explicit start
    {
      auto t = make_task().start();
      auto v = co_await std::move(t);  // Tasks propagate errors by default
      expect_eq(v, 7, "await after start");
    }
    co_return td::Unit();
  }
  Task<td::Unit> helpers() {
    LOG(INFO) << "=== Task helper ===";
    CHECK(5 == co_await td::actor::detail::make_awaitable(5));
    auto get7 = []() -> Task<int> { co_return 7; };
    CHECK(7 == co_await get7());
    auto square = [](size_t x) -> Task<size_t> { co_return x* x; };
    auto res = co_await get7().start().then(square);
    CHECK(res == 49);
    co_return td::Unit();
    co_return td::Unit();
  }

  Task<td::Unit> combinators() {
    LOG(ERROR) << "Test combinators";

    // Test all() with variadic arguments
    {
      auto make_task = [](int val, int delay_ms) -> Task<int> {
        small_sleep_ms(delay_ms);
        co_return val;
      };

      auto tuple = co_await all(make_task(1, 10), make_task(2, 20), make_task(3, 30));
      auto a = std::move(std::get<0>(tuple));
      auto b = std::move(std::get<1>(tuple));
      auto c = std::move(std::get<2>(tuple));
      expect_eq(1, a, "all() first result");
      expect_eq(2, b, "all() second result");
      expect_eq(3, c, "all() third result");
      LOG(ERROR) << "all() variadic test passed";
    }

    // Test all() with vector
    {
      std::vector<Task<int>> tasks;
      for (int i = 0; i < 5; ++i) {
        tasks.push_back([](int val) -> Task<int> {
          small_sleep_ms(val * 10);
          co_return val * 2;
        }(i));
      }

      auto results = co_await all(std::move(tasks));
      expect_eq(5u, results.size(), "all() vector size");
      for (size_t i = 0; i < results.size(); ++i) {
        expect_eq(static_cast<int>(i * 2), results[i], "all() vector result");
      }
      LOG(ERROR) << "all() vector test passed";
    }

    // Test all() with errors and collect_results
    {
      auto success_task = []() -> Task<int> { co_return 42; };
      auto error_task = []() -> Task<int> { co_return td::Status::Error("Test error"); };

      auto tuple = co_await all(success_task().wrap(), error_task().wrap());

      // Test that individual results can have errors
      auto s = std::move(std::get<0>(tuple));
      auto e = std::move(std::get<1>(tuple));
      expect_eq(42, s.ok(), "all() with error - success task");
      expect_true(e.is_error(), "all() with error - error task");

      // Test collect_results with tuple containing an error
      auto tuple2 = co_await all(success_task().wrap(), error_task().wrap());
      auto collected = collect(std::move(tuple2));
      expect_true(collected.is_error(), "collect_results should return error if any task failed");
      LOG(ERROR) << "all() error handling test passed";
    }

    // Test collect_results with all successful tasks
    {
      auto task1 = []() -> Task<int> { co_return 1; };
      auto task2 = []() -> Task<int> { co_return 2; };
      auto task3 = []() -> Task<int> { co_return 3; };

      // Test with tuple
      auto tuple = co_await all(task1().wrap(), task2().wrap(), task3().wrap());
      auto collected_tuple = collect(std::move(tuple));
      expect_ok(collected_tuple, "collect_results should succeed when all tasks succeed");
      auto [a, b, c] = collected_tuple.move_as_ok();
      expect_eq(1, a, "First value");
      expect_eq(2, b, "Second value");
      expect_eq(3, c, "Third value");

      // Test with vector
      std::vector<Task<int>> tasks;
      for (int i = 0; i < 5; ++i) {
        tasks.push_back([](int val) -> Task<int> { co_return val; }(i));
      }
      auto vec = co_await all_wrap(std::move(tasks));
      auto collected_vec = collect(std::move(vec));
      expect_ok(collected_vec, "collect_results should succeed for vector");
      auto& values = collected_vec.ok();
      expect_eq(5u, values.size(), "Vector size");
      for (size_t i = 0; i < values.size(); ++i) {
        expect_eq(static_cast<int>(i), values[i], "Vector element");
      }
      LOG(ERROR) << "collect_results test passed";
    }

    co_return td::Unit{};
  }

  // G. co_try success and error propagation
  Task<td::Unit> try_awaitable() {
    LOG(INFO) << "=== co_try ===";

    // Success path: unwrap value
    {
      auto ok_task = []() -> Task<int> { co_return 123; };
      int v = co_await ok_task();
      expect_eq(v, 123, "co_try unwraps ok value");
    }

    // Error path: propagate out of outer Task, so awaiting yields error
    {
      auto err_task = []() -> Task<int> { co_return td::Status::Error("boom"); };
      auto r = co_await err_task().wrap();  // control: direct await to observe error pattern
      expect_true(r.is_error(), "sanity: err_task returns error");

      // Now check propagation via co_try inside an outer task
      auto outer = [err_task]() -> Task<int> {
        int x = co_await err_task();
        co_return x + 1;  // should never reach
      }();
      auto rr = co_await std::move(outer).wrap();
      expect_true(rr.is_error(), "co_try propagates error to outer Task");
    }

    // Test try_unwrap() method on StartedTask
    {
      auto ok_task = []() -> Task<int> { co_return 456; };
      auto started = ok_task().start_immediate();
      int v = co_await std::move(started);
      expect_eq(v, 456, "try_unwrap() unwraps ok value from StartedTask");
    }

    // Test try_unwrap() error propagation
    {
      auto err_task = []() -> Task<int> { co_return td::Status::Error("test error"); };
      auto outer = [err_task]() -> Task<int> {
        auto started = err_task().start_immediate();
        int x = co_await std::move(started);
        co_return x + 1;  // should never reach
      }();
      auto result = co_await std::move(outer).wrap();
      expect_true(result.is_error(), "try_unwrap() propagates error from StartedTask");
    }

    // Test co_try() with Result<T> values (non-awaitable)
    {
      auto outer = []() -> Task<int> {
        td::Result<int> ok_result = 789;
        int x = co_await std::move(ok_result);
        co_return x + 1;
      }();
      auto result = co_await std::move(outer).wrap();  // Use wrap() to get Result<T>
      expect_true(result.is_ok(), "co_try(Result) works with ok value");
      expect_eq(result.move_as_ok(), 790, "co_try(Result) returns correct value");
    }

    // Test co_try() error propagation with Result<T>
    {
      auto outer = []() -> Task<int> {
        td::Result<int> err_result = td::Status::Error("direct error");
        int x = co_await std::move(err_result);
        co_return x + 1;  // should never reach
      }();
      auto result = co_await std::move(outer).wrap();  // Use wrap() to get Result<T>
      expect_true(result.is_error(), "co_try(Result) propagates error");
    }

    // Test co_try() with Result<T> lvalue reference
    {
      auto outer = []() -> Task<int> {
        td::Result<int> ok_result = 999;
        // Test with lvalue reference (should work with the Storage type handling)
        int x = co_await std::move(ok_result);
        // Result should be moved out after co_try
        co_return x + 2;
      }();
      auto result = co_await std::move(outer).wrap();  // Use wrap() to get Result<T>
      expect_true(result.is_ok(), "co_try(Result&) works with lvalue reference");
      expect_eq(result.move_as_ok(), 1001, "co_try(Result&) returns correct value");
    }

    // Test default Result co_await (propagates errors)
    {
      auto outer = []() -> Task<int> {
        td::Result<int> ok_result = 333;
        int x = co_await std::move(ok_result);  // Default: propagates error
        co_return x * 2;
      }();
      auto result = co_await std::move(outer).wrap();  // Use wrap() to get Result<T>
      expect_true(result.is_ok(), "Result default co_await works with ok value");
      expect_eq(result.move_as_ok(), 666, "Result default co_await returns correct value");
    }

    // Test default Result co_await error propagation
    {
      auto outer = []() -> Task<int> {
        td::Result<int> err_result = td::Status::Error("unwrap error");
        int x = co_await std::move(err_result);  // Default: propagates error
        co_return x * 2;                         // should never reach
      }();
      auto result = co_await std::move(outer).wrap();  // Use wrap() to get Result<T>
      expect_true(result.is_error(), "Result default co_await propagates error");
    }

    // Test Result::wrap() to prevent error propagation
    {
      auto outer = []() -> Task<td::Result<int>> {
        td::Result<int> err_result = td::Status::Error("wrapped error");
        auto full_result = co_await std::move(err_result).wrap();  // Explicit: no propagation
        expect_true(full_result.is_error(), "wrap() preserves error in Result");
        co_return full_result;
      }();
      auto result = co_await std::move(outer);
      expect_true(result.is_error(), "wrap() preserved the error");
    }

    // Test Result::wrap() with ok value
    {
      auto outer = []() -> Task<td::Result<int>> {
        td::Result<int> ok_result = 555;
        auto full_result = co_await std::move(ok_result).wrap();  // Explicit: no propagation
        expect_true(full_result.is_ok(), "wrap() preserves ok value in Result");
        co_return full_result;
      }();
      auto result = co_await std::move(outer).wrap();  // Need to wrap outer task too
      expect_true(result.is_ok(), "Task completes successfully");
      auto inner_result = result.move_as_ok();
      expect_true(inner_result.is_ok(), "wrap() preserved the ok value");
      expect_eq(inner_result.move_as_ok(), 555, "wrap() preserved the correct value");
    }

    // Test Task default co_await (propagates errors)
    {
      auto inner = []() -> Task<int> { co_return 888; };
      auto outer = [&inner]() -> Task<int> {
        int x = co_await inner();  // Default: propagates errors, returns T
        co_return x + 1;
      }();
      auto result = co_await std::move(outer).wrap();  // Wrap to get Result<int>
      expect_true(result.is_ok(), "Task default co_await works");
      expect_eq(result.move_as_ok(), 889, "Task default co_await returns correct value");
    }

    // Test Task default co_await error propagation
    {
      auto inner = []() -> Task<int> { co_return td::Status::Error("task error"); };
      auto outer = [&inner]() -> Task<int> {
        int x = co_await inner();  // Default: propagates error
        co_return x + 1;           // should never reach
      }();
      auto result = co_await std::move(outer).wrap();  // Wrap to get Result<int>
      expect_true(result.is_error(), "Task default co_await propagates error");
    }

    // Test Task::wrap() to prevent error propagation
    {
      auto inner = []() -> Task<int> { co_return td::Status::Error("wrapped task error"); };
      auto outer = [&inner]() -> Task<td::Result<int>> {
        auto full_result = co_await inner().wrap();  // Explicit: no propagation
        expect_true(full_result.is_error(), "Task::wrap() preserves error");
        co_return full_result;
      }();
      auto result = co_await std::move(outer).wrap();  // Wrap outer too
      expect_true(result.is_ok(), "Outer task completes successfully");
      auto inner_result = result.move_as_ok();
      expect_true(inner_result.is_error(), "Task::wrap() preserved the error");
    }

    co_return td::Unit{};
  }

  static Task<td::Unit> slow_task() {
    td::usleep_for(2000000);
    co_return td::Unit{};
  }
  Task<td::Unit> stop_actor() {
    class StopActor : public td::actor::Actor {
     public:
      void start_up() override {
        alarm_timestamp() = td::Timestamp::in(1);
      }
      void alarm() override {
        LOG(INFO) << "alarm";
        stop();
      }
      Task<int> query() {
        auto task = slow_task();
        task.set_executor(Executor::on_scheduler());
        // actor will stop before task is finished
        co_await std::move(task);
        // here we could access actor but we should v
        LOG(FATAL) << "access stopped actor";
        co_return 1;
      }
    };
    auto a = create_actor<StopActor>("stop_actor");
    auto r = co_await ask(a, &StopActor::query).wrap();
    r.ensure_error();
    LOG(INFO) << "Got error from stopped actor " << r.error();
    co_return td::Unit{};
  }

  // Master runner
  Task<td::Unit> run_all() {
    LOG(ERROR) << "Run tests";
    (void)co_await ask(logger_, &TestLogger::log, std::string("Starting coroutine tests"));

    co_await unified_queries();
    co_await concurrency();
    for (int i = 0; i < 10; i++) {
      co_await concurrency2();
    }
    co_await awaitable_branches();
    co_await recursion();
    co_await asks();
    co_await modifiers();
    co_await lifecycle();
    co_await helpers();
    co_await combinators();
    co_await try_awaitable();
    co_await stop_actor();

    (void)co_await ask(logger_, &TestLogger::log, std::string("All tests passed"));
    co_return td::Unit();
  }

 private:
  td::actor::ActorId<TestDatabase> db_;
  td::actor::ActorId<TestLogger> logger_;
};

// 5) Runner
int main() {
  SET_VERBOSITY_LEVEL(VERBOSITY_NAME(INFO));
  td::actor::Scheduler scheduler({4});
  scheduler.run_in_context([&] { td::actor::create_actor<CoroSpec>("CoroSpec").release(); });
  scheduler.run();
  return 0;
}
