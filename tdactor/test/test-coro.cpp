#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
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

template <class Pred>
Task<bool> wait_until(Pred&& pred, int max_iters = 50) {
  for (int i = 0; i < max_iters; i++) {
    if (pred()) {
      co_return true;
    }
    co_await yield_on_current();
  }
  co_return pred();
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
  explicit CoroSpec(bool run_external_parent_repro_only = false)
      : run_external_parent_repro_only_(run_external_parent_repro_only) {
  }

  void start_up() override {
    logger_ = td::actor::create_actor<TestLogger>("TestLogger").release();
    db_ = td::actor::create_actor<TestDatabase>("TestDatabase", logger_).release();
    if (run_external_parent_repro_only_) {
      [](Task<td::Unit> test) -> Task<td::Unit> {
        (co_await std::move(test).wrap()).ensure();
        co_await yield_on_current();
        td::actor::SchedulerContext::get().stop();
        co_return td::Unit{};
      }(external_parent_scope_repro_localize())
                                     .start_immediate_without_scope()
                                     .detach("CoroSpecRepro");
      return;
    }
    [](Task<td::Unit> test) -> Task<td::Unit> {
      (co_await std::move(test).wrap()).ensure();
      co_await yield_on_current();
      td::actor::SchedulerContext::get().stop();
      co_return td::Unit{};
    }(run_all())
                                   .start_immediate_without_scope()
                                   .detach("CoroSpec");
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
                                       .start_in_parent_scope();
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
                                       .start_in_parent_scope();
      td::usleep_for(td::Random::fast(0, 1000));
      task.detach_silent();
      td::usleep_for(100);
    }

    // Many parallel tasks + sum
    std::vector<StartedTask<size_t>> many;
    size_t expect = 0;
    for (size_t i = 0; i < 200; i++) {
      auto t = [](size_t v) -> Task<size_t> { co_return v; }(i).start_in_parent_scope();
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
        // shapes.push_back(spawn_actor("immediate", []() -> Task<int> { co_return 1; }));
      } else if (m == 1) {
        shapes.push_back(spawn_actor("hop1", []() -> Task<int> {
          co_await spawn_actor("sub", []() -> Task<td::Unit> { co_return td::Unit(); }());
          co_return 2;
        }()));
      } else if (m == 2) {
        // intentionally left out heavy nested spawns variant
      } else {
        // shapes.push_back([]() -> Task<int> { co_return 2; }().start_immediate());
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
      auto t = make_task().start_in_parent_scope();
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

    auto square_async = [](size_t x) -> Task<size_t> { co_return x* x; };
    auto res_async = co_await get7().start_in_parent_scope().then(square_async);
    CHECK(res_async == 49);

    auto square_sync = [](size_t x) -> size_t { return x * x; };
    auto res_sync = co_await get7().start_in_parent_scope().then(square_sync);
    CHECK(res_sync == 49);

    auto square_error = [](size_t x) -> td::Result<size_t> { return td::Status::Error("I forgor arithmetic!"); };
    auto res_error = co_await get7().start_in_parent_scope().then(square_error).wrap();
    CHECK(res_error.is_error());

    auto get_error = []() -> Task<> { co_return td::Status::Error("no"); };
    auto transform = [](td::Unit) -> td::Unit { return {}; };
    auto res_error_2 = co_await get_error().start_in_parent_scope().then(transform).wrap();
    CHECK(res_error_2.is_error());

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
      auto started = ok_task().start_immediate_without_scope();
      int v = co_await std::move(started);
      expect_eq(v, 456, "try_unwrap() unwraps ok value from StartedTask");
    }

    // Test try_unwrap() error propagation
    {
      auto err_task = []() -> Task<int> { co_return td::Status::Error("test error"); };
      auto outer = [err_task]() -> Task<int> {
        auto started = err_task().start_immediate_without_scope();
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
      auto outer = [inner]() -> Task<int> {
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
      auto outer = [inner]() -> Task<int> {
        int x = co_await inner();  // Default: propagates error
        co_return x + 1;           // should never reach
      }();
      auto result = co_await std::move(outer).wrap();  // Wrap to get Result<int>
      expect_true(result.is_error(), "Task default co_await propagates error");
    }

    // Test Task::wrap() to prevent error propagation
    {
      auto inner = []() -> Task<int> { co_return td::Status::Error("wrapped task error"); };
      auto outer = [inner]() -> Task<td::Result<int>> {
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

  Task<td::Unit> test_trace() {
    LOG(INFO) << "=== test_trace ===";

    // Test trace with error from Task
    {
      auto result = co_await []() -> Task<int> {
        auto err_task = []() -> Task<int> { co_return td::Status::Error("original error"); };
        co_return co_await err_task().trace("context");
      }()
                                         .wrap();
      expect_true(result.is_error(), "trace propagates error");
      auto msg = result.error().message().str();
      expect_true(msg.find("context") != std::string::npos, "trace adds context to error");
      expect_true(msg.find("original error") != std::string::npos, "trace preserves original message");
      LOG(INFO) << "Error with trace: " << result.error();
    }

    // Test trace with success from Task
    {
      auto result = co_await []() -> Task<int> {
        auto ok_task = []() -> Task<int> { co_return 42; };
        co_return co_await ok_task().trace("context");
      }()
                                         .wrap();
      expect_true(result.is_ok(), "trace passes through success");
      expect_eq(result.ok(), 42, "trace preserves value");
    }

    // Test trace with StartedTask
    {
      auto result = co_await []() -> Task<int> {
        auto err_task = []() -> Task<int> { co_return td::Status::Error("started error"); };
        co_return co_await err_task().start_in_parent_scope().trace("started context");
      }()
                                         .wrap();
      expect_true(result.is_error(), "trace works with StartedTask");
      auto msg = result.error().message().str();
      expect_true(msg.find("started context") != std::string::npos, "trace adds context to StartedTask error");
    }

    // Test trace with ask()
    {
      class ErrActor : public td::actor::Actor {
       public:
        Task<int> get_error() {
          co_return td::Status::Error("actor error");
        }
      };
      auto actor = create_actor<ErrActor>("ErrActor");
      auto result = co_await [](td::actor::ActorOwn<ErrActor> actor) -> Task<int> {
        co_return co_await ask(actor, &ErrActor::get_error).trace("ask context");
      }(std::move(actor))
                                                                            .wrap();
      expect_true(result.is_error(), "trace works with ask()");
      auto msg = result.error().message().str();
      expect_true(msg.find("ask context") != std::string::npos, "trace adds context to ask() error");
      LOG(INFO) << "Error with ask().trace(): " << result.error();
    }

    // Test Status::trace() directly
    {
      td::Status err = td::Status::Error("status error");
      td::Status traced = err.trace("status context");
      expect_true(traced.is_error(), "Status::trace() preserves error");
      auto msg = traced.message().str();
      expect_true(msg.find("status context") != std::string::npos, "Status::trace() adds context");
      expect_true(msg.find("status error") != std::string::npos, "Status::trace() preserves message");

      td::Status ok = td::Status::OK();
      td::Status traced_ok = ok.trace("ok context");
      expect_true(traced_ok.is_ok(), "Status::trace() preserves OK");
    }

    // Test Result<T>::trace() directly
    {
      td::Result<int> err = td::Status::Error("result error");
      td::Result<int> traced = err.trace("result context");
      expect_true(traced.is_error(), "Result::trace() preserves error");
      auto msg = traced.error().message().str();
      expect_true(msg.find("result context") != std::string::npos, "Result::trace() adds context");
      expect_true(msg.find("result error") != std::string::npos, "Result::trace() preserves message");

      td::Result<int> ok = 123;
      td::Result<int> traced_ok = ok.trace("ok context");
      expect_true(traced_ok.is_ok(), "Result::trace() preserves OK");
      expect_eq(traced_ok.ok(), 123, "Result::trace() preserves value");
    }

    // Test fast-path cancellation on .trace() with Task
    {
      constexpr int kCancelledCode = td::actor::kCancelledCode;
      auto outer = []() -> Task<td::Unit> {
        auto scope = co_await this_scope();
        auto ready_task = []() -> Task<int> { co_return 42; };
        auto child = ready_task().start_immediate_in_parent_scope();
        co_await yield_on_current();
        scope.cancel();
        // Even though child is ready, cancellation should win on traced await
        auto v = co_await std::move(child).trace("should not matter");
        (void)v;
        co_return td::Status::Error("should not reach here");
      };
      auto t = outer().start_in_parent_scope();
      auto r = co_await std::move(t).wrap();
      expect_true(r.is_error(), "trace Task cancellation: expected error");
      expect_eq(r.error().code(), kCancelledCode, "trace Task cancellation: expected cancelled code");
      LOG(INFO) << "fast-path cancellation on .trace() with Task: PASSED";
    }

    // Test fast-path cancellation on .trace() with ready Task (inline)
    {
      constexpr int kCancelledCode = td::actor::kCancelledCode;
      auto outer = []() -> Task<td::Unit> {
        auto scope = co_await this_scope();
        scope.cancel();
        auto ready_task = []() -> Task<int> { co_return 42; };
        // Task started and awaited inline via trace — cancellation should be caught
        auto v = co_await ready_task().trace("should not matter");
        (void)v;
        co_return td::Status::Error("should not reach here");
      };
      auto t = outer().start_in_parent_scope();
      auto r = co_await std::move(t).wrap();
      expect_true(r.is_error(), "trace inline Task cancellation: expected error");
      expect_eq(r.error().code(), kCancelledCode, "trace inline Task cancellation: expected cancelled code");
      LOG(INFO) << "fast-path cancellation on .trace() inline Task: PASSED";
    }

    LOG(INFO) << "test_trace passed";
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

  // Test 1: Promise stored in MAILBOX when actor stops
  // BUG: mailbox.clear() runs AFTER destroy_actor(), context still points to freed actor
  // FIX: Call actor_execute_context_.clear_actor() after destroy_actor()
  Task<td::Unit> promise_destroy_in_mailbox() {
    LOG(INFO) << "=== promise_destroy_in_mailbox ===";

    class Target : public td::actor::Actor {
     public:
      explicit Target(StartedTask<int>::ExternalPromise p) : promise_(std::move(p)) {
      }

      void start_up() override {
        // Queue the promise to ourselves - it goes to the mailbox
        send_closure_later(actor_id(this), &Target::receive_promise, std::move(promise_));
        stop();  // Promise in mailbox destroyed during mailbox.clear() AFTER destroy_actor()
      }

      void receive_promise(StartedTask<int>::ExternalPromise) {
        LOG(FATAL) << "Should not reach";
      }

     private:
      StartedTask<int>::ExternalPromise promise_;
    };

    auto [task, promise] = StartedTask<int>::make_bridge();
    create_actor<Target>("Target", std::move(promise)).release();

    auto result = co_await std::move(task).wrap();
    expect_true(result.is_error(), "Task should fail");
    LOG(INFO) << "Got expected error: " << result.error();
    co_return td::Unit{};
  }

  // Test 2: Promise stored in ACTOR MEMBER when actor stops
  // Promise is destroyed during Actor destructor, context might still point to actor
  Task<td::Unit> promise_destroy_in_actor_member() {
    LOG(INFO) << "=== promise_destroy_in_actor_member ===";

    class Target : public td::actor::Actor {
     public:
      explicit Target(StartedTask<int>::ExternalPromise p) : promise_(std::move(p)) {
      }

      void start_up() override {
        // Keep the promise in the actor member, don't move it anywhere
        stop();  // Promise destroyed during Actor destructor
      }

     private:
      StartedTask<int>::ExternalPromise promise_;  // Destroyed BEFORE actor_info_ptr_ (base class)
    };

    auto [task, promise] = StartedTask<int>::make_bridge();
    create_actor<Target>("Target", std::move(promise)).release();

    auto result = co_await std::move(task).wrap();
    expect_true(result.is_error(), "Task should fail");
    LOG(INFO) << "Got expected error: " << result.error();
    co_return td::Unit{};
  }

  // Test ActorRef UAF: coroutine outlives actor, SCOPE_EXIT accesses freed memory
  // This test demonstrates the need for ActorRef to prevent UAF
  Task<td::Unit> actor_ref_uaf() {
    LOG(INFO) << "=== actor_ref_uaf ===";

    class UafActor : public td::actor::Actor {
     public:
      int member_value = 42;

      ~UafActor() {
        LOG(INFO) << "~UafActor: zeroing member_value (was " << member_value << ")";
        member_value = 0;
      }

      void start_up() override {
        // Schedule alarm to stop actor in 0.05 seconds
        alarm_timestamp() = td::Timestamp::in(0.05);
      }
      void alarm() override {
        LOG(INFO) << "UafActor stopping";
        stop();
      }

      Task<int> query_with_scope_exit() {
        // SCOPE_EXIT will access 'this' when coroutine ends
        // But actor might be destroyed by then!
        SCOPE_EXIT {
          LOG(INFO) << "SCOPE_EXIT: accessing member_value = " << this->member_value;
          // UAF check: if actor destroyed, member_value will be 0
          LOG_CHECK(this->member_value == 42) << "UAF detected in SCOPE_EXIT!";
        };

        LOG(INFO) << "Before sleep, member_value = " << member_value;
        CHECK(member_value == 42);

        // Sleep longer than alarm timeout - actor will be destroyed while sleeping
        auto task = []() -> Task<td::Unit> {
          td::usleep_for(200000);  // 200ms sleep
          co_return td::Unit{};
        }();
        task.set_executor(Executor::on_scheduler());
        co_await std::move(task);

        // We reach here after actor is destroyed
        LOG(INFO) << "After sleep, member_value = " << member_value;  // UAF!
        LOG_CHECK(member_value == 42) << "UAF detected after sleep!";
        co_return member_value;  // UAF!
      }
    };

    auto a = create_actor<UafActor>("UafActor");
    auto r = co_await ask(a, &UafActor::query_with_scope_exit).wrap();
    // With current implementation: UAF happens or we get an error
    // With ActorRef: Should get clean error without UAF
    if (r.is_error()) {
      LOG(INFO) << "Got expected error: " << r.error();
    } else {
      LOG(INFO) << "Unexpected success, value = " << r.ok();
    }
    co_return td::Unit{};
  }

  Task<td::Unit> actor_task_unwrap_bug() {
    LOG(INFO) << "=== actor_task_unwrap_bug ===";

    class B : public td::actor::Actor {
     public:
      td::actor::Task<> run() {
        co_await td::actor::coro_sleep(td::Timestamp::in(2.0));
        co_return td::Status::Error("err");
      }
    };

    class A : public td::actor::Actor {
     public:
      void start_up() override {
        b_ = td::actor::create_actor<B>("B");
        run().start_in_parent_scope().detach();
        alarm_timestamp() = td::Timestamp::in(1.0);
      }

      void alarm() override {
        b_.release();
        stop();
      }

      td::actor::Task<> run() {
        LOG(ERROR) << "Start";
        std::vector<td::actor::StartedTask<>> tasks;
        tasks.push_back(td::actor::ask(b_, &B::run));
        co_await td::actor::all(std::move(tasks));
        co_return {};
      }

      td::actor::ActorOwn<B> b_;
    };

    td::actor::create_actor<A>("A").release();
    co_await coro_sleep(td::Timestamp::in(3.0));  // Wait for the scenario to play out

    LOG(INFO) << "actor_task_unwrap_bug test completed";
    co_return td::Unit{};
  }

  // Test that co_return {}; works correctly for Task<Unit>
  // Bug: co_return {}; was equivalent to co_return td::Status::Error(-1);
  // because {} matched ExternalResult via aggregate initialization
  Task<td::Unit> co_return_empty_braces() {
    LOG(INFO) << "=== co_return_empty_braces ===";

    // Test co_return {}; in Task<Unit> - should succeed, not return error
    auto test_task = []() -> Task<td::Unit> {
      co_return {};  // This was buggy - was equivalent to co_return td::Status::Error(-1);
    };

    auto result = co_await test_task().wrap();
    expect_true(result.is_ok(), "co_return {}; should succeed for Task<Unit>");

    // Also verify co_return td::Unit{}; still works
    auto test_task2 = []() -> Task<td::Unit> { co_return td::Unit{}; };
    auto result2 = co_await test_task2().wrap();
    expect_true(result2.is_ok(), "co_return td::Unit{}; should succeed");

    // Test designated initializers with simple structs
    struct SimpleStruct {
      int a;
      int b;
    };
    auto test_designated = []() -> Task<SimpleStruct> { co_return {.a = 1, .b = 2}; };
    auto result3 = co_await test_designated().wrap();
    expect_true(result3.is_ok(), "co_return {.a=1, .b=2}; should succeed");
    expect_eq(result3.ok().a, 1, "designated init .a");
    expect_eq(result3.ok().b, 2, "designated init .b");

    // Test simple brace init for structs
    auto test_brace = []() -> Task<SimpleStruct> { co_return {10, 20}; };
    auto result4 = co_await test_brace().wrap();
    expect_true(result4.is_ok(), "co_return {10, 20}; should succeed");
    expect_eq(result4.ok().a, 10, "brace init .a");
    expect_eq(result4.ok().b, 20, "brace init .b");

    LOG(INFO) << "co_return_empty_braces test passed";
    co_return td::Unit{};
  }

  // Test sleep_for - lightweight timer using IoWorker heap
  Task<td::Unit> sleep_for_test() {
    LOG(INFO) << "=== sleep_for_test ===";

    auto start = td::Timestamp::now();

    // Sleep for 100ms
    co_await sleep_for(0.1);

    auto elapsed = td::Timestamp::now().at() - start.at();
    LOG(INFO) << "Slept for " << elapsed << " seconds (expected ~0.1)";

    // Should have slept at least 90ms (allow some margin)
    expect_true(elapsed >= 0.09, "sleep_for should wait at least 90ms");
    // Should not have slept too long (allow up to 200ms for slow CI)
    expect_true(elapsed < 0.2, "sleep_for should not wait too long");

    // Regression: immediate-ready path must not dereference an uninitialized timer registration.
    auto immediate_start = td::Timestamp::now();
    for (int i = 0; i < 1000; i++) {
      co_await sleep_until(td::Timestamp::at(0));
    }
    auto immediate_elapsed = td::Timestamp::now().at() - immediate_start.at();
    expect_true(immediate_elapsed < 0.2, "Immediate-ready sleep should complete quickly");

    LOG(INFO) << "sleep_for_test passed";
    co_return td::Unit{};
  }

  // Test CoroMutex: mutual exclusion with async operations
  Task<td::Unit> coro_mutex_test() {
    LOG(INFO) << "=== coro_mutex_test ===";

    class MutexActor : public td::actor::Actor {
     public:
      Task<td::Unit> critical_section() {
        co_await coro_sleep(td::Timestamp::in(0.001 * td::Random::fast(0, 100)));
        auto lock = co_await mutex_.lock();
        in_lock_cnt_++;
        CHECK(in_lock_cnt_ == 1);
        co_await coro_sleep(td::Timestamp::in(0.001 * td::Random::fast(0, 100)));
        CHECK(in_lock_cnt_ == 1);
        in_lock_cnt_--;
        co_return td::Unit{};
      }

     private:
      CoroMutex mutex_;
      int in_lock_cnt_{0};
    };

    auto actor = create_actor<MutexActor>("MutexActor");

    constexpr int num_tasks = 20;
    std::vector<StartedTask<td::Unit>> tasks;
    for (int i = 0; i < num_tasks; i++) {
      tasks.push_back(ask(actor, &MutexActor::critical_section));
    }

    for (auto& t : tasks) {
      co_await std::move(t);
    }

    LOG(INFO) << "coro_mutex_test passed";
    co_return td::Unit{};
  }

  // Test CoroCoalesce: coalesce concurrent requests
  // Multiple queries arrive, only one executes, all get the same result
  Task<td::Unit> coro_coalesce_test() {
    LOG(INFO) << "=== coro_coalesce_test ===";

    class CoalesceActor : public td::actor::Actor {
     public:
      Task<int> query(int x) {
        co_return co_await coalesce_.run(x, [this, x]() -> Task<int> {
          LOG(INFO) << "Query(" << x << "): computing...";
          computation_count_++;
          co_await coro_sleep(td::Timestamp::in(0.1));
          int result = x * 2;
          LOG(INFO) << "Query(" << x << "): computed " << result;
          co_return result;
        });
      }

      int get_computation_count() {
        return computation_count_;
      }

     private:
      CoroCoalesce<int, int> coalesce_;
      int computation_count_{0};
    };

    auto actor = create_actor<CoalesceActor>("CoalesceActor");

    // Send many concurrent queries with x=21
    constexpr int num_queries = 10;
    std::vector<StartedTask<int>> tasks;
    for (int i = 0; i < num_queries; i++) {
      tasks.push_back(ask(actor, &CoalesceActor::query, 21));
    }

    // Wait for all queries
    for (auto& t : tasks) {
      int result = co_await std::move(t);
      expect_eq(result, 42, "Result should be 21*2=42");
    }

    // Verify only one computation happened
    auto count = co_await ask_immediate(actor, &CoalesceActor::get_computation_count);
    LOG(INFO) << "Computation count: " << count;
    expect_eq(count, 1, "Should have computed only once");

    LOG(INFO) << "coro_coalesce_test passed";
    co_return td::Unit{};
  }

  // === Structured Concurrency Tests ===

  // Helper coroutine - child that increments counter after sleeping
  static Task<int> child_task(std::shared_ptr<std::atomic<int>> counter, double sleep_time, int return_value) {
    co_await coro_sleep(td::Timestamp::in(sleep_time));
    counter->fetch_add(1, std::memory_order_relaxed);
    co_return return_value;
  }

  // === Structured Concurrency Helper Functions ===
  // (Using functions with arguments instead of lambda captures for safety)

  static Task<bool> test_scope_validity() {
    auto scope = co_await this_scope();
    co_return bool(scope);
  }

  static Task<int> sleeping_child(std::shared_ptr<std::atomic<bool>> flag, double sleep_time) {
    co_await coro_sleep(td::Timestamp::in(sleep_time));
    flag->store(true, std::memory_order_release);
    co_return 1;
  }

  static Task<int> parent_with_one_child(std::shared_ptr<std::atomic<bool>> child_completed) {
    sleeping_child(child_completed, 0.05).start_in_parent_scope().detach_silent();
    co_return 42;
  }

  static Task<int> parent_with_two_children(std::shared_ptr<std::atomic<int>> child_count) {
    child_task(child_count, 0.02, 1).start_in_parent_scope().detach_silent();
    child_task(child_count, 0.03, 2).start_in_parent_scope().detach_silent();
    co_return 100;
  }

  static Task<int> tls_after_yield() {
    co_await yield_on_current();
    auto* current = detail::get_current_promise();
    co_return current != nullptr ? 1 : 0;
  }

  static Task<int> yielding_child() {
    co_await yield_on_current();
    co_return 42;
  }

  static Task<int> tls_safety_parent() {
    auto* before = detail::get_current_promise();
    auto child = yielding_child().start_immediate_without_scope();
    auto* after = detail::get_current_promise();
    if (before != after) {
      co_return -1;
    }
    auto child_result = co_await std::move(child).wrap();
    co_return child_result.is_ok() ? child_result.ok() : -2;
  }

  static Task<td::Unit> detached_setter(std::shared_ptr<std::atomic<bool>> flag) {
    flag->store(true, std::memory_order_release);
    co_return td::Unit{};
  }

  static Task<int> grandchild_task(std::shared_ptr<std::atomic<bool>> done_flag) {
    co_await coro_sleep(td::Timestamp::in(0.03));
    done_flag->store(true, std::memory_order_release);
    co_return 7;
  }

  static Task<int> middle_parent(std::shared_ptr<std::atomic<bool>> grandchild_done) {
    grandchild_task(grandchild_done).start_in_parent_scope().detach_silent();
    co_return 3;
  }

  static Task<int> grandparent_task(std::shared_ptr<std::atomic<bool>> grandchild_done) {
    middle_parent(grandchild_done).start_in_parent_scope().detach_silent();
    co_return 1;
  }

  static Task<int> stress_child(std::shared_ptr<std::atomic<int>> counter, int index) {
    co_await coro_sleep(td::Timestamp::in(0.01 + (index % 5) * 0.005));
    counter->fetch_add(1, std::memory_order_relaxed);
    co_return index;
  }

  static Task<int> stress_parent(std::shared_ptr<std::atomic<int>> counter, int num_children) {
    for (int i = 0; i < num_children; i++) {
      stress_child(counter, i).start_in_parent_scope().detach_silent();
    }
    co_return 999;
  }

  Task<td::Unit> structured_concurrency_test() {
    LOG(INFO) << "=== structured_concurrency_test ===";

    auto run_case = [&](const char* name, auto make_test) -> Task<td::Unit> {
      auto r = co_await make_test().wrap();
      r.ensure();
      LOG(INFO) << name << ": PASSED";
      co_return td::Unit{};
    };

    co_await run_case("this_scope() returns valid scope", []() -> Task<td::Unit> {
      expect_true(co_await test_scope_validity(), "this_scope() should return valid scope");
      co_return td::Unit{};
    });

    co_await run_case("parent waits for 1 child", []() -> Task<td::Unit> {
      auto child_completed = std::make_shared<std::atomic<bool>>(false);
      expect_eq(co_await parent_with_one_child(child_completed), 42, "Parent result should be correct");
      expect_true(child_completed->load(std::memory_order_acquire), "Parent should wait for child");
      co_return td::Unit{};
    });

    co_await run_case("parent waits for 2 children", []() -> Task<td::Unit> {
      auto child_count = std::make_shared<std::atomic<int>>(0);
      expect_eq(co_await parent_with_two_children(child_count), 100, "Parent result should be correct");
      expect_eq(child_count->load(), 2, "Both children should complete");
      co_return td::Unit{};
    });

    co_await run_case("TLS is set after scheduler resume", []() -> Task<td::Unit> {
      expect_eq(co_await tls_after_yield(), 1, "TLS should be set after scheduler resume");
      co_return td::Unit{};
    });

    co_await run_case("start_immediate restores caller TLS", []() -> Task<td::Unit> {
      expect_eq(co_await tls_safety_parent(), 42, "start_immediate should restore TLS and child should complete");
      co_return td::Unit{};
    });

    co_await run_case("start_detached completes and cleans up", []() -> Task<td::Unit> {
      auto completed = std::make_shared<std::atomic<bool>>(false);
      detached_setter(completed).start_in_parent_scope().detach_silent();
      // Give the scheduler a chance to run the detached task
      for (int i = 0; i < 5 && !completed->load(std::memory_order_acquire); i++) {
        co_await yield_on_current();
      }
      expect_true(completed->load(std::memory_order_acquire), "Detached task should complete");
      co_return td::Unit{};
    });

    co_await run_case("nested scopes (grandparent → parent → child) wait correctly", []() -> Task<td::Unit> {
      auto grandchild_done = std::make_shared<std::atomic<bool>>(false);
      expect_eq(co_await grandparent_task(grandchild_done), 1, "Grandparent result should be correct");
      expect_true(grandchild_done->load(std::memory_order_acquire),
                  "Grandchild should complete before grandparent returns");
      co_return td::Unit{};
    });

    co_await run_case("concurrent child completion stress", []() -> Task<td::Unit> {
      constexpr int NUM_CHILDREN = 20;
      auto completion_count = std::make_shared<std::atomic<int>>(0);
      expect_eq(co_await stress_parent(completion_count, NUM_CHILDREN), 999, "Parent result should be correct");
      expect_eq(completion_count->load(), NUM_CHILDREN, "All children should complete");
      co_return td::Unit{};
    });

    co_await run_case("TLS matches this_scope() on scheduler path", []() -> Task<td::Unit> {
      auto r = co_await []() -> Task<int> {
        co_await detach_from_actor();

        auto scope = co_await this_scope();
        auto* tls = detail::get_current_promise();
        if (!tls || tls != scope.get_promise()) {
          co_return 0;
        }

        co_await yield_on_current();
        auto* tls2 = detail::get_current_promise();
        co_return (tls2 && tls2 == scope.get_promise()) ? 1 : 0;
      }()
                                    .wrap();

      expect_ok(r, "Scheduler TLS test should not error");
      expect_eq(r.ok(), 1, "TLS should match current promise on scheduler resumes");
      co_return td::Unit{};
    });

    co_await run_case("ask() promise path preserves scope tracking", []() -> Task<td::Unit> {
      class PromiseScopeActor final : public td::actor::Actor {
       public:
        void run(std::shared_ptr<std::atomic<bool>> done, td::Promise<td::Unit> promise) {
          done_ = std::move(done);
          promise_ = std::move(promise);
          alarm_timestamp() = td::Timestamp::in(0.03);
        }
        void alarm() override {
          done_->store(true, std::memory_order_release);
          promise_.set_value(td::Unit{});
          stop();
        }

       private:
        std::shared_ptr<std::atomic<bool>> done_;
        td::Promise<td::Unit> promise_;
      };

      auto done = std::make_shared<std::atomic<bool>>(false);
      auto actor = td::actor::create_actor<PromiseScopeActor>("PromiseScopeActor").release();

      (co_await [done, actor]() -> Task<td::Unit> {
        auto scope = co_await this_scope();
        expect_true(detail::get_current_promise() == scope.get_promise(), "TLS should match scope before ask()");
        auto req = ask(actor, &PromiseScopeActor::run, done);
        req.detach_silent();
        co_return td::Unit{};
      }()
                                       .wrap())
          .ensure();

      expect_true(done->load(std::memory_order_acquire), "Parent should wait for ask()-connected work via scope");
      co_return td::Unit{};
    });

    co_await run_case("parent error waits for children before completing", []() -> Task<td::Unit> {
      auto child_completed = std::make_shared<std::atomic<bool>>(false);
      auto r = co_await [child_completed]() -> Task<td::Unit> {
        sleeping_child(child_completed, 0.03).start_in_parent_scope().detach_silent();
        co_return td::Status::Error(123, "parent error");
      }()
                                                   .wrap();

      expect_true(r.is_error(), "Parent should return error");
      expect_eq(r.error().code(), 123, "Parent error code should be preserved");
      expect_true(child_completed->load(std::memory_order_acquire), "Child should complete before parent finishes");
      co_return td::Unit{};
    });

    co_await run_case("ask() Task-return remote coroutine TLS + resume location", []() -> Task<td::Unit> {
      class AskCoroTlsActor final : public td::actor::Actor {
       public:
        Task<int> check_tls_and_yield() {
          auto scope = co_await this_scope();
          auto* p0 = detail::get_current_promise();
          if (!p0 || p0 != scope.get_promise()) {
            co_return 0;
          }
          co_await yield_on_current();
          auto* p1 = detail::get_current_promise();
          co_return (p1 && p1 == scope.get_promise()) ? 1 : 0;
        }
      };

      auto remote = td::actor::create_actor<AskCoroTlsActor>("AskCoroTlsActor").release();
      auto caller_before = td::actor::detail::get_current_actor_id();

      auto r = co_await ask(remote, &AskCoroTlsActor::check_tls_and_yield).wrap();
      expect_ok(r, "ask(remote Task) should not error");
      expect_eq(r.ok(), 1, "Remote coroutine TLS should match its promise across yield");

      auto caller_after = td::actor::detail::get_current_actor_id();
      expect_eq(caller_after, caller_before, "Awaiting ask(remote Task) should resume on caller actor");
      co_return td::Unit{};
    });

    LOG(INFO) << "structured_concurrency_test passed";
    co_return td::Unit{};
  }

  // =============================================================================
  // Cancellation tests
  // =============================================================================
  Task<td::Unit> cancellation_comprehensive_test() {
    LOG(INFO) << "=== cancellation_comprehensive_test ===";

    constexpr int kCancelledCode = td::actor::kCancelledCode;

    auto run_case = [&](const char* name, auto make_test) -> Task<td::Unit> {
      LOG(INFO) << "Running: " << name;
      auto start_time = td::Timestamp::now();
      auto r = co_await make_test().wrap();
      r.ensure();
      auto elapsed = td::Timestamp::now().at() - start_time.at();
      LOG_CHECK(elapsed < 1.0) << name << " took too long: " << elapsed << "s";
      LOG(INFO) << name << ": PASSED";
      co_return td::Unit{};
    };

    struct Gate {
      StartedTask<td::Unit> task;
      StartedTask<td::Unit>::ExternalPromise promise;
      void open() {
        promise.set_value(td::Unit{});
      }
    };
    auto make_gate = []() -> Gate {
      auto [t, p] = StartedTask<td::Unit>::make_bridge();
      return Gate{std::move(t), std::move(p)};
    };

    co_await run_case("cancelled task returns Error(653) on resume boundary", [&]() -> Task<td::Unit> {
      auto sleeper = []() -> Task<int> {
        co_await sleep_for(10.0);  // Will hang if cancellation doesn't work
        co_return 1;
      };
      auto t = sleeper().start_in_parent_scope();

      co_await sleep_for(0.01);
      t.cancel();

      auto r = co_await std::move(t).wrap();
      expect_true(r.is_error(), "Expected cancelled error");
      expect_eq(r.error().code(), kCancelledCode, "Expected Error(653) from cancellation");
      co_return td::Unit{};
    });

    co_await run_case("cancel works after immediate-ready sleep", [&]() -> Task<td::Unit> {
      auto worker = []() -> Task<td::Unit> {
        co_await sleep_until(td::Timestamp::at(0));
        co_await sleep_for(10.0);
        co_return td::Unit{};
      };

      auto t = worker().start_in_parent_scope();
      co_await yield_on_current();
      t.cancel();

      auto r = co_await std::move(t).wrap();
      expect_true(r.is_error(), "Expected cancellation error");
      expect_eq(r.error().code(), kCancelledCode, "Expected Error(653) from cancellation");
      co_return td::Unit{};
    });

    co_await run_case("cancel parent while awaiting 1 of N children (no UAF, waits for all)", [&]() -> Task<td::Unit> {
      auto g0 = make_gate();
      auto g1 = make_gate();
      auto g2 = make_gate();

      auto children_started = std::make_shared<std::atomic<bool>>(false);

      auto parent_body = [](std::shared_ptr<std::atomic<bool>> children_started, StartedTask<td::Unit> t0,
                            StartedTask<td::Unit> t1, StartedTask<td::Unit> t2) -> Task<td::Unit> {
        auto child_body = [](StartedTask<td::Unit> gate_task) -> Task<td::Unit> {
          co_await std::move(gate_task);
          co_return td::Unit{};
        };

        auto awaited_child = child_body(std::move(t0)).start_in_parent_scope();
        child_body(std::move(t1)).start_in_parent_scope().detach_silent();
        child_body(std::move(t2)).start_in_parent_scope().detach_silent();

        children_started->store(true, std::memory_order_release);

        // Await one child. If parent is cancelled, it will finish with Error(653) at the resume boundary.
        (void)co_await std::move(awaited_child).wrap();
        co_return td::Unit{};
      };

      auto parent = parent_body(children_started, std::move(g0.task), std::move(g1.task), std::move(g2.task))
                        .start_in_parent_scope();

      // Ensure parent started and registered children before we cancel it.
      bool started_ok = false;
      for (int i = 0; i < 100 && !started_ok; i++) {
        started_ok = children_started->load(std::memory_order_acquire);
        if (!started_ok) {
          co_await yield_on_current();
        }
      }
      expect_true(started_ok, "Parent should start and spawn children");

      parent.cancel();

      // Let the awaited child complete first; parent should *not* become ready yet (still has other children).
      g0.open();
      for (int i = 0; i < 5; i++) {
        co_await yield_on_current();
      }
      expect_true(!parent.await_ready(), "Parent should still be waiting for remaining children");

      g1.open();
      g2.open();

      auto r = co_await std::move(parent).wrap();
      expect_true(r.is_error(), "Expected parent cancellation error");
      expect_eq(r.error().code(), kCancelledCode, "Expected Error(653) on parent cancellation");
      co_return td::Unit{};
    });

    // Test is_active() and ensure_active() Kotlin-like API
    co_await run_case("is_active() returns true when not cancelled", [&]() -> Task<td::Unit> {
      bool active = co_await is_active();
      expect_true(active, "is_active() should return true when not cancelled");
      co_return td::Unit{};
    });

    co_await run_case("push-down cancellation sets child cancelled flag", [&]() -> Task<td::Unit> {
      auto child = []() -> Task<td::Unit> {
        co_await sleep_for(10.0);  // Will hang if cancellation doesn't work
        co_return td::Unit{};
      };

      auto t = child().start_in_parent_scope();

      co_await sleep_for(0.01);
      t.cancel();

      auto r = co_await std::move(t).wrap();
      expect_true(r.is_error(), "Child should see cancellation");
      co_return td::Unit{};
    });

    co_await run_case("ensure_active() passes when not cancelled", [&]() -> Task<td::Unit> {
      co_await ensure_active();  // Should not throw
      co_return td::Unit{};
    });

    co_await run_case("ensure_active() throws cancellation when cancelled", [&]() -> Task<td::Unit> {
      auto check_ensure = []() -> Task<td::Unit> {
        co_await sleep_for(10.0);  // Will hang if cancellation doesn't work
        co_await ensure_active();  // Should abort with Error(653)
        co_return td::Unit{};
      };

      auto t = check_ensure().start_in_parent_scope();

      co_await sleep_for(0.01);
      t.cancel();

      auto r = co_await std::move(t).wrap();
      expect_true(r.is_error(), "Expected cancellation error");
      expect_eq(r.error().code(), kCancelledCode, "Expected Error(653)");
      co_return td::Unit{};
    });

    // Test push-down cancellation to children
    co_await run_case("cancel propagates to nested children (push-down)", [&]() -> Task<td::Unit> {
      auto grandchild = []() -> Task<td::Unit> {
        co_await sleep_for(10.0);  // Will hang if cancellation doesn't work
        co_return td::Unit{};
      };

      auto child = [&]() -> Task<td::Unit> {
        co_await grandchild().start_in_parent_scope();
        co_return td::Unit{};
      };

      auto parent = [&]() -> Task<td::Unit> {
        co_await child().start_in_parent_scope();
        co_return td::Unit{};
      };

      auto t = parent().start_in_parent_scope();

      co_await sleep_for(0.01);
      t.cancel();

      auto r = co_await std::move(t).wrap();
      expect_true(r.is_error(), "Parent should be cancelled");
      co_return td::Unit{};
    });

    // Test timer cancellation when scope cancels
    co_await run_case("sleep_for wakes up when scope is cancelled", [&]() -> Task<td::Unit> {
      auto sleeper = []() -> Task<td::Unit> {
        co_await sleep_for(10.0);  // Will hang if cancellation doesn't work
        co_return td::Unit{};
      };

      auto t = sleeper().start_in_parent_scope();

      co_await sleep_for(0.01);
      t.cancel();

      auto r = co_await std::move(t).wrap();
      expect_true(r.is_error(), "Sleeper should be cancelled");
      expect_eq(r.error().code(), kCancelledCode, "Expected Error(653)");
      co_return td::Unit{};
    });

    // Repro: cancellation should not touch a completed awaiter
    co_await run_case("cancel does not call on_cancel after awaiter resume", [&]() -> Task<td::Unit> {
      struct LateCancelNode : HeapCancelNode {
        std::atomic<bool> active{true};
        std::atomic<bool> late_cancel{false};

        void do_cancel() override {
          if (!active.load(std::memory_order_acquire)) {
            late_cancel.store(true, std::memory_order_release);
          }
        }
        void do_cleanup() override {
          active.store(false, std::memory_order_release);
        }
      };

      struct LateCancelAwaitable {
        Ref<LateCancelNode> node;

        bool await_ready() const noexcept {
          return false;
        }
        void await_suspend(std::coroutine_handle<> h) noexcept {
          node = make_ref<LateCancelNode>();
          auto lease = current_scope_lease();
          if (lease) {
            lease.publish_heap_cancel_node(*node);
          }
          detail::SchedulerExecutor{}.schedule(h);
        }
        bool await_resume() noexcept {
          bool cancelled = !node->disarm();
          node.reset();
          return cancelled;
        }
      };

      auto node = make_ref<LateCancelNode>();
      auto test_ref = node.share();
      std::atomic<bool> awaiter_done{false};

      auto worker = [node = std::move(node), &awaiter_done]() mutable -> Task<td::Unit> {
        co_await LateCancelAwaitable{std::move(node)};
        awaiter_done.store(true, std::memory_order_release);
        co_await sleep_for(10.0);
        co_return td::Unit{};
      };

      auto t = worker().start_in_parent_scope();

      bool done = false;
      for (int i = 0; i < 100 && !done; i++) {
        done = awaiter_done.load(std::memory_order_acquire);
        if (!done) {
          co_await yield_on_current();
        }
      }
      expect_true(done, "Awaiter should complete before cancellation");

      t.cancel();
      auto r = co_await std::move(t).wrap();
      expect_true(r.is_error(), "Expected cancellation");

      expect_true(!test_ref->late_cancel.load(std::memory_order_acquire),
                  "on_cancel must not be called after awaiter has resumed");
      co_return td::Unit{};
    });

    co_await run_case("double publish does not leak or double-cleanup node", [&]() -> Task<td::Unit> {
      struct PublishStats {
        std::atomic<int> cancel_calls{0};
        std::atomic<int> cleanup_calls{0};
        std::atomic<int> destroy_calls{0};
      };

      struct PublishNode final : HeapCancelNode {
        std::shared_ptr<PublishStats> stats;
        explicit PublishNode(std::shared_ptr<PublishStats> s) : stats(std::move(s)) {
        }
        ~PublishNode() override {
          stats->destroy_calls.fetch_add(1, std::memory_order_relaxed);
        }
        void do_cancel() override {
          stats->cancel_calls.fetch_add(1, std::memory_order_relaxed);
        }
        void do_cleanup() override {
          stats->cleanup_calls.fetch_add(1, std::memory_order_relaxed);
        }
      };

      struct DoublePublishAwaitable {
        std::shared_ptr<PublishStats> stats;
        std::shared_ptr<std::atomic<bool>> awaiter_done;
        Ref<PublishNode> cancel_node;

        bool await_ready() const noexcept {
          return false;
        }
        void await_suspend(std::coroutine_handle<> h) noexcept {
          cancel_node = make_ref<PublishNode>(stats);
          auto lease = current_scope_lease();
          CHECK(lease);
          lease.publish_heap_cancel_node(*cancel_node);
          lease.publish_heap_cancel_node(*cancel_node);
          detail::SchedulerExecutor{}.schedule(h);
        }
        void await_resume() noexcept {
          cancel_node->disarm();
          cancel_node.reset();
          awaiter_done->store(true, std::memory_order_release);
        }
      };

      auto stats = std::make_shared<PublishStats>();
      auto awaiter_done = std::make_shared<std::atomic<bool>>(false);

      auto worker = [stats, awaiter_done]() -> Task<td::Unit> {
        co_await DoublePublishAwaitable{stats, awaiter_done, {}};
        co_await sleep_for(10.0);
        co_return td::Unit{};
      };

      // Deliberately parentless: test checks that node is destroyed synchronously
      // when the last StartedTask ref drops, before this coroutine continues.
      auto t = worker().start_without_scope();
      bool done = false;
      for (int i = 0; i < 100 && !done; i++) {
        done = awaiter_done->load(std::memory_order_acquire);
        if (!done) {
          co_await yield_on_current();
        }
      }
      expect_true(done, "Awaiter should complete before cancellation");

      t.cancel();
      auto r = co_await std::move(t).wrap();
      expect_true(r.is_error(), "Expected cancellation");
      expect_eq(r.error().code(), kCancelledCode, "Expected Error(653)");

      expect_eq(stats->cancel_calls.load(std::memory_order_acquire), 0,
                "Cancel callback must not run after awaiter disarm");
      expect_eq(stats->cleanup_calls.load(std::memory_order_acquire), 0,
                "Cleanup callback must not run after awaiter disarm");
      expect_eq(stats->destroy_calls.load(std::memory_order_acquire), 1, "Node must be destroyed exactly once");
      co_return td::Unit{};
    });

    // Test cancellation propagation via ask()
    co_await run_case("cancellation propagates through ask() to remote actor task", [&]() -> Task<td::Unit> {
      class SlowActor final : public td::actor::Actor {
       public:
        Task<int> slow_method(std::shared_ptr<std::atomic<bool>> started,
                              std::shared_ptr<std::atomic<bool>> saw_cancel) {
          started->store(true, std::memory_order_release);

          // Check cancellation periodically while "working"
          for (int i = 0; i < 100; i++) {
            if (!(co_await is_active())) {
              saw_cancel->store(true, std::memory_order_release);
              co_return -1;
            }
            co_await sleep_for(0.01);  // 10ms
          }
          co_return 42;
        }
      };

      auto actor = td::actor::create_actor<SlowActor>("SlowActor").release();
      auto started = std::make_shared<std::atomic<bool>>(false);
      auto saw_cancel = std::make_shared<std::atomic<bool>>(false);

      auto t = ask(actor, &SlowActor::slow_method, started, saw_cancel);

      // Wait for actor to start
      bool started_ok = co_await wait_until([&] { return started->load(std::memory_order_acquire); }, 5000);
      expect_true(started_ok, "Actor method should start");

      // Cancel the ask
      t.cancel();

      // Wait for result
      auto r = co_await std::move(t).wrap();

      // The task should be cancelled (Error 653)
      expect_true(r.is_error(), "ask() should return error when cancelled");
      expect_eq(r.error().code(), kCancelledCode, "Expected Error(653)");

      // The remote task should have seen the cancellation
      // (it may or may not have, depending on timing, so just log it)
      LOG(INFO) << "Remote task saw cancellation: " << (saw_cancel->load(std::memory_order_acquire) ? "yes" : "no");

      co_return td::Unit{};
    });

    // Test that cancellation propagates through ask() to the remote coroutine
    co_await run_case("ask() cancellation propagates to remote coroutine", [&]() -> Task<td::Unit> {
      class SleepActor final : public td::actor::Actor {
       public:
        Task<int> slow_method() {
          co_await sleep_for(10.0);  // Will hang if cancellation doesn't work
          co_return 42;
        }
      };

      auto actor = td::actor::create_actor<SleepActor>("SleepActor").release();
      auto t = ask(actor, &SleepActor::slow_method);

      co_await sleep_for(0.01);
      t.cancel();

      auto r = co_await std::move(t).wrap();
      expect_true(r.is_error(), "ask() should return error when cancelled");
      expect_eq(r.error().code(), kCancelledCode, "Expected Error(653)");
      co_return td::Unit{};
    });

    // Test that child cannot catch cancellation via .wrap() to prevent its own cancellation
    // When parent is cancelled, child is also cancelled, so even if child uses .wrap() on grandchild,
    // the child itself will still be cancelled at the resume boundary.
    co_await run_case("child cannot catch cancellation to prevent grandchild cancellation", [&]() -> Task<td::Unit> {
      auto child_caught_error = std::make_shared<std::atomic<bool>>(false);
      auto child_continued_after_wrap = std::make_shared<std::atomic<bool>>(false);

      auto grandchild = []() -> Task<int> {
        // Use a long sleep - will be cancelled
        co_await sleep_for(10.0);
        co_return 42;
      };

      auto child = [grandchild, child_caught_error, child_continued_after_wrap]() -> Task<int> {
        auto gc = grandchild().start_in_parent_scope();

        // Child tries to catch the grandchild's cancellation with .wrap()
        auto result = co_await std::move(gc).wrap();

        // This code should NOT run because child itself is cancelled at the resume boundary
        child_continued_after_wrap->store(true, std::memory_order_release);

        if (result.is_error()) {
          child_caught_error->store(true, std::memory_order_release);
          co_return -100;  // Child tries to "handle" the error
        }
        co_return result.ok();
      };

      auto parent = [child]() -> Task<int> { co_return co_await child().start_in_parent_scope(); };

      auto task = parent().start_in_parent_scope();

      // Let things start
      co_await sleep_for(0.02);

      // Cancel the parent
      task.cancel();

      auto result = co_await std::move(task).wrap();

      // Parent should be cancelled
      expect_true(result.is_error(), "Parent should be cancelled");
      expect_eq(result.error().code(), kCancelledCode, "Parent error should be 653");

      // Child should NOT have continued after .wrap() - it was cancelled at the resume boundary
      expect_true(!child_continued_after_wrap->load(std::memory_order_acquire),
                  "Child should NOT continue after .wrap() because it is also cancelled");
      expect_true(!child_caught_error->load(std::memory_order_acquire), "Child should NOT catch the error via .wrap()");

      co_return td::Unit{};
    });

    co_await run_case("wrap() on child returns cancellation error when parent cancelled", [&]() -> Task<td::Unit> {
      auto inner_cancelled = std::make_shared<std::atomic<bool>>(false);

      auto inner_task = [inner_cancelled]() -> Task<int> {
        co_await sleep_for(10.0);
        co_return 42;
      };

      auto child = [inner_task, inner_cancelled]() -> Task<td::Unit> {
        auto inner = inner_task().start_in_parent_scope();
        auto result = co_await std::move(inner).wrap();

        // If we get here, the wrap() result should be a cancellation error
        if (result.is_error() && result.error().code() == kCancelledCode) {
          inner_cancelled->store(true, std::memory_order_release);
        }
        co_return td::Unit{};
      };

      auto parent = [child]() -> Task<td::Unit> { co_return co_await child().start_in_parent_scope(); };

      auto task = parent().start_in_parent_scope();
      co_await sleep_for(0.02);
      task.cancel();
      auto r = co_await std::move(task).wrap();

      expect_true(r.is_error(), "Parent should be cancelled");
      expect_eq(r.error().code(), kCancelledCode, "Parent error should be 653");
      co_return td::Unit{};
    });

    co_await run_case("dropping StartedTask cancels the task", [&]() -> Task<td::Unit> {
      auto ran_to_completion = std::make_shared<std::atomic<bool>>(false);

      auto worker = [ran_to_completion]() -> Task<int> {
        co_await sleep_for(10.0);
        ran_to_completion->store(true, std::memory_order_release);
        co_return 42;
      };

      // Drop the StartedTask immediately - should cancel the task
      {
        auto dropped = worker().start_in_parent_scope();
      }

      co_await sleep_for(0.05);
      expect_true(!ran_to_completion->load(std::memory_order_acquire),
                  "Task should be cancelled, not run to completion");
      co_return td::Unit{};
    });

    co_await run_case("DFS cancel completes in topological order", [&]() -> Task<td::Unit> {
      constexpr int max_i = 300;
      auto mu = std::make_shared<std::mutex>();
      auto order = std::make_shared<std::vector<int>>();

      // DFS tree: node i has children 2i+1 (left, stored as local) and 2i+2 (right, awaited).
      // SCOPE_EXIT is declared before cl, so frame destruction order is:
      //   cl destroyed (dec_ref → left child frame destroyed recursively) → then SCOPE_EXIT runs.
      // Right child was already awaited and released. This guarantees topological order.
      auto dfs = [](auto& self, int i, int max_i, std::shared_ptr<std::mutex> mu,
                    std::shared_ptr<std::vector<int>> order) -> Task<td::Unit> {
        if (i > max_i) {
          co_return td::Unit{};
        }
        SCOPE_EXIT {
          td::usleep_for(td::Random::fast(0, 1000));
          std::lock_guard<std::mutex> lock(*mu);
          order->push_back(i);
        };
        int l = i * 2 + 1;
        int r = i * 2 + 2;
        auto cl = self(self, l, max_i, mu, order).start_in_parent_scope();
        auto cr = self(self, r, max_i, mu, order).start_in_parent_scope();
        co_await sleep_for(100);
        UNREACHABLE();
      };

      auto t = dfs(dfs, 0, max_i, mu, order).start_without_scope();
      co_await sleep_for(0.05);
      t.cancel();
      auto r = co_await std::move(t).wrap();
      expect_true(r.is_error(), "DFS root should be cancelled");
      expect_eq(r.error().code(), kCancelledCode, "Expected Error(653)");

      // All interior nodes (0..max_i) should have completed
      expect_eq(static_cast<int>(order->size()), max_i + 1, "All interior nodes should have completed");

      // Check topological order: every node's parent must appear AFTER it
      std::vector<int> position(max_i + 1, -1);
      for (int idx = 0; idx < static_cast<int>(order->size()); idx++) {
        position[(*order)[idx]] = idx;
      }
      for (int node = 1; node <= max_i; node++) {
        int parent = (node - 1) / 2;
        LOG_CHECK(position[node] < position[parent])
            << "Node " << node << " (pos " << position[node] << ") should complete before parent " << parent << " (pos "
            << position[parent] << ")";
      }

      co_return td::Unit{};
    });

    co_await run_case("publish_cancel_promise fires on cancellation", [&]() -> Task<td::Unit> {
      auto fired = std::make_shared<std::atomic<bool>>(false);

      auto worker = [](std::shared_ptr<std::atomic<bool>> fired) -> Task<td::Unit> {
        current_scope_lease().publish_cancel_promise(
            [fired](td::Result<td::Unit>) { fired->store(true, std::memory_order_release); });
        co_await sleep_for(10.0);
        co_return td::Unit{};
      };

      auto t = worker(fired).start_without_scope();
      co_await sleep_for(0.01);
      expect_true(!fired->load(std::memory_order_acquire), "Should not fire before cancel");
      t.cancel();
      auto r = co_await std::move(t).wrap();
      expect_true(r.is_error(), "Worker should be cancelled");
      expect_eq(r.error().code(), kCancelledCode, "Expected Error(653)");
      expect_true(fired->load(std::memory_order_acquire), "publish_cancel_promise should have fired");
      co_return td::Unit{};
    });

    co_await run_case("publish_cancel_promise does not fire cancellation on normal completion",
                      [&]() -> Task<td::Unit> {
                        auto was_cancel = std::make_shared<std::atomic<bool>>(false);

                        auto worker = [](std::shared_ptr<std::atomic<bool>> was_cancel) -> Task<td::Unit> {
                          current_scope_lease().publish_cancel_promise([was_cancel](td::Result<td::Unit> r) {
                            if (r.is_ok()) {
                              was_cancel->store(true, std::memory_order_release);
                            }
                          });
                          co_return td::Unit{};
                        };

                        co_await worker(was_cancel);
                        expect_true(!was_cancel->load(std::memory_order_acquire),
                                    "Should not fire cancellation on normal completion");
                        co_return td::Unit{};
                      });

    LOG(INFO) << "cancellation_comprehensive_test completed";
    co_return td::Unit{};
  }

  // =============================================================================
  // ignore_cancellation() tests
  // =============================================================================
  Task<td::Unit> ignore_cancellation_test() {
    LOG(INFO) << "=== ignore_cancellation_test ===";

    constexpr int kCancelledCode = td::actor::kCancelledCode;

    auto run_case = [&](const char* name, auto make_test) -> Task<td::Unit> {
      LOG(INFO) << "Running: " << name;
      auto start_time = td::Timestamp::now();
      auto r = co_await make_test().wrap();
      r.ensure();
      auto elapsed = td::Timestamp::now().at() - start_time.at();
      LOG_CHECK(elapsed < 1.0) << name << " took too long: " << elapsed << "s";
      LOG(INFO) << name << ": PASSED";
      co_return td::Unit{};
    };

    // A: Fast-path cancellation on ready Task
    co_await run_case("fast-path cancellation on ready Task", [&]() -> Task<td::Unit> {
      auto ready_task = []() -> Task<int> { co_return 42; };
      auto outer = [&]() -> Task<td::Unit> {
        auto scope = co_await this_scope();
        auto child = ready_task().start_immediate_in_parent_scope();
        // Wait for child to complete
        co_await yield_on_current();
        // Now cancel ourselves — next co_await should detect it
        scope.cancel();
        // Even though child is ready with value, cancellation wins
        auto v = co_await std::move(child);
        (void)v;
        // Should not reach here
        co_return td::Status::Error("should not reach here");
      };
      auto t = outer().start_in_parent_scope();
      auto r = co_await std::move(t).wrap();
      expect_true(r.is_error(), "A: expected cancellation error");
      expect_eq(r.error().code(), kCancelledCode, "A: expected Error(653)");
      co_return td::Unit{};
    });

    // B: Fast-path cancellation on .wrap()
    co_await run_case("fast-path cancellation on .wrap()", [&]() -> Task<td::Unit> {
      auto ready_task = []() -> Task<int> { co_return 42; };
      auto outer = [&]() -> Task<td::Unit> {
        auto scope = co_await this_scope();
        auto child = ready_task().start_immediate_in_parent_scope();
        co_await yield_on_current();
        scope.cancel();
        auto r = co_await std::move(child).wrap();
        (void)r;
        co_return td::Status::Error("should not reach here");
      };
      auto t = outer().start_in_parent_scope();
      auto r = co_await std::move(t).wrap();
      expect_true(r.is_error(), "B: expected cancellation error");
      expect_eq(r.error().code(), kCancelledCode, "B: expected Error(653)");
      co_return td::Unit{};
    });

    // C: Fast-path cancellation on Result
    co_await run_case("fast-path cancellation on Result", [&]() -> Task<td::Unit> {
      auto outer = []() -> Task<td::Unit> {
        auto scope = co_await this_scope();
        scope.cancel();
        td::Result<int> ok_result = 42;
        auto v = co_await std::move(ok_result);
        (void)v;
        co_return td::Status::Error("should not reach here");
      };
      auto t = outer().start_in_parent_scope();
      auto r = co_await std::move(t).wrap();
      expect_true(r.is_error(), "C: expected cancellation error");
      expect_eq(r.error().code(), kCancelledCode, "C: expected Error(653)");
      co_return td::Unit{};
    });

    // D: Fast-path cancellation on Status
    co_await run_case("fast-path cancellation on Status", [&]() -> Task<td::Unit> {
      auto outer = []() -> Task<td::Unit> {
        auto scope = co_await this_scope();
        scope.cancel();
        co_await td::Status::OK();
        co_return td::Status::Error("should not reach here");
      };
      auto t = outer().start_in_parent_scope();
      auto r = co_await std::move(t).wrap();
      expect_true(r.is_error(), "D: expected cancellation error");
      expect_eq(r.error().code(), kCancelledCode, "D: expected Error(653)");
      co_return td::Unit{};
    });

    // E: cancel-before-enter — co_await ignore_cancellation() fails, task terminates
    co_await run_case("cancel-before-enter terminates task", [&]() -> Task<td::Unit> {
      auto outer = []() -> Task<td::Unit> {
        auto scope = co_await this_scope();
        scope.cancel();
        // Cancellation already set, ignore attempt should fail → task terminates
        auto guard = co_await ignore_cancellation();
        (void)guard;
        co_return td::Status::Error("should not reach here");
      };
      auto t = outer().start_in_parent_scope();
      auto r = co_await std::move(t).wrap();
      expect_true(r.is_error(), "E: expected cancellation error");
      expect_eq(r.error().code(), kCancelledCode, "E: expected Error(653)");
      co_return td::Unit{};
    });

    // F: enter-before-cancel — guard wins, deferred propagation, flush on drop
    co_await run_case("enter-before-cancel defers propagation", [&]() -> Task<td::Unit> {
      auto child_saw_cancel = std::make_shared<std::atomic<bool>>(false);

      auto outer = [child_saw_cancel]() -> Task<td::Unit> {
        auto scope = co_await this_scope();

        auto child = [child_saw_cancel]() -> Task<td::Unit> {
          co_await sleep_for(10.0);
          child_saw_cancel->store(true, std::memory_order_release);
          co_return td::Unit{};
        };
        auto started_child = child().start_in_parent_scope();

        {
          auto guard = co_await ignore_cancellation();
          // Cancel while guard is active — should defer propagation
          scope.cancel();

          // Inside guard: is_active() should return true (IGNORED suppresses)
          bool active = co_await is_active();
          expect_true(active, "F: is_active() should be true inside guard");

          // Child should NOT have been cancelled yet (propagation deferred)
          co_await yield_on_current();
          expect_true(!child_saw_cancel->load(std::memory_order_acquire),
                      "F: child should not see cancel while guard is active");
          // Guard drops here → leave_ignore → flush deferred propagation → child cancelled
        }

        // After guard drop, we're effectively cancelled, next co_await terminates
        auto r = co_await std::move(started_child).wrap();
        (void)r;
        co_return td::Unit{};
      };

      auto t = outer().start_in_parent_scope();
      auto r = co_await std::move(t).wrap();
      expect_true(r.is_error(), "F: expected cancellation error after guard drop");
      expect_eq(r.error().code(), kCancelledCode, "F: expected Error(653)");
      co_return td::Unit{};
    });

    // G: Nested guards — inner drop doesn't flush, outer does
    co_await run_case("nested guards: inner drop doesn't flush", [&]() -> Task<td::Unit> {
      auto child_cancelled = std::make_shared<std::atomic<bool>>(false);

      auto outer = [child_cancelled]() -> Task<td::Unit> {
        auto scope = co_await this_scope();

        auto child = [child_cancelled]() -> Task<td::Unit> {
          co_await sleep_for(10.0);
          child_cancelled->store(true, std::memory_order_release);
          co_return td::Unit{};
        };
        auto started_child = child().start_in_parent_scope();

        {
          auto guard1 = co_await ignore_cancellation();
          scope.cancel();

          {
            auto guard2 = co_await ignore_cancellation();
            // Still inside nested guard, should be active
            bool active = co_await is_active();
            expect_true(active, "G: is_active() true in nested guard");
            // guard2 drops here — inner drop, doesn't flush
          }

          // After inner drop, still have outer guard — no flush yet
          co_await yield_on_current();
          expect_true(!child_cancelled->load(std::memory_order_acquire),
                      "G: child should not be cancelled after inner guard drop");
          // guard1 drops here → outermost → flush → child cancelled
        }

        auto r = co_await std::move(started_child).wrap();
        (void)r;
        co_return td::Unit{};
      };

      auto t = outer().start_in_parent_scope();
      auto r = co_await std::move(t).wrap();
      expect_true(r.is_error(), "G: expected cancellation error");
      expect_eq(r.error().code(), kCancelledCode, "G: expected Error(653)");
      co_return td::Unit{};
    });

    // H: Publish-vs-cancel race — child always cancelled exactly once (idempotent on_cancel)
    co_await run_case("publish-vs-cancel: child cancelled exactly once", [&]() -> Task<td::Unit> {
      struct CountingNode : HeapCancelNode {
        std::atomic<int> cancel_count{0};
        void do_cancel() override {
          cancel_count.fetch_add(1, std::memory_order_relaxed);
        }
        void do_cleanup() override {
        }
      };

      auto node = make_ref<CountingNode>();
      auto node_ref = node.share();

      auto outer = [node = std::move(node)]() mutable -> Task<td::Unit> {
        current_scope_lease().publish_heap_cancel_node(*node);
        // Disarm so on_cancel won't fire from normal cancel path... actually we want it to fire
        // Let's not disarm, and let cancel trigger it
        co_await sleep_for(10.0);
        node->disarm();
        node.reset();
        co_return td::Unit{};
      };

      auto t = outer().start_in_parent_scope();
      co_await sleep_for(0.01);
      t.cancel();
      auto r = co_await std::move(t).wrap();
      expect_true(r.is_error(), "H: expected cancellation");
      // on_cancel should have been called exactly once (idempotent via disarm)
      expect_eq(node_ref->cancel_count.load(std::memory_order_acquire), 1,
                "H: on_cancel should be called exactly once");
      co_return td::Unit{};
    });

    // I: Counter boundary — IGNORED bit doesn't corrupt child_count
    co_await run_case("IGNORED bit doesn't corrupt child_count", [&]() -> Task<td::Unit> {
      auto outer = []() -> Task<td::Unit> {
        auto scope = co_await this_scope();
        auto* promise = scope.get_promise();
        auto initial_count = promise->cancellation_.child_count_relaxed_for_test();

        {
          auto guard = co_await ignore_cancellation();
          // Child count should not have changed from entering ignore
          auto count_in_guard = promise->cancellation_.child_count_relaxed_for_test();
          expect_eq(count_in_guard, initial_count, "I: child_count unchanged by ignore");

          // Add a child ref and verify it works correctly with IGNORED set
          promise->cancellation_.add_child_ref();
          auto count_with_child = promise->cancellation_.child_count_relaxed_for_test();
          expect_eq(count_with_child, initial_count + 1, "I: child_count incremented correctly with IGNORED");
          promise->cancellation_.release_child_ref(*promise, CancellationRuntime::ChildReleasePolicy::NoComplete);
        }

        auto final_count = promise->cancellation_.child_count_relaxed_for_test();
        expect_eq(final_count, initial_count, "I: child_count restored after guard drop");
        co_return td::Unit{};
      };

      co_await outer();
      co_return td::Unit{};
    });

    // J: is_active/ensure_active inside guard — returns true while guard is active
    co_await run_case("is_active/ensure_active inside guard", [&]() -> Task<td::Unit> {
      auto outer = []() -> Task<td::Unit> {
        auto scope = co_await this_scope();

        {
          auto guard = co_await ignore_cancellation();
          scope.cancel();

          // Inside guard with cancellation pending but ignored
          bool active = co_await is_active();
          expect_true(active, "J: is_active() should be true inside guard even after cancel");

          co_await ensure_active();  // Should NOT terminate (guard active)
        }

        // After guard drop, effectively cancelled — is_active returns false
        bool active = co_await is_active();
        expect_true(!active, "J: is_active() should be false after guard drop");

        // ensure_active() should terminate the task
        co_await ensure_active();
        co_return td::Status::Error("should not reach here");
      };

      auto t = outer().start_in_parent_scope();
      auto r = co_await std::move(t).wrap();
      expect_true(r.is_error(), "J: expected cancellation after guard drop");
      expect_eq(r.error().code(), kCancelledCode, "J: expected Error(653)");
      co_return td::Unit{};
    });

    // K: Regression — existing cancellation tests still work (covered by cancellation_comprehensive_test)
    // Just verify basic cancel-and-await still works
    co_await run_case("regression: basic cancel-and-await", [&]() -> Task<td::Unit> {
      auto worker = []() -> Task<int> {
        co_await sleep_for(10.0);
        co_return 42;
      };
      auto t = worker().start_in_parent_scope();
      co_await sleep_for(0.01);
      t.cancel();
      auto r = co_await std::move(t).wrap();
      expect_true(r.is_error(), "K: expected cancellation");
      expect_eq(r.error().code(), kCancelledCode, "K: expected Error(653)");
      co_return td::Unit{};
    });

    LOG(INFO) << "ignore_cancellation_test completed";
    co_return td::Unit{};
  }

  // Test ParentScopeLease RAII and thread-safety
  Task<td::Unit> cancellation_parent_scope_lease_test() {
    LOG(INFO) << "=== cancellation_parent_scope_lease_test ===";

    // Test 1: ParentScopeLease keeps scope alive via child_count
    co_await []() -> Task<td::Unit> {
      auto scope = co_await this_scope();
      auto* promise = scope.get_promise();
      CHECK(promise);

      auto initial_count = promise->cancellation_.child_count_relaxed_for_test();

      {
        auto handle = current_scope_lease();
        auto count_after_handle = promise->cancellation_.child_count_relaxed_for_test();
        expect_eq(count_after_handle, initial_count + 1, "Handle should increment child_count");
      }

      auto count_after_destroy = promise->cancellation_.child_count_relaxed_for_test();
      expect_eq(count_after_destroy, initial_count, "Destroying handle should decrement child_count");

      co_return td::Unit{};
    }();
    LOG(INFO) << "Handle RAII child_count: PASSED";

    // Test 2: ParentScopeLease promise reports cancellation correctly
    co_await []() -> Task<td::Unit> {
      auto scope = co_await this_scope();
      auto* promise = scope.get_promise();
      CHECK(promise);

      auto handle = current_scope_lease();

      // Initially not cancelled
      expect_true(handle && !handle.is_cancelled(), "Handle should not be cancelled initially");

      // Set cancelled flag
      promise->cancel();

      // Now should be cancelled
      expect_true(handle && handle.is_cancelled(), "Handle should see cancellation");

      co_return td::Unit{};
    }();
    LOG(INFO) << "Handle is_cancelled() works correctly: PASSED";

    // Test 3: Move assignment decrements old handle's count
    co_await []() -> Task<td::Unit> {
      auto scope = co_await this_scope();
      auto* promise = scope.get_promise();
      CHECK(promise);

      auto initial_count = promise->cancellation_.child_count_relaxed_for_test();

      auto handle1 = current_scope_lease();
      auto count_with_h1 = promise->cancellation_.child_count_relaxed_for_test();
      expect_eq(count_with_h1, initial_count + 1, "handle1 should add 1");

      auto handle2 = current_scope_lease();
      auto count_with_h2 = promise->cancellation_.child_count_relaxed_for_test();
      expect_eq(count_with_h2, initial_count + 2, "handle2 should add 1 more");

      handle1 = std::move(handle2);
      auto count_after_move = promise->cancellation_.child_count_relaxed_for_test();
      expect_eq(count_after_move, initial_count + 1, "Move assignment should decrement old count");

      co_return td::Unit{};
    }();
    LOG(INFO) << "Handle move assignment RAII: PASSED";

    // Test 4: Releasing last handle after parent final_suspend wakes parent
    co_await []() -> Task<td::Unit> {
      auto held_handle = std::make_shared<ParentScopeLease>();
      auto started = std::make_shared<std::atomic<bool>>(false);

      auto parent = [](std::shared_ptr<ParentScopeLease> held_handle,
                       std::shared_ptr<std::atomic<bool>> started) -> Task<td::Unit> {
        *held_handle = current_scope_lease();
        started->store(true, std::memory_order_release);
        co_return td::Unit{};
      }(held_handle, started)
                                                                          .start_in_parent_scope();

      bool parent_started = co_await wait_until([&] { return started->load(std::memory_order_acquire); });
      expect_true(parent_started, "Parent should start");

      for (int i = 0; i < 5; i++) {
        co_await yield_on_current();
      }
      expect_true(!parent.await_ready(), "Parent should wait while handle is held");

      *held_handle = ParentScopeLease{};

      bool ready = co_await wait_until([&] { return parent.await_ready(); });
      expect_true(ready, "Parent should become ready after last handle release");

      auto r = co_await std::move(parent).wrap();
      expect_ok(r, "Parent should complete after last handle release");
      co_return td::Unit{};
    }();
    LOG(INFO) << "Last-handle wake-up: PASSED";

    // Test 5: External handle can outlive detached creator frame safely
    co_await []() -> Task<td::Unit> {
      auto held_handle = std::make_shared<ParentScopeLease>();
      auto started = std::make_shared<std::atomic<bool>>(false);
      auto finished = std::make_shared<std::atomic<bool>>(false);

      [](std::shared_ptr<ParentScopeLease> held_handle, std::shared_ptr<std::atomic<bool>> started,
         std::shared_ptr<std::atomic<bool>> finished) -> Task<td::Unit> {
        *held_handle = current_scope_lease();
        started->store(true, std::memory_order_release);
        finished->store(true, std::memory_order_release);
        co_return td::Unit{};
      }(held_handle, started, finished)
                                                             .start_in_parent_scope()
                                                             .detach_silent();

      bool parent_started = co_await wait_until([&] { return started->load(std::memory_order_acquire); });
      expect_true(parent_started, "Detached parent should start");
      bool parent_finished = co_await wait_until([&] { return finished->load(std::memory_order_acquire); });
      expect_true(parent_finished, "Detached parent should finish body");

      for (int i = 0; i < 10; i++) {
        if (*held_handle) {
          (void)(*held_handle).is_cancelled();
        }
        co_await yield_on_current();
      }

      *held_handle = ParentScopeLease{};
      expect_true(!*held_handle, "Handle should be released");
      for (int i = 0; i < 5; i++) {
        co_await yield_on_current();
      }
      co_return td::Unit{};
    }();
    LOG(INFO) << "Detached frame lifetime with external handle: PASSED";

    // Test 6: start_external_in_parent_scope keeps parent waiting until external completion
    co_await []() -> Task<td::Unit> {
      using ExternalPromise = StartedTask<td::Unit>::ExternalPromise;

      auto parent_started = std::make_shared<std::atomic<bool>>(false);
      auto parent_external_promise = std::make_shared<std::optional<ExternalPromise>>();

      auto parent =
          [](std::shared_ptr<std::atomic<bool>> parent_started,
             std::shared_ptr<std::optional<ExternalPromise>> parent_external_promise) -> Task<td::Unit> {
        auto lease = current_scope_lease();

        auto external_child = []() -> Task<td::Unit> {
          co_return typename Task<td::Unit>::promise_type::ExternalResult{};
        }();
        parent_external_promise->emplace(&external_child.h.promise());

        std::move(external_child).start_external_in_parent_scope(std::move(lease)).detach_silent();
        parent_started->store(true, std::memory_order_release);
        co_return td::Unit{};
      }(parent_started, parent_external_promise)
                                                                                             .start_in_parent_scope();

      bool started = co_await wait_until([&] { return parent_started->load(std::memory_order_acquire); });
      expect_true(started, "Parent should start");
      expect_true(parent_external_promise->has_value(), "External promise should be initialized");

      for (int i = 0; i < 5; i++) {
        co_await yield_on_current();
      }
      expect_true(!parent.await_ready(), "Parent should wait until external child completion");

      parent_external_promise->value().set_value(td::Unit{});
      bool ready = co_await wait_until([&] { return parent.await_ready(); });
      expect_true(ready, "Parent should become ready after external child completion");

      auto r = co_await std::move(parent).wrap();
      expect_ok(r, "Parent should complete after external child completion");
      co_return td::Unit{};
    }();
    LOG(INFO) << "start_external_in_parent_scope parent wait: PASSED";

    // Test 7: start_external_in_parent_scope + set_error still wakes parent
    co_await []() -> Task<td::Unit> {
      using ExternalPromise = StartedTask<td::Unit>::ExternalPromise;

      auto parent_started = std::make_shared<std::atomic<bool>>(false);
      auto parent_external_promise = std::make_shared<std::optional<ExternalPromise>>();

      auto parent =
          [](std::shared_ptr<std::atomic<bool>> parent_started,
             std::shared_ptr<std::optional<ExternalPromise>> parent_external_promise) -> Task<td::Unit> {
        auto lease = current_scope_lease();

        auto external_child = []() -> Task<td::Unit> {
          co_return typename Task<td::Unit>::promise_type::ExternalResult{};
        }();
        parent_external_promise->emplace(&external_child.h.promise());

        std::move(external_child).start_external_in_parent_scope(std::move(lease)).detach_silent();
        parent_started->store(true, std::memory_order_release);
        co_return td::Unit{};
      }(parent_started, parent_external_promise)
                                                                                             .start_in_parent_scope();

      bool started = co_await wait_until([&] { return parent_started->load(std::memory_order_acquire); });
      expect_true(started, "Parent should start");
      expect_true(parent_external_promise->has_value(), "External promise should be initialized");

      for (int i = 0; i < 5; i++) {
        co_await yield_on_current();
      }
      expect_true(!parent.await_ready(), "Parent should wait until external child completion");

      parent_external_promise->value().set_error(td::Status::Error("external_child_error"));
      bool ready = co_await wait_until([&] { return parent.await_ready(); });
      expect_true(ready, "Parent should become ready after external child error completion");

      auto r = co_await std::move(parent).wrap();
      expect_ok(r, "Parent should complete after external child error completion");
      co_return td::Unit{};
    }();
    LOG(INFO) << "start_external_in_parent_scope set_error wake-up: PASSED";

    // Test 8: dropping ExternalPromise triggers deleter path and wakes parent
    co_await []() -> Task<td::Unit> {
      using ExternalPromise = StartedTask<td::Unit>::ExternalPromise;

      auto parent_started = std::make_shared<std::atomic<bool>>(false);
      auto parent_external_promise = std::make_shared<std::optional<ExternalPromise>>();

      auto parent =
          [](std::shared_ptr<std::atomic<bool>> parent_started,
             std::shared_ptr<std::optional<ExternalPromise>> parent_external_promise) -> Task<td::Unit> {
        auto lease = current_scope_lease();

        auto external_child = []() -> Task<td::Unit> {
          co_return typename Task<td::Unit>::promise_type::ExternalResult{};
        }();
        parent_external_promise->emplace(&external_child.h.promise());

        std::move(external_child).start_external_in_parent_scope(std::move(lease)).detach_silent();
        parent_started->store(true, std::memory_order_release);
        co_return td::Unit{};
      }(parent_started, parent_external_promise)
                                                                                             .start_in_parent_scope();

      bool started = co_await wait_until([&] { return parent_started->load(std::memory_order_acquire); });
      expect_true(started, "Parent should start");
      expect_true(parent_external_promise->has_value(), "External promise should be initialized");

      for (int i = 0; i < 5; i++) {
        co_await yield_on_current();
      }
      expect_true(!parent.await_ready(), "Parent should wait until external child completion");

      parent_external_promise->reset();
      bool ready = co_await wait_until([&] { return parent.await_ready(); });
      expect_true(ready, "Parent should become ready after ExternalPromise drop");

      auto r = co_await std::move(parent).wrap();
      expect_ok(r, "Parent should complete after ExternalPromise drop");
      co_return td::Unit{};
    }();
    LOG(INFO) << "start_external_in_parent_scope drop promise wake-up: PASSED";

    LOG(INFO) << "cancellation_parent_scope_lease_test passed";
    co_return td::Unit{};
  }

  Task<td::Unit> task_cancellation_source_test() {
    LOG(INFO) << "=== task_cancellation_source_test ===";

    constexpr int kCancelledCode = td::actor::kCancelledCode;

    // Detached source can mint leases incrementally and cancel all children.
    co_await [&]() -> Task<td::Unit> {
      auto source = TaskCancellationSource::create_detached();
      std::vector<StartedTask<td::Unit>> children;
      children.reserve(4);

      for (int i = 0; i < 4; i++) {
        children.push_back([]() -> Task<td::Unit> {
          co_await sleep_for(10.0);
          co_return td::Unit{};
        }()
                                       .start_in_parent_scope(source.get_scope_lease()));
      }

      source.cancel();

      for (auto& child : children) {
        auto r = co_await std::move(child).wrap();
        expect_true(r.is_error(), "Detached source child should be cancelled");
        expect_eq(r.error().code(), kCancelledCode, "Detached source child cancellation code should be 653");
      }
      co_return td::Unit{};
    }();
    LOG(INFO) << "TaskCancellationSource detached fanout cancel: PASSED";

    // Destructor path should also cancel children.
    co_await [&]() -> Task<td::Unit> {
      StartedTask<td::Unit> child;
      {
        auto source = TaskCancellationSource::create_detached();
        child = []() -> Task<td::Unit> {
          co_await sleep_for(10.0);
          co_return td::Unit{};
        }()
                            .start_in_parent_scope(source.get_scope_lease());
      }

      auto r = co_await std::move(child).wrap();
      expect_true(r.is_error(), "Source destructor should cancel child");
      expect_eq(r.error().code(), kCancelledCode, "Source destructor cancellation code should be 653");
      co_return td::Unit{};
    }();
    LOG(INFO) << "TaskCancellationSource destructor cancel: PASSED";

    // Move-only ownership should work.
    co_await [&]() -> Task<td::Unit> {
      auto source1 = TaskCancellationSource::create_detached();
      auto source2 = std::move(source1);

      auto child = []() -> Task<td::Unit> {
        co_await sleep_for(10.0);
        co_return td::Unit{};
      }()
                               .start_in_parent_scope(source2.get_scope_lease());

      source2.cancel();
      auto r = co_await std::move(child).wrap();
      expect_true(r.is_error(), "Moved TaskCancellationSource should still cancel child");
      expect_eq(r.error().code(), kCancelledCode, "Moved TaskCancellationSource cancellation code should be 653");
      co_return td::Unit{};
    }();
    LOG(INFO) << "TaskCancellationSource move semantics: PASSED";

    // StartedTask move assignment should cancel previously owned unfinished task.
    co_await [&]() -> Task<td::Unit> {
      auto completed = std::make_shared<std::atomic<bool>>(false);
      StartedTask<td::Unit> slot = [completed]() -> Task<td::Unit> {
        co_await sleep_for(0.2);
        completed->store(true, std::memory_order_release);
        co_return td::Unit{};
      }()
                                                        .start_in_parent_scope();

      auto replacement = []() -> Task<td::Unit> { co_return td::Unit{}; }().start_immediate_without_scope();
      slot = std::move(replacement);

      co_await sleep_for(0.3);
      expect_true(!completed->load(std::memory_order_acquire),
                  "Move-assigned-out StartedTask should be cancelled, not detached-running");

      auto r = co_await std::move(slot).wrap();
      expect_ok(r, "Replacement task should still complete");
      co_return td::Unit{};
    }();
    LOG(INFO) << "StartedTask move assignment cancels old task: PASSED";

    // Linked source should hold exactly one parent child-count lease.
    co_await [&]() -> Task<td::Unit> {
      auto scope = co_await this_scope();
      auto* promise = scope.get_promise();
      CHECK(promise);
      auto initial_count = promise->cancellation_.child_count_relaxed_for_test();

      auto source = TaskCancellationSource::create_linked();
      auto count_with_source = promise->cancellation_.child_count_relaxed_for_test();
      expect_eq(count_with_source, initial_count + 1, "Linked source should add one parent child-count lease");

      source.cancel();
      auto count_after_cancel = promise->cancellation_.child_count_relaxed_for_test();
      expect_eq(count_after_cancel, initial_count, "Cancelling linked source should release parent child-count lease");

      co_return td::Unit{};
    }();
    LOG(INFO) << "TaskCancellationSource linked parent lease: PASSED";

    LOG(INFO) << "task_cancellation_source_test passed";
    co_return td::Unit{};
  }

  enum class ExternalParentAction : uint8_t { SetValue = 0, SetError = 1, DropPromise = 2 };

  struct ExternalParentReproCase {
    int case_id{0};
    int repeat{0};
    bool cancel_parent{false};
    bool cancel_child_before_detach{false};
    int setup_yields{0};
    int action_yields{0};
    ExternalParentAction action{ExternalParentAction::SetValue};
  };

  static const char* external_parent_action_name(ExternalParentAction action) {
    switch (action) {
      case ExternalParentAction::SetValue:
        return "set_value";
      case ExternalParentAction::SetError:
        return "set_error";
      case ExternalParentAction::DropPromise:
        return "drop_promise";
    }
    return "unknown";
  }

  Task<td::Unit> external_parent_scope_repro_case(ExternalParentReproCase c) {
    using ExternalPromise = StartedTask<td::Unit>::ExternalPromise;

    LOG(INFO) << "external-parent repro case begin id=" << c.case_id << " repeat=" << c.repeat
              << " cancel_parent=" << c.cancel_parent << " cancel_child_before_detach=" << c.cancel_child_before_detach
              << " setup_yields=" << c.setup_yields << " action_yields=" << c.action_yields
              << " action=" << external_parent_action_name(c.action);

    auto started = std::make_shared<std::atomic<bool>>(false);
    auto external_promise = std::make_shared<std::optional<ExternalPromise>>();

    auto parent = [](std::shared_ptr<std::atomic<bool>> started,
                     std::shared_ptr<std::optional<ExternalPromise>> external_promise, bool cancel_child_before_detach,
                     int setup_yields) -> Task<td::Unit> {
      auto lease = current_scope_lease();
      auto external_child = []() -> Task<td::Unit> {
        co_return typename Task<td::Unit>::promise_type::ExternalResult{};
      }();
      external_promise->emplace(&external_child.h.promise());

      auto started_child = std::move(external_child).start_external_in_parent_scope(std::move(lease));
      for (int i = 0; i < setup_yields; i++) {
        co_await yield_on_current();
      }
      if (cancel_child_before_detach) {
        started_child.cancel();
      }
      started_child.detach_silent();
      started->store(true, std::memory_order_release);
      co_return td::Unit{};
    }(started, external_promise, c.cancel_child_before_detach, c.setup_yields)
                                              .start_in_parent_scope();

    bool parent_started = co_await wait_until([started] { return started->load(std::memory_order_acquire); }, 5000);
    LOG_CHECK(parent_started) << "external-parent repro: parent not started, case_id=" << c.case_id;
    LOG_CHECK(external_promise->has_value())
        << "external-parent repro: missing external promise, case_id=" << c.case_id;

    for (int i = 0; i < c.action_yields; i++) {
      co_await yield_on_current();
    }

    if (c.cancel_parent) {
      parent.cancel();
    }

    switch (c.action) {
      case ExternalParentAction::SetValue:
        external_promise->value().set_value(td::Unit{});
        break;
      case ExternalParentAction::SetError:
        external_promise->value().set_error(td::Status::Error("external_parent_repro_error"));
        break;
      case ExternalParentAction::DropPromise:
        external_promise->reset();
        break;
    }

    bool parent_ready = co_await wait_until([&parent] { return parent.await_ready(); }, 5000);
    LOG_CHECK(parent_ready) << "external-parent repro: parent stalled, case_id=" << c.case_id;

    auto r = co_await std::move(parent).wrap();
    if (c.cancel_parent) {
      if (r.is_error()) {
        LOG_CHECK(r.error().code() == kCancelledCode)
            << "external-parent repro: unexpected error code, case_id=" << c.case_id;
        LOG(INFO) << "external-parent repro case cancel outcome id=" << c.case_id << ": err653";
      } else {
        LOG(INFO) << "external-parent repro case cancel outcome id=" << c.case_id << ": ok";
      }
    } else {
      LOG_CHECK(r.is_ok()) << "external-parent repro: expected OK parent, case_id=" << c.case_id;
    }

    LOG(INFO) << "external-parent repro case done id=" << c.case_id;
    co_return td::Unit{};
  }

  Task<td::Unit> external_parent_scope_repro_localize() {
    LOG(INFO) << "=== external_parent_scope_repro_localize ===";

    int case_id = 0;
    constexpr int kRepeatsPerConfig = 16;
    for (int cancel_parent = 0; cancel_parent <= 1; cancel_parent++) {
      for (int cancel_child = 0; cancel_child <= 1; cancel_child++) {
        for (int action = 0; action <= 2; action++) {
          for (int setup_yields = 0; setup_yields <= 2; setup_yields++) {
            for (int action_yields = 0; action_yields <= 2; action_yields++) {
              for (int repeat = 0; repeat < kRepeatsPerConfig; repeat++) {
                case_id++;
                co_await external_parent_scope_repro_case(ExternalParentReproCase{
                    .case_id = case_id,
                    .repeat = repeat,
                    .cancel_parent = cancel_parent != 0,
                    .cancel_child_before_detach = cancel_child != 0,
                    .setup_yields = setup_yields,
                    .action_yields = action_yields,
                    .action = static_cast<ExternalParentAction>(action),
                });
              }
            }
          }
        }
      }
    }

    LOG(INFO) << "external_parent_scope_repro_localize completed all cases";
    co_return td::Unit{};
  }

  // Regression: scheduled sleep_for cancellation must be safe under heavy races.
  Task<td::Unit> scheduled_sleep_cancel_stress_test() {
    LOG(INFO) << "=== scheduled_sleep_cancel_stress_test ===";

    constexpr int kIterations = 5000;
    for (int i = 0; i < kIterations; i++) {
      auto started = []() -> Task<td::Unit> {
        co_await sleep_for(60.0);
        co_return td::Unit{};
      }()
                                 .start_in_parent_scope();

      if ((i & 1) == 0) {
        co_await yield_on_current();
      } else {
        co_await sleep_for(0.0005);
      }

      started.cancel();
      auto r = co_await std::move(started).wrap();
      expect_true(r.is_error(), "scheduled sleep_for task should be cancelled");
      expect_eq(r.error().code(), kCancelledCode, "scheduled sleep_for should return cancelled error");
    }

    LOG(INFO) << "scheduled_sleep_cancel_stress_test passed";
    co_return td::Unit{};
  }

  // Serious randomized cancellation stress with fixed deterministic configuration.
  Task<td::Unit> cancellation_serious_stress_test() {
    LOG(INFO) << "=== cancellation_serious_stress_test ===";

    constexpr int kIterations = 10000;
    constexpr td::uint64 kBaseSeed = 0xC0FFEE1234ULL;

    struct StressStats {
      td::uint64 started_tasks{0};
      td::uint64 cancel_calls{0};
      td::uint64 completed_ok{0};
      td::uint64 completed_err653{0};
    };
    StressStats stats;
    auto started_at = td::Timestamp::now();

    for (int iter = 0; iter < kIterations; iter++) {
      auto seed = kBaseSeed ^ (static_cast<td::uint64>(iter + 1) * 0x9e3779b97f4a7c15ULL);
      td::Random::Xorshift128plus rnd(seed);
      auto scenario = rnd.fast(0, 3);

      if (scenario == 0) {
        auto cancel_delay_steps = rnd.fast(0, 8);
        auto started = []() -> Task<td::Unit> {
          co_await sleep_for(60.0);
          co_return td::Unit{};
        }()
                                   .start_in_parent_scope();
        stats.started_tasks++;
        for (int i = 0; i < cancel_delay_steps; i++) {
          co_await yield_on_current();
        }
        started.cancel();
        stats.cancel_calls++;
        auto r = co_await std::move(started).wrap();
        LOG_CHECK(r.is_error() && r.error().code() == kCancelledCode)
            << "serious stress: suspended cancel scenario expected err653, iter=" << iter << " seed=" << seed;
        stats.completed_err653++;
      } else if (scenario == 1) {
        auto guard_depth = rnd.fast(0, 2);
        auto cancel_inside_guard = rnd.fast(0, 1) == 1;

        auto outer = [](int guard_depth_arg, bool cancel_inside_guard_arg) -> Task<td::Unit> {
          auto scope = co_await this_scope();
          auto child = []() -> Task<td::Unit> {
            for (int i = 0; i < 8; i++) {
              co_await sleep_for(0.0004);
              co_await yield_on_current();
            }
            co_return td::Unit{};
          };
          auto started_child = child().start_in_parent_scope();

          bool active_inside_guard = false;
          if (guard_depth_arg == 0) {
            if (cancel_inside_guard_arg) {
              scope.cancel();
            }
          } else if (guard_depth_arg == 1) {
            auto guard = co_await ignore_cancellation();
            if (cancel_inside_guard_arg) {
              scope.cancel();
            }
            active_inside_guard = co_await is_active();
          } else {
            auto guard1 = co_await ignore_cancellation();
            auto guard2 = co_await ignore_cancellation();
            if (cancel_inside_guard_arg) {
              scope.cancel();
            }
            active_inside_guard = co_await is_active();
          }

          if (guard_depth_arg > 0 && cancel_inside_guard_arg) {
            LOG_CHECK(active_inside_guard) << "serious stress: expected active inside ignore guard";
          }
          if (!cancel_inside_guard_arg) {
            scope.cancel();
          }

          (void)co_await std::move(started_child).wrap();
          co_return td::Unit{};
        }(guard_depth, cancel_inside_guard);

        auto started = std::move(outer).start_in_parent_scope();
        stats.started_tasks++;
        auto r = co_await std::move(started).wrap();
        LOG_CHECK(r.is_error() && r.error().code() == kCancelledCode)
            << "serious stress: ignore scenario expected err653, iter=" << iter << " seed=" << seed
            << " guard_depth=" << guard_depth << " cancel_inside_guard=" << cancel_inside_guard;
        stats.completed_err653++;
      } else if (scenario == 2) {
        auto task_count = rnd.fast(8, 20);
        std::vector<StartedTask<td::Unit>> tasks;
        tasks.reserve(static_cast<size_t>(task_count));

        for (int i = 0; i < task_count; i++) {
          auto worker = [](int worker_i) -> Task<td::Unit> {
            for (int step = 0; step < 3; step++) {
              if (((worker_i + step) & 1) == 0) {
                co_await yield_on_current();
              } else {
                co_await sleep_for(0.0003);
              }
            }
            co_await sleep_for(0.004);
            co_return td::Unit{};
          };
          tasks.push_back(worker(i).start_in_parent_scope());
          stats.started_tasks++;
        }

        for (int spin = 0, spins = rnd.fast(0, 4); spin < spins; spin++) {
          co_await yield_on_current();
        }

        for (int i = 0; i < task_count; i++) {
          if (rnd.fast(0, 3) != 0) {
            tasks[static_cast<size_t>(i)].cancel();
            stats.cancel_calls++;
          }
        }

        for (auto& task : tasks) {
          auto r = co_await std::move(task).wrap();
          if (r.is_error()) {
            LOG_CHECK(r.error().code() == kCancelledCode)
                << "serious stress: timer scenario unexpected error code, iter=" << iter << " seed=" << seed;
            stats.completed_err653++;
          } else {
            stats.completed_ok++;
          }
        }
      } else {
        auto cancel_delay_steps = rnd.fast(0, 4);
        auto work_steps = rnd.fast(2, 8);

        auto parent = [](int work_steps_arg) -> Task<td::Unit> {
          auto grandchild = [](int work_steps_inner) -> Task<td::Unit> {
            for (int i = 0; i < work_steps_inner; i++) {
              if ((i & 1) == 0) {
                co_await sleep_for(0.0005);
              } else {
                co_await yield_on_current();
              }
            }
            co_return td::Unit{};
          };
          auto child = [](Task<td::Unit> grandchild_task) -> Task<td::Unit> {
            co_await std::move(grandchild_task).start_in_parent_scope();
            co_return td::Unit{};
          };
          co_await child(grandchild(work_steps_arg)).start_in_parent_scope();
          co_return td::Unit{};
        }(work_steps);

        auto started = std::move(parent).start_in_parent_scope();
        stats.started_tasks++;
        for (int i = 0; i < cancel_delay_steps; i++) {
          co_await yield_on_current();
        }
        started.cancel();
        stats.cancel_calls++;

        auto r = co_await std::move(started).wrap();
        LOG_CHECK(r.is_error() && r.error().code() == kCancelledCode)
            << "serious stress: nested cancel scenario expected err653, iter=" << iter << " seed=" << seed;
        stats.completed_err653++;
      }

      if ((iter + 1) % 2000 == 0) {
        LOG(INFO) << "cancellation_serious_stress_test progress: " << (iter + 1) << "/" << kIterations;
      }
    }

    LOG_CHECK(stats.started_tasks == stats.completed_ok + stats.completed_err653)
        << "serious stress: completion mismatch started=" << stats.started_tasks << " ok=" << stats.completed_ok
        << " err653=" << stats.completed_err653;

    auto elapsed = td::Timestamp::now().at() - started_at.at();
    LOG(INFO) << "cancellation_serious_stress_test stats: started=" << stats.started_tasks
              << " cancel_calls=" << stats.cancel_calls << " ok=" << stats.completed_ok
              << " err653=" << stats.completed_err653 << " elapsed=" << elapsed << "s";
    LOG(INFO) << "cancellation_serious_stress_test passed";
    co_return td::Unit{};
  }

  // Test with_timeout()
  Task<td::Unit> with_timeout_test() {
    LOG(INFO) << "=== with_timeout_test ===";

    // Test 1: Task completes before timeout
    co_await []() -> Task<td::Unit> {
      auto fast_task = []() -> Task<int> {
        co_await sleep_for(0.01);
        co_return 42;
      };

      auto started = fast_task().start_in_parent_scope();
      auto result = co_await with_timeout(std::move(started), 1.0);

      expect_true(result.is_ok(), "Task should complete successfully");
      expect_eq(result.ok(), 42, "Task should return correct value");

      co_return td::Unit{};
    }();
    LOG(INFO) << "Task completes before timeout: PASSED";

    // Test 2: Timeout fires and cancels task
    co_await []() -> Task<td::Unit> {
      auto slow_task = []() -> Task<int> {
        co_await sleep_for(10.0);  // Will hang if timeout doesn't work
        co_return 42;
      };

      auto start_time = td::Timestamp::now();
      auto started = slow_task().start_in_parent_scope();
      auto result = co_await with_timeout(std::move(started), 0.05);
      auto elapsed = td::Timestamp::now().at() - start_time.at();

      expect_true(result.is_error(), "Task should be cancelled by timeout");
      expect_eq(result.error().code(), 653, "Error should be cancellation (653)");
      expect_true(elapsed < 1.0, "Should not wait for full 10 seconds");

      co_return td::Unit{};
    }();
    LOG(INFO) << "Timeout fires and cancels task: PASSED";

    // Test 3: Zero/negative timeout immediately cancels
    co_await []() -> Task<td::Unit> {
      auto task = []() -> Task<int> {
        co_await sleep_for(10.0);
        co_return 42;
      };

      auto started = task().start_in_parent_scope();
      auto result = co_await with_timeout(std::move(started), 0.0);

      expect_true(result.is_error(), "Zero timeout should cancel immediately");
      expect_eq(result.error().code(), 653, "Error should be cancellation (653)");

      co_return td::Unit{};
    }();
    LOG(INFO) << "Zero timeout cancels immediately: PASSED";

    // Test 4: with_timeout with Timestamp overload
    co_await []() -> Task<td::Unit> {
      auto fast_task = []() -> Task<int> {
        co_await sleep_for(0.01);
        co_return 99;
      };

      auto started = fast_task().start_in_parent_scope();
      auto result = co_await with_timeout(std::move(started), td::Timestamp::in(1.0));

      expect_true(result.is_ok(), "Task should complete before deadline");
      expect_eq(result.ok(), 99, "Task should return correct value");

      co_return td::Unit{};
    }();
    LOG(INFO) << "with_timeout(Timestamp) overload: PASSED";

    LOG(INFO) << "with_timeout_test passed";
    co_return td::Unit{};
  }

  // Test to confirm SCOPE_EXIT vs final_suspend timing
  Task<td::Unit> scope_exit_timing_test() {
    LOG(INFO) << "=== scope_exit_timing_test ===";

    struct Logger {
      const char* name;
      Logger(const char* n) : name(n) {
        LOG(INFO) << "  [" << name << "] constructed";
      }
      ~Logger() {
        LOG(INFO) << "  [" << name << "] DESTROYED";
      }
    };

    auto test_coro = []() -> Task<int> {
      Logger local("local_in_coro");
      LOG(INFO) << "  [coro] before co_return";
      co_return 42;
      // After co_return: when does ~Logger run vs final_suspend?
    };

    LOG(INFO) << "Starting test coroutine...";
    auto result = co_await test_coro();
    LOG(INFO) << "Test coroutine returned: " << result;

    LOG(INFO) << "scope_exit_timing_test passed";
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
    co_await test_trace();
    co_await stop_actor();
    co_await promise_destroy_in_mailbox();
    co_await promise_destroy_in_actor_member();
    co_await co_return_empty_braces();
    co_await actor_ref_uaf();       // UAF test - demonstrates need for ActorRef
    co_await sleep_for_test();      // Lightweight timer test
    co_await coro_mutex_test();     // CoroMutex mutual exclusion test
    co_await coro_coalesce_test();  // CoroCoalesce coalescing test
    co_await actor_task_unwrap_bug();
    co_await structured_concurrency_test();           // Structured concurrency tests
    co_await cancellation_comprehensive_test();       // Cancellation semantics documentation
    co_await ignore_cancellation_test();              // ignore_cancellation() guard semantics
    co_await cancellation_parent_scope_lease_test();  // ParentScopeLease RAII and thread-safety
    co_await task_cancellation_source_test();         // TaskCancellationSource lifecycle and propagation
    co_await cancellation_serious_stress_test();      // Heavy randomized cancellation stress
    co_await scheduled_sleep_cancel_stress_test();
    co_await scope_exit_timing_test();  // Test SCOPE_EXIT vs final_suspend timing
    co_await with_timeout_test();       // with_timeout() function

    (void)co_await ask(logger_, &TestLogger::log, std::string("All tests passed"));
    co_return td::Unit();
  }

 private:
  bool run_external_parent_repro_only_{false};
  td::actor::ActorId<TestDatabase> db_;
  td::actor::ActorId<TestLogger> logger_;
};

// 5) Runner
int main(int argc, char** argv) {
  bool run_external_parent_repro_only = false;
  for (int i = 1; i < argc; i++) {
    auto arg = std::string_view(argv[i]);
    if (arg == "--repro-external-parent") {
      run_external_parent_repro_only = true;
      continue;
    }
    LOG(ERROR) << "Unknown argument: " << std::string(arg);
    return 1;
  }

  SET_VERBOSITY_LEVEL(VERBOSITY_NAME(INFO));
  td::actor::Scheduler scheduler({4});
  scheduler.run_in_context(
      [&] { td::actor::create_actor<CoroSpec>("CoroSpec", run_external_parent_repro_only).release(); });
  scheduler.run();
  return 0;
}
