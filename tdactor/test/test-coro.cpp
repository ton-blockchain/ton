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
#include "td/actor/core/ActorInfo.h"
#include "td/actor/coro.h"
#include "td/actor/coro_test.h"
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

// === Structured Concurrency Helper Functions ===

static Task<int> child_task(std::shared_ptr<std::atomic<int>> counter, double sleep_time, int return_value) {
  co_await coro_sleep(td::Timestamp::in(sleep_time));
  counter->fetch_add(1, std::memory_order_relaxed);
  co_return return_value;
}

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

// === Cancellation Test Helpers ===

struct Gate {
  StartedTask<td::Unit> task;
  StartedTask<td::Unit>::ExternalPromise promise;
  void open() {
    promise.set_value(td::Unit{});
  }
};

static Gate make_gate() {
  auto [t, p] = StartedTask<td::Unit>::make_bridge();
  return Gate{std::move(t), std::move(p)};
}

// === Additional file-scope helpers for converted tests ===

static Task<td::Unit> slow_task() {
  td::usleep_for(2000000);
  co_return td::Unit{};
}

static Task<int> rec_fast(int n) {
  if (n == 0)
    co_return 0;
  int r = co_await rec_fast(n - 1);
  co_return r + 1;
}

class RecTestActor : public td::actor::Actor {
 public:
  Task<int> rec_slow(int n) {
    if (n == 0)
      co_return 0;
    int r = co_await ask(actor_id(this), &RecTestActor::rec_slow, n - 1);
    co_return r + 1;
  }
};

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

static Task<td::Unit> external_parent_scope_repro_case(ExternalParentReproCase c) {
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
  LOG_CHECK(external_promise->has_value()) << "external-parent repro: missing external promise, case_id=" << c.case_id;

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
      LOG_CHECK(r.error().code() == td::actor::kCancelledCode)
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

// =============================================================================
// Structured concurrency tests (TEST_CORO)
// =============================================================================

TEST_CORO(Coro, this_scope_returns_valid_scope) {
  expect_true(co_await test_scope_validity(), "this_scope() should return valid scope");
  co_return td::Unit{};
}

TEST_CORO(Coro, parent_waits_for_1_child) {
  auto child_completed = std::make_shared<std::atomic<bool>>(false);
  expect_eq(co_await parent_with_one_child(child_completed), 42, "Parent result should be correct");
  expect_true(child_completed->load(std::memory_order_acquire), "Parent should wait for child");
  co_return td::Unit{};
}

TEST_CORO(Coro, parent_waits_for_2_children) {
  auto child_count = std::make_shared<std::atomic<int>>(0);
  expect_eq(co_await parent_with_two_children(child_count), 100, "Parent result should be correct");
  expect_eq(child_count->load(), 2, "Both children should complete");
  co_return td::Unit{};
}

TEST_CORO(Coro, TLS_is_set_after_scheduler_resume) {
  expect_eq(co_await tls_after_yield(), 1, "TLS should be set after scheduler resume");
  co_return td::Unit{};
}

TEST_CORO(Coro, start_immediate_restores_caller_TLS) {
  expect_eq(co_await tls_safety_parent(), 42, "start_immediate should restore TLS and child should complete");
  co_return td::Unit{};
}

TEST_CORO(Coro, start_detached_completes_and_cleans_up) {
  auto completed = std::make_shared<std::atomic<bool>>(false);
  detached_setter(completed).start_in_parent_scope().detach_silent();
  for (int i = 0; i < 5 && !completed->load(std::memory_order_acquire); i++) {
    co_await yield_on_current();
  }
  expect_true(completed->load(std::memory_order_acquire), "Detached task should complete");
  co_return td::Unit{};
}

TEST_CORO(Coro, nested_scopes_grandparent_parent_child_wait_correctly) {
  auto grandchild_done = std::make_shared<std::atomic<bool>>(false);
  expect_eq(co_await grandparent_task(grandchild_done), 1, "Grandparent result should be correct");
  expect_true(grandchild_done->load(std::memory_order_acquire),
              "Grandchild should complete before grandparent returns");
  co_return td::Unit{};
}

TEST_CORO(Coro, concurrent_child_completion_stress) {
  constexpr int NUM_CHILDREN = 20;
  auto completion_count = std::make_shared<std::atomic<int>>(0);
  expect_eq(co_await stress_parent(completion_count, NUM_CHILDREN), 999, "Parent result should be correct");
  expect_eq(completion_count->load(), NUM_CHILDREN, "All children should complete");
  co_return td::Unit{};
}

TEST_CORO(Coro, TLS_matches_this_scope_on_scheduler_path) {
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
}

TEST_CORO(Coro, ask_promise_path_preserves_scope_tracking) {
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
}

TEST_CORO(Coro, parent_error_waits_for_children_before_completing) {
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
}

TEST_CORO(Coro, ask_Task_return_remote_coroutine_TLS_resume_location) {
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
}

// =============================================================================
// Cancellation tests (TEST_CORO)
// =============================================================================

TEST_CORO(Coro, cancelled_task_returns_Error653_on_resume_boundary) {
  constexpr int kCancelledCode = td::actor::kCancelledCode;
  auto sleeper = []() -> Task<int> {
    co_await sleep_for(10.0);
    co_return 1;
  };
  auto t = sleeper().start_in_parent_scope();
  co_await sleep_for(0.01);
  t.cancel();
  auto r = co_await std::move(t).wrap();
  expect_true(r.is_error(), "Expected cancelled error");
  expect_eq(r.error().code(), kCancelledCode, "Expected Error(653) from cancellation");
  co_return td::Unit{};
}

TEST_CORO(Coro, cancel_works_after_immediate_ready_sleep) {
  constexpr int kCancelledCode = td::actor::kCancelledCode;
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
}

TEST_CORO(Coro, cancel_parent_while_awaiting_1_of_N_children) {
  constexpr int kCancelledCode = td::actor::kCancelledCode;
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
    (void)co_await std::move(awaited_child).wrap();
    co_return td::Unit{};
  };

  auto parent =
      parent_body(children_started, std::move(g0.task), std::move(g1.task), std::move(g2.task)).start_in_parent_scope();

  bool started_ok = false;
  for (int i = 0; i < 100 && !started_ok; i++) {
    started_ok = children_started->load(std::memory_order_acquire);
    if (!started_ok) {
      co_await yield_on_current();
    }
  }
  expect_true(started_ok, "Parent should start and spawn children");
  parent.cancel();
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
}

TEST_CORO(Coro, is_active_returns_true_when_not_cancelled) {
  bool active = co_await is_active();
  expect_true(active, "is_active() should return true when not cancelled");
  co_return td::Unit{};
}

TEST_CORO(Coro, push_down_cancellation_sets_child_cancelled_flag) {
  auto child = []() -> Task<td::Unit> {
    co_await sleep_for(10.0);
    co_return td::Unit{};
  };
  auto t = child().start_in_parent_scope();
  co_await sleep_for(0.01);
  t.cancel();
  auto r = co_await std::move(t).wrap();
  expect_true(r.is_error(), "Child should see cancellation");
  co_return td::Unit{};
}

TEST_CORO(Coro, ensure_active_passes_when_not_cancelled) {
  co_await ensure_active();
  co_return td::Unit{};
}

TEST_CORO(Coro, ensure_active_throws_cancellation_when_cancelled) {
  constexpr int kCancelledCode = td::actor::kCancelledCode;
  auto check_ensure = []() -> Task<td::Unit> {
    co_await sleep_for(10.0);
    co_await ensure_active();
    co_return td::Unit{};
  };
  auto t = check_ensure().start_in_parent_scope();
  co_await sleep_for(0.01);
  t.cancel();
  auto r = co_await std::move(t).wrap();
  expect_true(r.is_error(), "Expected cancellation error");
  expect_eq(r.error().code(), kCancelledCode, "Expected Error(653)");
  co_return td::Unit{};
}

TEST_CORO(Coro, cancel_propagates_to_nested_children) {
  auto grandchild = []() -> Task<td::Unit> {
    co_await sleep_for(10.0);
    co_return td::Unit{};
  };
  auto child = [grandchild]() -> Task<td::Unit> {
    co_await grandchild().start_in_parent_scope();
    co_return td::Unit{};
  };
  auto parent = [child]() -> Task<td::Unit> {
    co_await child().start_in_parent_scope();
    co_return td::Unit{};
  };
  auto t = parent().start_in_parent_scope();
  co_await sleep_for(0.01);
  t.cancel();
  auto r = co_await std::move(t).wrap();
  expect_true(r.is_error(), "Parent should be cancelled");
  co_return td::Unit{};
}

TEST_CORO(Coro, sleep_for_wakes_up_when_scope_is_cancelled) {
  constexpr int kCancelledCode = td::actor::kCancelledCode;
  auto sleeper = []() -> Task<td::Unit> {
    co_await sleep_for(10.0);
    co_return td::Unit{};
  };
  auto t = sleeper().start_in_parent_scope();
  co_await sleep_for(0.01);
  t.cancel();
  auto r = co_await std::move(t).wrap();
  expect_true(r.is_error(), "Sleeper should be cancelled");
  expect_eq(r.error().code(), kCancelledCode, "Expected Error(653)");
  co_return td::Unit{};
}

TEST_CORO(Coro, infinite_wait_cancelled_when_actor_stopped) {
  constexpr int kCancelledCode = td::actor::kCancelledCode;
  class InfiniteWaitActor final : public td::actor::Actor {
   public:
    Task<td::Unit> wait_forever(std::shared_ptr<std::atomic<bool>> started) {
      started->store(true, std::memory_order_release);
      co_await sleep_for(1000.0);
      co_return td::Unit{};
    }
    void request_stop() {
      stop();
    }
  };

  auto started = std::make_shared<std::atomic<bool>>(false);
  auto actor = td::actor::create_actor<InfiniteWaitActor>("InfiniteWaitActor").release();
  auto t = ask(actor, &InfiniteWaitActor::wait_forever, started).start_in_parent_scope();

  bool started_ok = co_await wait_until([&] { return started->load(std::memory_order_acquire); }, 5000);
  expect_true(started_ok, "Infinite wait coroutine should start");
  send_closure(actor, &InfiniteWaitActor::request_stop);

  auto timed_wrap = co_await with_timeout(std::move(t), 0.5).wrap();
  expect_true(timed_wrap.is_ok(), "with_timeout wrapper should complete");
  auto r = timed_wrap.move_as_ok();
  expect_true(r.is_error(), "Stopping actor should end infinite wait with an error");
  expect_eq(r.error().code(), kCancelledCode, "Expected Error(653) from cancellation");
  expect_true(r.error().message().str() != "timeout", "Must be actor-stop cancellation, not watchdog timeout");
  co_return td::Unit{};
}

TEST_CORO(Coro, actor_cancelled_publish_path) {
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
  class DummyActor final : public td::actor::Actor {};

  auto stats = std::make_shared<PublishStats>();
  auto node = make_ref<PublishNode>(stats);

  td::actor::core::ActorState::Flags flags;
  auto actor_info =
      std::make_unique<td::actor::core::ActorInfo>(std::make_unique<DummyActor>(), flags, "DummyActorInfo", 0);

  actor_info->cancel_coro_cancel_nodes();
  bool published = actor_info->publish_coro_cancel_node(*node);
  expect_true(published, "publish should still register node in actor topology");
  expect_eq(stats->cancel_calls.load(std::memory_order_acquire), 1,
            "publish after actor cancel must trigger cancellation");
  expect_eq(stats->cleanup_calls.load(std::memory_order_acquire), 0,
            "cancel callback must not force immediate cleanup");

  bool second_publish = actor_info->publish_coro_cancel_node(*node);
  expect_true(!second_publish, "second publish must report already-linked node");
  expect_eq(stats->cancel_calls.load(std::memory_order_acquire), 1, "double publish must not double-cancel");

  bool unpublished = actor_info->unpublish_coro_cancel_node(*node);
  expect_true(unpublished, "cancelled node should still be unpublishable");
  bool second_unpublish = actor_info->unpublish_coro_cancel_node(*node);
  expect_true(!second_unpublish, "double unpublish should report no-op");

  node.reset();
  expect_eq(stats->destroy_calls.load(std::memory_order_acquire), 1, "node should be destroyed exactly once");
  actor_info->dec_ref();
  actor_info.reset();
  co_return td::Unit{};
}

TEST_CORO(Coro, cancel_does_not_call_on_cancel_after_awaiter_resume) {
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
    if (!done)
      co_await yield_on_current();
  }
  expect_true(done, "Awaiter should complete before cancellation");
  t.cancel();
  auto r = co_await std::move(t).wrap();
  expect_true(r.is_error(), "Expected cancellation");
  expect_true(!test_ref->late_cancel.load(std::memory_order_acquire),
              "on_cancel must not be called after awaiter has resumed");
  co_return td::Unit{};
}

TEST_CORO(Coro, double_publish_does_not_leak_or_double_cleanup) {
  constexpr int kCancelledCode = td::actor::kCancelledCode;
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
  auto t = worker().start_without_scope();
  bool done = false;
  for (int i = 0; i < 100 && !done; i++) {
    done = awaiter_done->load(std::memory_order_acquire);
    if (!done)
      co_await yield_on_current();
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
}

TEST_CORO(Coro, cancellation_propagates_through_ask_to_remote) {
  constexpr int kCancelledCode = td::actor::kCancelledCode;
  class SlowActor final : public td::actor::Actor {
   public:
    Task<int> slow_method(std::shared_ptr<std::atomic<bool>> started, std::shared_ptr<std::atomic<bool>> saw_cancel) {
      started->store(true, std::memory_order_release);
      for (int i = 0; i < 100; i++) {
        if (!(co_await is_active())) {
          saw_cancel->store(true, std::memory_order_release);
          co_return -1;
        }
        co_await sleep_for(0.01);
      }
      co_return 42;
    }
  };
  auto actor = td::actor::create_actor<SlowActor>("SlowActor").release();
  auto started = std::make_shared<std::atomic<bool>>(false);
  auto saw_cancel = std::make_shared<std::atomic<bool>>(false);
  auto t = ask(actor, &SlowActor::slow_method, started, saw_cancel).start_in_parent_scope();
  bool started_ok = co_await wait_until([&] { return started->load(std::memory_order_acquire); }, 5000);
  expect_true(started_ok, "Actor method should start");
  t.cancel();
  auto r = co_await std::move(t).wrap();
  expect_true(r.is_error(), "ask() should return error when cancelled");
  expect_eq(r.error().code(), kCancelledCode, "Expected Error(653)");
  co_return td::Unit{};
}

TEST_CORO(Coro, ask_cancellation_propagates_to_remote_coroutine) {
  constexpr int kCancelledCode = td::actor::kCancelledCode;
  class SleepActor final : public td::actor::Actor {
   public:
    Task<int> slow_method() {
      co_await sleep_for(10.0);
      co_return 42;
    }
  };
  auto actor = td::actor::create_actor<SleepActor>("SleepActor").release();
  auto t = ask(actor, &SleepActor::slow_method).start_in_parent_scope();
  co_await sleep_for(0.01);
  t.cancel();
  auto r = co_await std::move(t).wrap();
  expect_true(r.is_error(), "ask() should return error when cancelled");
  expect_eq(r.error().code(), kCancelledCode, "Expected Error(653)");
  co_return td::Unit{};
}

TEST_CORO(Coro, child_cannot_catch_cancellation_to_prevent_grandchild) {
  constexpr int kCancelledCode = td::actor::kCancelledCode;
  auto child_caught_error = std::make_shared<std::atomic<bool>>(false);
  auto child_continued_after_wrap = std::make_shared<std::atomic<bool>>(false);

  auto grandchild = []() -> Task<int> {
    co_await sleep_for(10.0);
    co_return 42;
  };
  auto child = [grandchild, child_caught_error, child_continued_after_wrap]() -> Task<int> {
    auto gc = grandchild().start_in_parent_scope();
    auto result = co_await std::move(gc).wrap();
    child_continued_after_wrap->store(true, std::memory_order_release);
    if (result.is_error()) {
      child_caught_error->store(true, std::memory_order_release);
      co_return -100;
    }
    co_return result.ok();
  };
  auto parent = [child]() -> Task<int> { co_return co_await child().start_in_parent_scope(); };

  auto task = parent().start_in_parent_scope();
  co_await sleep_for(0.02);
  task.cancel();
  auto result = co_await std::move(task).wrap();
  expect_true(result.is_error(), "Parent should be cancelled");
  expect_eq(result.error().code(), kCancelledCode, "Parent error should be 653");
  expect_true(!child_continued_after_wrap->load(std::memory_order_acquire),
              "Child should NOT continue after .wrap() because it is also cancelled");
  expect_true(!child_caught_error->load(std::memory_order_acquire), "Child should NOT catch the error via .wrap()");
  co_return td::Unit{};
}

TEST_CORO(Coro, wrap_on_child_returns_cancellation_error) {
  constexpr int kCancelledCode = td::actor::kCancelledCode;
  auto inner_cancelled = std::make_shared<std::atomic<bool>>(false);
  auto inner_task = [inner_cancelled]() -> Task<int> {
    co_await sleep_for(10.0);
    co_return 42;
  };
  auto child = [inner_task, inner_cancelled]() -> Task<td::Unit> {
    auto inner = inner_task().start_in_parent_scope();
    auto result = co_await std::move(inner).wrap();
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
}

TEST_CORO(Coro, dropping_StartedTask_cancels_the_task) {
  auto ran_to_completion = std::make_shared<std::atomic<bool>>(false);
  auto worker = [ran_to_completion]() -> Task<int> {
    co_await sleep_for(10.0);
    ran_to_completion->store(true, std::memory_order_release);
    co_return 42;
  };
  {
    auto dropped = worker().start_in_parent_scope();
  }
  co_await sleep_for(0.05);
  expect_true(!ran_to_completion->load(std::memory_order_acquire), "Task should be cancelled, not run to completion");
  co_return td::Unit{};
}

TEST_CORO(Coro, DFS_cancel_completes_in_topological_order) {
  constexpr int kCancelledCode = td::actor::kCancelledCode;
  constexpr int max_i = 300;
  auto mu = std::make_shared<std::mutex>();
  auto order = std::make_shared<std::vector<int>>();

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
  expect_eq(static_cast<int>(order->size()), max_i + 1, "All interior nodes should have completed");

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
}

TEST_CORO(Coro, publish_cancel_promise_fires_on_cancellation) {
  constexpr int kCancelledCode = td::actor::kCancelledCode;
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
}

TEST_CORO(Coro, publish_cancel_promise_no_fire_on_normal_completion) {
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
  expect_true(!was_cancel->load(std::memory_order_acquire), "Should not fire cancellation on normal completion");
  co_return td::Unit{};
}

// =============================================================================
// ignore_cancellation() tests (TEST_CORO)
// =============================================================================

TEST_CORO(Coro, fast_path_cancellation_on_ready_Task) {
  constexpr int kCancelledCode = td::actor::kCancelledCode;
  auto ready_task = []() -> Task<int> { co_return 42; };
  auto outer = [ready_task]() -> Task<td::Unit> {
    auto scope = co_await this_scope();
    auto child = ready_task().start_immediate_in_parent_scope();
    co_await yield_on_current();
    scope.cancel();
    auto v = co_await std::move(child);
    (void)v;
    co_return td::Status::Error("should not reach here");
  };
  auto t = outer().start_in_parent_scope();
  auto r = co_await std::move(t).wrap();
  expect_true(r.is_error(), "expected cancellation error");
  expect_eq(r.error().code(), kCancelledCode, "expected Error(653)");
  co_return td::Unit{};
}

TEST_CORO(Coro, fast_path_cancellation_on_wrap) {
  constexpr int kCancelledCode = td::actor::kCancelledCode;
  auto ready_task = []() -> Task<int> { co_return 42; };
  auto outer = [ready_task]() -> Task<td::Unit> {
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
  expect_true(r.is_error(), "expected cancellation error");
  expect_eq(r.error().code(), kCancelledCode, "expected Error(653)");
  co_return td::Unit{};
}

TEST_CORO(Coro, fast_path_cancellation_on_Result) {
  constexpr int kCancelledCode = td::actor::kCancelledCode;
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
  expect_true(r.is_error(), "expected cancellation error");
  expect_eq(r.error().code(), kCancelledCode, "expected Error(653)");
  co_return td::Unit{};
}

TEST_CORO(Coro, fast_path_cancellation_on_Status) {
  constexpr int kCancelledCode = td::actor::kCancelledCode;
  auto outer = []() -> Task<td::Unit> {
    auto scope = co_await this_scope();
    scope.cancel();
    co_await td::Status::OK();
    co_return td::Status::Error("should not reach here");
  };
  auto t = outer().start_in_parent_scope();
  auto r = co_await std::move(t).wrap();
  expect_true(r.is_error(), "expected cancellation error");
  expect_eq(r.error().code(), kCancelledCode, "expected Error(653)");
  co_return td::Unit{};
}

TEST_CORO(Coro, cancel_before_enter_terminates_task) {
  constexpr int kCancelledCode = td::actor::kCancelledCode;
  auto outer = []() -> Task<td::Unit> {
    auto scope = co_await this_scope();
    scope.cancel();
    auto guard = co_await ignore_cancellation();
    (void)guard;
    co_return td::Status::Error("should not reach here");
  };
  auto t = outer().start_in_parent_scope();
  auto r = co_await std::move(t).wrap();
  expect_true(r.is_error(), "expected cancellation error");
  expect_eq(r.error().code(), kCancelledCode, "expected Error(653)");
  co_return td::Unit{};
}

TEST_CORO(Coro, enter_before_cancel_defers_propagation) {
  constexpr int kCancelledCode = td::actor::kCancelledCode;
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
      scope.cancel();
      bool active = co_await is_active();
      expect_true(active, "is_active() should be true inside guard");
      co_await yield_on_current();
      expect_true(!child_saw_cancel->load(std::memory_order_acquire),
                  "child should not see cancel while guard is active");
    }
    auto r = co_await std::move(started_child).wrap();
    (void)r;
    co_return td::Unit{};
  };
  auto t = outer().start_in_parent_scope();
  auto r = co_await std::move(t).wrap();
  expect_true(r.is_error(), "expected cancellation error after guard drop");
  expect_eq(r.error().code(), kCancelledCode, "expected Error(653)");
  co_return td::Unit{};
}

TEST_CORO(Coro, nested_guards_inner_drop_doesnt_flush) {
  constexpr int kCancelledCode = td::actor::kCancelledCode;
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
        bool active = co_await is_active();
        expect_true(active, "is_active() true in nested guard");
      }
      co_await yield_on_current();
      expect_true(!child_cancelled->load(std::memory_order_acquire),
                  "child should not be cancelled after inner guard drop");
    }
    auto r = co_await std::move(started_child).wrap();
    (void)r;
    co_return td::Unit{};
  };
  auto t = outer().start_in_parent_scope();
  auto r = co_await std::move(t).wrap();
  expect_true(r.is_error(), "expected cancellation error");
  expect_eq(r.error().code(), kCancelledCode, "expected Error(653)");
  co_return td::Unit{};
}

TEST_CORO(Coro, publish_vs_cancel_child_cancelled_exactly_once) {
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
    co_await sleep_for(10.0);
    node->disarm();
    node.reset();
    co_return td::Unit{};
  };
  auto t = outer().start_in_parent_scope();
  co_await sleep_for(0.01);
  t.cancel();
  auto r = co_await std::move(t).wrap();
  expect_true(r.is_error(), "expected cancellation");
  expect_eq(node_ref->cancel_count.load(std::memory_order_acquire), 1, "on_cancel should be called exactly once");
  co_return td::Unit{};
}

TEST_CORO(Coro, IGNORED_bit_doesnt_corrupt_child_count) {
  auto outer = []() -> Task<td::Unit> {
    auto scope = co_await this_scope();
    auto* promise = scope.get_promise();
    auto initial_count = promise->cancellation_.child_count_relaxed_for_test();
    {
      auto guard = co_await ignore_cancellation();
      auto count_in_guard = promise->cancellation_.child_count_relaxed_for_test();
      expect_eq(count_in_guard, initial_count, "child_count unchanged by ignore");
      promise->cancellation_.add_child_ref();
      auto count_with_child = promise->cancellation_.child_count_relaxed_for_test();
      expect_eq(count_with_child, initial_count + 1, "child_count incremented correctly with IGNORED");
      promise->cancellation_.release_child_ref(*promise, CancellationRuntime::ChildReleasePolicy::NoComplete);
    }
    auto final_count = promise->cancellation_.child_count_relaxed_for_test();
    expect_eq(final_count, initial_count, "child_count restored after guard drop");
    co_return td::Unit{};
  };
  co_await outer();
  co_return td::Unit{};
}

TEST_CORO(Coro, is_active_ensure_active_inside_guard) {
  constexpr int kCancelledCode = td::actor::kCancelledCode;
  auto outer = []() -> Task<td::Unit> {
    auto scope = co_await this_scope();
    {
      auto guard = co_await ignore_cancellation();
      scope.cancel();
      bool active = co_await is_active();
      expect_true(active, "is_active() should be true inside guard even after cancel");
      co_await ensure_active();
    }
    bool active = co_await is_active();
    expect_true(!active, "is_active() should be false after guard drop");
    co_await ensure_active();
    co_return td::Status::Error("should not reach here");
  };
  auto t = outer().start_in_parent_scope();
  auto r = co_await std::move(t).wrap();
  expect_true(r.is_error(), "expected cancellation after guard drop");
  expect_eq(r.error().code(), kCancelledCode, "expected Error(653)");
  co_return td::Unit{};
}

TEST_CORO(Coro, regression_basic_cancel_and_await) {
  constexpr int kCancelledCode = td::actor::kCancelledCode;
  auto worker = []() -> Task<int> {
    co_await sleep_for(10.0);
    co_return 42;
  };
  auto t = worker().start_in_parent_scope();
  co_await sleep_for(0.01);
  t.cancel();
  auto r = co_await std::move(t).wrap();
  expect_true(r.is_error(), "expected cancellation");
  expect_eq(r.error().code(), kCancelledCode, "expected Error(653)");
  co_return td::Unit{};
}

// =============================================================================
// Remaining CoroSpec tests converted to TEST_CORO
// =============================================================================

TEST_CORO(Coro, unified_queries) {
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
    check(co_await ask(args...).wrap());
    check(co_await ask_immediate(args...).wrap());
    check_value(co_await ask_immediate(args...));
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
    check_err(co_await ask(args...).wrap());
    check_err(co_await ask_immediate(args...).wrap());

    check_err(co_await
              [](auto&&... args) -> Task<Value> { co_return co_await ask_immediate(args...); }(args...).wrap());
    check_err(co_await
              [](auto&&... args) -> Task<Value> { co_return co_await ask_immediate(args...); }(args...).wrap());
    check_err(co_await [](auto&&... args) -> Task<Value> {
      co_await ask(args...);
      co_return std::make_unique<int>(17);
    }(args...)
                                                 .wrap());

    co_return td::Unit{};
  };

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
  send_closure_immediate(uni, &Uni::get_void, [&](td::Result<td::Unit> r) {
    (void)r;
    done = true;
  });
  CHECK(done);

  co_return td::Unit();
}

TEST_CORO(Coro, awaitable_branches) {
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

TEST_CORO(Coro, recursion) {
  // Direct recursion (free function)
  for (int depth : {5, 10}) {
    int b = co_await rec_fast(depth);
    expect_eq(b, depth, "direct recursion");
  }
  // Recursion via ask hop
  auto actor = td::actor::create_actor<RecTestActor>("RecTestActor");
  for (int depth : {5, 10}) {
    int a = co_await ask(actor, &RecTestActor::rec_slow, depth);
    expect_eq(a, depth, "recursion via ask");
  }
  co_return td::Unit();
}

TEST_CORO(Coro, asks) {
  auto logger = td::actor::create_actor<TestLogger>("TestLogger");
  auto db = td::actor::create_actor<TestDatabase>("TestDatabase", logger.get());

  co_await Yield{};
  for (int i = 0; i < 16; i++) {
    auto immediate = ask_immediate(db, &TestDatabase::square, 4);
    expect_true(immediate.await_ready(), "immediate ask is ready");
    expect_eq(immediate.await_resume().ok(), static_cast<size_t>(16), "immediate ask result");
  }

  auto delayed = ask(db, &TestDatabase::square, 4);
  expect_true(!delayed.await_ready(), "delayed ask is not ready");
  expect_eq(co_await std::move(delayed), static_cast<size_t>(16), "delayed ask result");

  auto user = co_await ask(db, &TestDatabase::get, std::string("user"));
  LOG(INFO) << "User: " << user;

  (void)co_await ask(logger, &TestLogger::log, std::string("unified Task target"));
  (void)co_await ask(logger, &TestLogger::log_promise, std::string("unified Promise target"));
  co_return td::Unit();
}

TEST_CORO(Coro, modifiers) {
  class DummyActor : public td::actor::Actor {};
  auto actor = td::actor::create_actor<DummyActor>("DummyActor");
  auto self = actor.get();
  co_await attach_to_actor(self);

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
      co_await attach_to_actor(self);
      on_self();
    }
    LOG(INFO) << "attach_to_actor (x100) : " << timer.elapsed();
  }

  // Attach to actor and ensure suspended await resumes on same actor
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

TEST_CORO(Coro, concurrency) {
  class DummyActor : public td::actor::Actor {};
  auto actor = td::actor::create_actor<DummyActor>("DummyActor");
  auto self = actor.get();
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
    auto v = co_await std::move(t);
    got += v;
  }
  expect_eq(got, expect, "many parallel sum");
  co_return td::Unit{};
}

TEST_CORO(Coro, concurrency2) {
  for (int rep = 0; rep < 10; rep++) {
    std::vector<StartedTask<int>> shapes;
    for (int i = 0; i < 8000; i++) {
      int m = i % 4;
      if (m == 1) {
        shapes.push_back(spawn_actor("hop1", []() -> Task<int> {
          co_await spawn_actor("sub", []() -> Task<td::Unit> { co_return td::Unit(); }());
          co_return 2;
        }()));
      }
    }
    int s = 0;
    for (auto& t : shapes) {
      auto v = co_await std::move(t);
      s += v;
    }
    LOG(INFO) << "shapes sum: " << s;
  }
  co_return td::Unit();
}

TEST_CORO(Coro, lifecycle) {
  auto make_task = []() -> Task<int> { co_return 7; };

  // Await without explicit start
  {
    auto v = co_await make_task();
    expect_eq(v, 7, "await without start");
  }
  // Explicit start
  {
    auto t = make_task().start_in_parent_scope();
    auto v = co_await std::move(t);
    expect_eq(v, 7, "await after start");
  }
  co_return td::Unit();
}

TEST_CORO(Coro, helpers) {
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

TEST_CORO(Coro, combinators) {
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
  }

  // Test all() with errors and collect_results
  {
    auto success_task = []() -> Task<int> { co_return 42; };
    auto error_task = []() -> Task<int> { co_return td::Status::Error("Test error"); };

    auto tuple = co_await all(success_task().wrap(), error_task().wrap());

    auto s = std::move(std::get<0>(tuple));
    auto e = std::move(std::get<1>(tuple));
    expect_eq(42, s.ok(), "all() with error - success task");
    expect_true(e.is_error(), "all() with error - error task");

    auto tuple2 = co_await all(success_task().wrap(), error_task().wrap());
    auto collected = collect(std::move(tuple2));
    expect_true(collected.is_error(), "collect_results should return error if any task failed");
  }

  // Regression: all_wrap(Result, Result) must compile and keep errors wrapped
  {
    auto tuple = co_await all_wrap(td::Result<int>(11), td::Result<int>(td::Status::Error("wrapped result error")));
    auto first = std::move(std::get<0>(tuple));
    auto second = std::move(std::get<1>(tuple));
    expect_true(first.is_ok(), "all_wrap(Result, Result) keeps first result");
    expect_eq(11, first.move_as_ok(), "all_wrap(Result, Result) keeps ok value");
    expect_true(second.is_error(), "all_wrap(Result, Result) keeps second error");
  }

  // Test collect_results with all successful tasks
  {
    auto task1 = []() -> Task<int> { co_return 1; };
    auto task2 = []() -> Task<int> { co_return 2; };
    auto task3 = []() -> Task<int> { co_return 3; };

    auto tuple = co_await all(task1().wrap(), task2().wrap(), task3().wrap());
    auto collected_tuple = collect(std::move(tuple));
    expect_ok(collected_tuple, "collect_results should succeed when all tasks succeed");
    auto [a, b, c] = collected_tuple.move_as_ok();
    expect_eq(1, a, "First value");
    expect_eq(2, b, "Second value");
    expect_eq(3, c, "Third value");

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
  }

  co_return td::Unit{};
}

TEST_CORO(Coro, try_awaitable) {
  // Success path: unwrap value
  {
    auto ok_task = []() -> Task<int> { co_return 123; };
    int v = co_await ok_task();
    expect_eq(v, 123, "co_try unwraps ok value");
  }

  // Error path: propagate out of outer Task
  {
    auto err_task = []() -> Task<int> { co_return td::Status::Error("boom"); };
    auto r = co_await err_task().wrap();
    expect_true(r.is_error(), "sanity: err_task returns error");

    auto outer = [err_task]() -> Task<int> {
      int x = co_await err_task();
      co_return x + 1;
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
      co_return x + 1;
    }();
    auto result = co_await std::move(outer).wrap();
    expect_true(result.is_error(), "try_unwrap() propagates error from StartedTask");
  }

  // Default co_await StartedTask auto-attaches current scope.
  {
    auto probe_scope = []() -> Task<bool> {
      co_await sleep_for(0.005);
      auto scope = co_await this_scope();
      CHECK(scope);
      co_return scope.get_promise()->cancellation_.has_parent_scope();
    };

    auto outer = [probe_scope]() -> Task<bool> {
      auto started = probe_scope().start_immediate_without_scope();
      co_return co_await std::move(started);
    }();

    auto result = co_await std::move(outer).wrap();
    expect_true(result.is_ok(), "default co_await StartedTask returns value");
    expect_true(result.move_as_ok(), "default co_await StartedTask links parent scope");
  }

  // .unlinked() keeps StartedTask detached from parent scope.
  {
    auto probe_scope = []() -> Task<bool> {
      co_await sleep_for(0.005);
      auto scope = co_await this_scope();
      CHECK(scope);
      co_return scope.get_promise()->cancellation_.has_parent_scope();
    };

    auto outer = [probe_scope]() -> Task<bool> {
      auto started = probe_scope().start_immediate_without_scope();
      co_return co_await std::move(started).unlinked();
    }();

    auto result = co_await std::move(outer).wrap();
    expect_true(result.is_ok(), ".unlinked() StartedTask returns value");
    expect_true(!result.move_as_ok(), ".unlinked() keeps StartedTask without parent scope");
  }

  // Test explicit .child() wrapper for StartedTask
  {
    auto ok_task = []() -> Task<int> { co_return 777; };
    auto outer = [ok_task]() -> Task<int> {
      auto started = ok_task().start_in_parent_scope();
      co_return co_await std::move(started).child();
    }();
    auto result = co_await std::move(outer).wrap();
    expect_true(result.is_ok(), ".child() unwraps value from child StartedTask");
    expect_eq(result.move_as_ok(), 777, ".child() returns correct value");
  }

  // .child() enforces child semantics by attaching unscoped StartedTask when possible.
  {
    auto probe_scope = []() -> Task<bool> {
      co_await sleep_for(0.005);
      auto scope = co_await this_scope();
      CHECK(scope);
      co_return scope.get_promise()->cancellation_.has_parent_scope();
    };

    auto outer = [probe_scope]() -> Task<bool> {
      auto started = probe_scope().start_immediate_without_scope();
      co_return co_await std::move(started).child();
    }();

    auto result = co_await std::move(outer).wrap();
    expect_true(result.is_ok(), ".child() StartedTask returns value");
    expect_true(result.move_as_ok(), ".child() links unscoped StartedTask to parent scope");
  }

  // Test .child().trace(...) path
  {
    auto err_task = []() -> Task<int> { co_return td::Status::Error("child trace test"); };
    auto outer = [err_task]() -> Task<int> {
      auto started = err_task().start_in_parent_scope();
      co_return co_await std::move(started).child().trace("child trace context");
    }();
    auto result = co_await std::move(outer).wrap();
    expect_true(result.is_error(), ".child().trace(...) propagates error");
    auto msg = result.error().message().str();
    expect_true(msg.find("child trace context") != std::string::npos, ".child().trace(...) adds context");
  }

  // Test explicit .unlinked() wrapper for StartedTask
  {
    auto ok_task = []() -> Task<int> { co_return 888; };
    auto started = ok_task().start_immediate_without_scope();
    int v = co_await std::move(started).unlinked();
    expect_eq(v, 888, ".unlinked() unwraps value from StartedTask");
  }

  // Test .unlinked().wrap(...) path
  {
    auto err_task = []() -> Task<int> { co_return td::Status::Error("unlinked wrap test"); };
    auto started = err_task().start_immediate_without_scope();
    auto result = co_await std::move(started).unlinked().wrap();
    expect_true(result.is_error(), ".unlinked().wrap(...) returns error Result");
  }

  // Test trace+wrap mode: trace().wrap()
  {
    auto err_task = []() -> Task<int> { co_return td::Status::Error("trace wrap test 1"); };
    auto started = err_task().start_in_parent_scope();
    auto result = co_await std::move(started).child().trace("trace wrap context 1").wrap();
    expect_true(result.is_error(), "trace().wrap() returns error Result");
    auto msg = result.error().message().str();
    expect_true(msg.find("trace wrap context 1") != std::string::npos, "trace().wrap() adds trace context");
  }

  // Test trace+wrap mode: wrap().trace()
  {
    auto err_task = []() -> Task<int> { co_return td::Status::Error("trace wrap test 2"); };
    auto result = co_await err_task().unlinked().wrap().trace("trace wrap context 2");
    expect_true(result.is_error(), "wrap().trace() returns error Result");
    auto msg = result.error().message().str();
    expect_true(msg.find("trace wrap context 2") != std::string::npos, "wrap().trace() adds trace context");
  }

  // Task.child() preserves Task await semantics
  {
    auto ok_task = []() -> Task<int> { co_return 321; };
    int v = co_await ok_task().child();
    expect_eq(v, 321, "Task.child() preserves normal Task await semantics");
  }

  // Task.unlinked() awaits without parent scope
  {
    auto ok_task = []() -> Task<int> { co_return 654; };
    int v = co_await ok_task().unlinked();
    expect_eq(v, 654, "Task.unlinked() unwraps value");
  }

  // Test co_try() with Result<T> values
  {
    auto outer = []() -> Task<int> {
      td::Result<int> ok_result = 789;
      int x = co_await std::move(ok_result);
      co_return x + 1;
    }();
    auto result = co_await std::move(outer).wrap();
    expect_true(result.is_ok(), "co_try(Result) works with ok value");
    expect_eq(result.move_as_ok(), 790, "co_try(Result) returns correct value");
  }

  // Test co_try() error propagation with Result<T>
  {
    auto outer = []() -> Task<int> {
      td::Result<int> err_result = td::Status::Error("direct error");
      int x = co_await std::move(err_result);
      co_return x + 1;
    }();
    auto result = co_await std::move(outer).wrap();
    expect_true(result.is_error(), "co_try(Result) propagates error");
  }

  // Test co_try() with Result<T> lvalue reference
  {
    auto outer = []() -> Task<int> {
      td::Result<int> ok_result = 999;
      int x = co_await std::move(ok_result);
      co_return x + 2;
    }();
    auto result = co_await std::move(outer).wrap();
    expect_true(result.is_ok(), "co_try(Result&) works with lvalue reference");
    expect_eq(result.move_as_ok(), 1001, "co_try(Result&) returns correct value");
  }

  // Test default Result co_await (propagates errors)
  {
    auto outer = []() -> Task<int> {
      td::Result<int> ok_result = 333;
      int x = co_await std::move(ok_result);
      co_return x * 2;
    }();
    auto result = co_await std::move(outer).wrap();
    expect_true(result.is_ok(), "Result default co_await works with ok value");
    expect_eq(result.move_as_ok(), 666, "Result default co_await returns correct value");
  }

  // Test default Result co_await error propagation
  {
    auto outer = []() -> Task<int> {
      td::Result<int> err_result = td::Status::Error("unwrap error");
      int x = co_await std::move(err_result);
      co_return x * 2;
    }();
    auto result = co_await std::move(outer).wrap();
    expect_true(result.is_error(), "Result default co_await propagates error");
  }

  // Test Result::wrap() to prevent error propagation
  {
    auto outer = []() -> Task<td::Result<int>> {
      td::Result<int> err_result = td::Status::Error("wrapped error");
      auto full_result = co_await std::move(err_result).wrap();
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
      auto full_result = co_await std::move(ok_result).wrap();
      expect_true(full_result.is_ok(), "wrap() preserves ok value in Result");
      co_return full_result;
    }();
    auto result = co_await std::move(outer).wrap();
    expect_true(result.is_ok(), "Task completes successfully");
    auto inner_result = result.move_as_ok();
    expect_true(inner_result.is_ok(), "wrap() preserved the ok value");
    expect_eq(inner_result.move_as_ok(), 555, "wrap() preserved the correct value");
  }

  // Test Task default co_await (propagates errors)
  {
    auto inner = []() -> Task<int> { co_return 888; };
    auto outer = [inner]() -> Task<int> {
      int x = co_await inner();
      co_return x + 1;
    }();
    auto result = co_await std::move(outer).wrap();
    expect_true(result.is_ok(), "Task default co_await works");
    expect_eq(result.move_as_ok(), 889, "Task default co_await returns correct value");
  }

  // Test Task default co_await error propagation
  {
    auto inner = []() -> Task<int> { co_return td::Status::Error("task error"); };
    auto outer = [inner]() -> Task<int> {
      int x = co_await inner();
      co_return x + 1;
    }();
    auto result = co_await std::move(outer).wrap();
    expect_true(result.is_error(), "Task default co_await propagates error");
  }

  // Test Task::wrap() to prevent error propagation
  {
    auto inner = []() -> Task<int> { co_return td::Status::Error("wrapped task error"); };
    auto outer = [inner]() -> Task<td::Result<int>> {
      auto full_result = co_await inner().wrap();
      expect_true(full_result.is_error(), "Task::wrap() preserves error");
      co_return full_result;
    }();
    auto result = co_await std::move(outer).wrap();
    expect_true(result.is_ok(), "Outer task completes successfully");
    auto inner_result = result.move_as_ok();
    expect_true(inner_result.is_error(), "Task::wrap() preserved the error");
  }

  co_return td::Unit{};
}

TEST_CORO(Coro, test_trace) {
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
      auto v = co_await std::move(child).trace("should not matter");
      (void)v;
      co_return td::Status::Error("should not reach here");
    };
    auto t = outer().start_in_parent_scope();
    auto r = co_await std::move(t).wrap();
    expect_true(r.is_error(), "trace Task cancellation: expected error");
    expect_eq(r.error().code(), kCancelledCode, "trace Task cancellation: expected cancelled code");
  }

  // Test fast-path cancellation on .trace() with ready Task (inline)
  {
    constexpr int kCancelledCode = td::actor::kCancelledCode;
    auto outer = []() -> Task<td::Unit> {
      auto scope = co_await this_scope();
      scope.cancel();
      auto ready_task = []() -> Task<int> { co_return 42; };
      auto v = co_await ready_task().trace("should not matter");
      (void)v;
      co_return td::Status::Error("should not reach here");
    };
    auto t = outer().start_in_parent_scope();
    auto r = co_await std::move(t).wrap();
    expect_true(r.is_error(), "trace inline Task cancellation: expected error");
    expect_eq(r.error().code(), kCancelledCode, "trace inline Task cancellation: expected cancelled code");
  }

  co_return td::Unit{};
}

TEST_CORO(Coro, stop_actor) {
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
      co_await std::move(task);
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

TEST_CORO(Coro, promise_destroy_in_mailbox) {
  class Target : public td::actor::Actor {
   public:
    explicit Target(StartedTask<int>::ExternalPromise p) : promise_(std::move(p)) {
    }
    void start_up() override {
      send_closure_later(actor_id(this), &Target::receive_promise, std::move(promise_));
      stop();
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

TEST_CORO(Coro, promise_destroy_in_actor_member) {
  class Target : public td::actor::Actor {
   public:
    explicit Target(StartedTask<int>::ExternalPromise p) : promise_(std::move(p)) {
    }
    void start_up() override {
      stop();
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

TEST_CORO(Coro, co_return_empty_braces) {
  // Test co_return {}; in Task<Unit>
  auto test_task = []() -> Task<td::Unit> { co_return {}; };
  auto result = co_await test_task().wrap();
  expect_true(result.is_ok(), "co_return {}; should succeed for Task<Unit>");

  auto test_task2 = []() -> Task<td::Unit> { co_return td::Unit{}; };
  auto result2 = co_await test_task2().wrap();
  expect_true(result2.is_ok(), "co_return td::Unit{}; should succeed");

  struct SimpleStruct {
    int a;
    int b;
  };
  auto test_designated = []() -> Task<SimpleStruct> { co_return {.a = 1, .b = 2}; };
  auto result3 = co_await test_designated().wrap();
  expect_true(result3.is_ok(), "co_return {.a=1, .b=2}; should succeed");
  expect_eq(result3.ok().a, 1, "designated init .a");
  expect_eq(result3.ok().b, 2, "designated init .b");

  auto test_brace = []() -> Task<SimpleStruct> { co_return {10, 20}; };
  auto result4 = co_await test_brace().wrap();
  expect_true(result4.is_ok(), "co_return {10, 20}; should succeed");
  expect_eq(result4.ok().a, 10, "brace init .a");
  expect_eq(result4.ok().b, 20, "brace init .b");

  co_return td::Unit{};
}

TEST_CORO(Coro, actor_ref_uaf) {
  class UafActor : public td::actor::Actor {
   public:
    int member_value = 42;

    ~UafActor() {
      LOG(INFO) << "~UafActor: zeroing member_value (was " << member_value << ")";
      member_value = 0;
    }

    void start_up() override {
      alarm_timestamp() = td::Timestamp::in(0.05);
    }
    void alarm() override {
      LOG(INFO) << "UafActor stopping";
      stop();
    }

    Task<int> query_with_scope_exit() {
      SCOPE_EXIT {
        LOG(INFO) << "SCOPE_EXIT: accessing member_value = " << this->member_value;
        LOG_CHECK(this->member_value == 42) << "UAF detected in SCOPE_EXIT!";
      };
      CHECK(member_value == 42);

      auto task = []() -> Task<td::Unit> {
        td::usleep_for(200000);
        co_return td::Unit{};
      }();
      task.set_executor(Executor::on_scheduler());
      co_await std::move(task);

      LOG_CHECK(member_value == 42) << "UAF detected after sleep!";
      co_return member_value;
    }
  };

  auto a = create_actor<UafActor>("UafActor");
  auto r = co_await ask(a, &UafActor::query_with_scope_exit).wrap();
  if (r.is_error()) {
    LOG(INFO) << "Got expected error: " << r.error();
  } else {
    LOG(INFO) << "Unexpected success, value = " << r.ok();
  }
  co_return td::Unit{};
}

TEST_CORO(Coro, sleep_for) {
  auto start = td::Timestamp::now();
  co_await sleep_for(0.1);
  auto elapsed = td::Timestamp::now().at() - start.at();
  expect_true(elapsed >= 0.09, "sleep_for should wait at least 90ms");
  expect_true(elapsed < 0.2, "sleep_for should not wait too long");

  // Regression: immediate-ready path
  auto immediate_start = td::Timestamp::now();
  for (int i = 0; i < 1000; i++) {
    co_await sleep_until(td::Timestamp::at(0));
  }
  auto immediate_elapsed = td::Timestamp::now().at() - immediate_start.at();
  expect_true(immediate_elapsed < 0.2, "Immediate-ready sleep should complete quickly");

  co_return td::Unit{};
}

TEST_CORO(Coro, coro_mutex) {
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
    tasks.push_back(ask(actor, &MutexActor::critical_section).start_in_parent_scope());
  }
  for (auto& t : tasks) {
    co_await std::move(t);
  }
  co_return td::Unit{};
}

TEST_CORO(Coro, coro_coalesce) {
  class CoalesceActor : public td::actor::Actor {
   public:
    Task<int> query(int x) {
      co_return co_await coalesce_.run(x, [this, x]() -> Task<int> {
        computation_count_++;
        co_await coro_sleep(td::Timestamp::in(0.1));
        co_return x * 2;
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
  constexpr int num_queries = 10;
  std::vector<StartedTask<int>> tasks;
  for (int i = 0; i < num_queries; i++) {
    tasks.push_back(ask(actor, &CoalesceActor::query, 21).start_in_parent_scope());
  }
  for (auto& t : tasks) {
    int result = co_await std::move(t);
    expect_eq(result, 42, "Result should be 21*2=42");
  }
  auto count = co_await ask_immediate(actor, &CoalesceActor::get_computation_count);
  expect_eq(count, 1, "Should have computed only once");
  co_return td::Unit{};
}

TEST_CORO(Coro, actor_task_unwrap_bug) {
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
      std::vector<td::actor::StartedTask<>> tasks;
      tasks.push_back(td::actor::ask(b_, &B::run).start_in_parent_scope());
      co_await td::actor::all(std::move(tasks));
      co_return {};
    }
    td::actor::ActorOwn<B> b_;
  };

  td::actor::create_actor<A>("A").release();
  co_await coro_sleep(td::Timestamp::in(3.0));
  co_return td::Unit{};
}

TEST_CORO(Coro, cancellation_parent_scope_lease) {
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

  // Test 2: ParentScopeLease promise reports cancellation correctly
  co_await []() -> Task<td::Unit> {
    auto scope = co_await this_scope();
    auto* promise = scope.get_promise();
    CHECK(promise);

    auto handle = current_scope_lease();
    expect_true(handle && !handle.is_cancelled(), "Handle should not be cancelled initially");
    promise->cancel();
    expect_true(handle && handle.is_cancelled(), "Handle should see cancellation");
    co_return td::Unit{};
  }();

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

  co_return td::Unit{};
}

TEST_CORO(Coro, task_cancellation_source) {
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

  co_return td::Unit{};
}

TEST_CORO(Coro, cancellation_serious_stress) {
  constexpr int kCancelledCode = td::actor::kCancelledCode;
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
      LOG(INFO) << "cancellation_serious_stress progress: " << (iter + 1) << "/" << kIterations;
    }
  }

  LOG_CHECK(stats.started_tasks == stats.completed_ok + stats.completed_err653)
      << "serious stress: completion mismatch started=" << stats.started_tasks << " ok=" << stats.completed_ok
      << " err653=" << stats.completed_err653;

  auto elapsed = td::Timestamp::now().at() - started_at.at();
  LOG(INFO) << "cancellation_serious_stress stats: started=" << stats.started_tasks
            << " cancel_calls=" << stats.cancel_calls << " ok=" << stats.completed_ok
            << " err653=" << stats.completed_err653 << " elapsed=" << elapsed << "s";
  co_return td::Unit{};
}

TEST_CORO(Coro, scheduled_sleep_cancel_stress) {
  constexpr int kCancelledCode = td::actor::kCancelledCode;
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
  co_return td::Unit{};
}

TEST_CORO(Coro, scope_exit_timing) {
  struct Logger {
    const char* name;
    Logger(const char* n) : name(n) {
    }
    ~Logger() {
      LOG(INFO) << "  [" << name << "] DESTROYED";
    }
  };

  auto test_coro = []() -> Task<int> {
    Logger local("local_in_coro");
    co_return 42;
  };

  auto result = co_await test_coro();
  expect_eq(result, 42, "scope_exit_timing result");
  co_return td::Unit{};
}

TEST_CORO(Coro, with_timeout) {
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

  // Test 2: Timeout fires and cancels task
  co_await []() -> Task<td::Unit> {
    auto slow_task = []() -> Task<int> {
      co_await sleep_for(10.0);
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

  co_return td::Unit{};
}

TEST_CORO(Coro, external_parent_scope_repro) {
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
  co_return td::Unit{};
}

// 5) Runner
int main(int argc, char** argv) {
  SET_VERBOSITY_LEVEL(VERBOSITY_NAME(INFO));

  auto& runner = td::TestsRunner::get_default();
  for (int i = 1; i < argc; i++) {
    auto arg = std::string_view(argv[i]);
    if (arg == "--filter") {
      CHECK(i + 1 < argc);
      runner.add_substr_filter(std::string(argv[++i]));
    }
  }
  runner.run_all();
  return runner.any_test_failed() ? 1 : 0;
}
