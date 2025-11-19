#pragma once

#include <atomic>
#include <coroutine>
#include <memory>
#include <type_traits>
#include <utility>

#include "td/actor/PromiseFuture.h"
#include "td/actor/coro_awaitables.h"
#include "td/actor/coro_executor.h"
#include "td/actor/coro_types.h"
#include "td/utils/Status.h"

namespace td::actor {

namespace detail {

struct TaskStateManagerData {
  Executor executor{Executor::on_default()};
  std::atomic<uint8_t> flags{0};
  std::coroutine_handle<> continuation{};

  enum Flags : uint8_t {
    READY_FLAG = 1,
    STARTED_FLAG = 2,
    SUSPEND_FLAG = 4,
    DETACH_FLAG = 8,
  };

  uint8_t set_flag(uint8_t new_flag) noexcept {
    auto old_flags = flags.fetch_or(new_flag, std::memory_order_acq_rel);
    CHECK((old_flags & new_flag) == 0);
    return old_flags;
  }

  [[nodiscard]] std::coroutine_handle<> on_ready(std::coroutine_handle<> self_handle) {
    auto old_flags = set_flag(READY_FLAG);

    if (!(old_flags & STARTED_FLAG)) {
      return continuation;
    }

    std::coroutine_handle<> next = std::noop_coroutine();
    if (old_flags & SUSPEND_FLAG) {
      next = continuation;
    }
    if (old_flags & DETACH_FLAG) {
      self_handle.destroy();
    }
    return next;
  }

  void set_executor(Executor new_executor) noexcept {
    executor = std::move(new_executor);
  }
};

template <class P>
struct TaskStateManager {
  using Data = TaskStateManagerData;
  using Coro = std::coroutine_handle<>;
  using Self = std::coroutine_handle<P>;
  Data* data;

  void set_executor(Executor executor) noexcept {
    data->executor = std::move(executor);
  }

  void start(Self self) {
    set_is_started();
    data->executor.schedule(self);
  }
  void start_immediate(Self self) {
    set_is_started();
    data->executor.execute_or_schedule(self).resume();
  }
  void start_external() {
    set_is_started();
  }

  bool is_ready() const noexcept {
    return false;
  }

  [[nodiscard]] std::coroutine_handle<> on_suspend_and_start(Self self, Coro continuation) {
    data->continuation = continuation;
    return data->executor.execute_or_schedule(self);
  }

  void on_detach(Self self) {
    self.destroy();
  }

 private:
  void set_is_started() {
    data->flags.fetch_or(Data::STARTED_FLAG, std::memory_order_relaxed);
  }
};

template <class P>
struct StartedTaskStateManager {
  using Data = TaskStateManagerData;
  using Coro = std::coroutine_handle<>;
  using Self = std::coroutine_handle<P>;
  Data* data;

  bool is_ready() const noexcept {
    return data->flags.load(std::memory_order_acquire) & Data::READY_FLAG;
  }

  [[nodiscard]] std::coroutine_handle<> on_suspend(Coro new_continuation) {
    CHECK(!data->continuation);
    data->continuation = new_continuation;
    auto old_flags = data->set_flag(Data::SUSPEND_FLAG);
    return (old_flags & Data::READY_FLAG) ? new_continuation : std::noop_coroutine();
  }

  void on_detach(Self self) {
    auto old_flags = data->set_flag(Data::DETACH_FLAG);
    if (old_flags & Data::READY_FLAG) {
      self.destroy();
    }
  }
};

}  // namespace detail

struct promise_common {
  detail::TaskStateManagerData state_manager_data;
};

template <class ResultT>
struct promise_value : promise_common {
  [[no_unique_address]] ResultT result;

  template <class TT>
  void return_value(TT&& v) noexcept {
    result = std::forward<TT>(v);
  }

  struct ExternalResult {};
  void return_value(ExternalResult&&) noexcept {
  }

  void unhandled_exception() noexcept {
    result = td::Status::Error("unhandled exception in coroutine");
  }

  ResultT extract_result() noexcept {
    return std::move(result);
  }
};

template <class T>
struct Task;

template <class T>
struct StartedTask;

template <class T>
struct promise_type : promise_value<td::Result<T>> {
  static_assert(!std::is_void_v<T>, "Task<void> is not supported; use Task<Unit> instead");
  using Handle = std::coroutine_handle<promise_type>;

  auto self() noexcept {
    return Handle::from_promise(*this);
  }

  template <class TT>
  void external_return_value(TT&& v) noexcept
    requires(!std::is_void_v<T>)
  {
    route_finish(std::forward<TT>(v)).resume();
  }

  Task<T> get_return_object() noexcept {
    return Task<T>{Handle::from_promise(*this)};
  }

  std::suspend_always initial_suspend() noexcept {
    return {};
  }

  auto final_suspend() noexcept {
    struct Final {
      bool await_ready() noexcept {
        return false;
      }
      std::coroutine_handle<> await_suspend(Handle self) noexcept {
        return self.promise().state_manager_data.on_ready(self);
      }
      void await_resume() noexcept {
      }
    };
    return Final{};
  }

  auto await_transform(detail::YieldOn y) {
    this->state_manager_data.set_executor(y.executor);
    return y;
  }
  auto await_transform(detail::ResumeOn y) {
    this->state_manager_data.set_executor(y.executor);
    return y;
  }

  template <class Aw>
  auto await_transform(SkipAwaitTransform<Aw> wrapped_aw) noexcept {
    return std::move(wrapped_aw.awaitable);
  }

  auto await_transform(Yield) noexcept {
    return yield_on(this->state_manager_data.executor);
  }

  auto await_transform(std::suspend_always) noexcept {
    return std::suspend_always{};
  }

  auto await_transform(std::suspend_never) noexcept {
    return std::suspend_always{};
  }

  template <class U>
  auto await_transform(td::ResultUnwrap<U> wrapped) noexcept {
    return await_transform(std::move(wrapped.result));
  }
  template <class U>
  auto await_transform(td::ResultWrap<U> wrapped) noexcept {
    return await_transform(Wrapped<Result<U>>{std::move(wrapped.result)});
  }

  template <class U>
  auto await_transform(Wrapped<td::Result<U>> wrapped) noexcept {
    return result_awaiter_wrap(std::move(wrapped.value));
  }

  template <class U>
  auto await_transform(td::Result<U>&& result) noexcept {
    return result_awaiter_unwrap(std::move(result));
  }
  auto await_transform(td::Status&& status) noexcept {
    td::Result<td::Unit> res;
    if (status.is_ok()) {
      res = td::Result<td::Unit>{td::Unit{}};
    } else {
      res = std::move(status);
    }
    return await_transform(std::move(res));
  }

  template <class U>
  auto await_transform(Task<U>&& task) noexcept {
    return unwrap_and_resume_on_current(std::move(task).start_immediate());
  }
  template <class U>
  auto await_transform(StartedTask<U>&& task) noexcept {
    return unwrap_and_resume_on_current(std::move(task));
  }

  template <class U>
  auto await_transform(Wrapped<Task<U>>&& wrapped) noexcept {
    return wrap_and_resume_on_current(std::move(wrapped.value));
  }
  template <class U>
  auto await_transform(Wrapped<StartedTask<U>>&& wrapped) noexcept {
    return wrap_and_resume_on_current(std::move(wrapped.value));
  }

  template <class Aw>
  auto await_transform(Aw&& aw) noexcept {
    return wrap_and_resume_on_current(std::forward<Aw>(aw));
  }

  // API used by TaskWrapAwaiter and TaskUnwrapAwaiter
  bool is_immediate_execution_always_allowed() const noexcept {
    return this->state_manager_data.executor.is_immediate_execution_always_allowed();
  }

  std::coroutine_handle<> route_resume() {
    return this->state_manager_data.executor.resume_or_schedule(self());
  }

  std::coroutine_handle<> route_finish(td::Result<T> r) {
    this->return_value(std::move(r));
    return final_suspend().await_suspend(self());
  }
};

template <class T>
struct [[nodiscard]] Task {
  using value_type = T;

  using promise_type = promise_type<T>;
  using Handle = std::coroutine_handle<promise_type>;
  Handle h{};

  Task() = default;
  explicit Task(Handle hh) : h(hh) {
  }
  Task(Task&& o) noexcept : h(std::exchange(o.h, {})) {
  }
  Task& operator=(Task&& o) = delete;
  Task(const Task&) = delete;
  Task& operator=(const Task&) = delete;

  auto sm() {
    return detail::TaskStateManager<promise_type>{&h.promise().state_manager_data};
  }

  ~Task() noexcept {
    detach();
  }
  void detach() {
    if (!h) {
      return;
    }
    sm().on_detach(h);
    h = {};
  }

  auto start() && {
    sm().start(h);
    return StartedTask<T>{std::exchange(h, {})};
  }
  auto start_immediate() && {
    sm().start_immediate(h);
    return StartedTask<T>{std::exchange(h, {})};
  }
  auto start_external() && {
    sm().start_external();
    return StartedTask<T>{std::exchange(h, {})};
  }
  void set_executor(Executor new_executor) {
    CHECK(h);
    sm().set_executor(std::move(new_executor));
  }

  constexpr bool await_ready() noexcept {
    return sm().is_ready();
  }

  std::coroutine_handle<> await_suspend(std::coroutine_handle<> continuation) noexcept {
    CHECK(h);
    return sm().on_suspend_and_start(h, continuation);
  }
  td::Result<T> await_resume() noexcept {
    CHECK(h);
    return h.promise().extract_result();
  }

  const td::Result<T>& await_resume_peek() const noexcept {
    CHECK(h);
    return h.promise().result;
  }

  auto wrap() && {
    return Wrapped<Task>{std::move(*this)};
  }
};

template <class T>
struct [[nodiscard]] StartedTask {
  using value_type = T;

  using promise_type = promise_type<T>;
  using Handle = std::coroutine_handle<promise_type>;
  Handle h{};

  auto sm() {
    CHECK(h);
    return detail::StartedTaskStateManager<promise_type>{&h.promise().state_manager_data};
  }
  StartedTask() = default;
  explicit StartedTask(Handle hh) : h(hh) {
    CHECK(h);
  }
  StartedTask(StartedTask&& o) noexcept : h(std::exchange(o.h, {})) {
  }
  StartedTask& operator=(StartedTask&& o) = delete;
  StartedTask(const StartedTask&) = delete;
  StartedTask& operator=(const StartedTask&) = delete;

  ~StartedTask() noexcept {
    detach();
  }
  void detach() {
    if (!h) {
      return;
    }
    sm().on_detach(h);
    h = {};
  }
  bool await_ready() noexcept {
    return sm().is_ready();
  }

  std::coroutine_handle<> await_suspend(std::coroutine_handle<> continuation) noexcept {
    return sm().on_suspend(continuation);
  }
  td::Result<T> await_resume() noexcept {
    return h.promise().extract_result();
  }

  const td::Result<T>& await_resume_peek() const noexcept {
    CHECK(h);
    return h.promise().result;
  }

  auto wrap() && {
    return Wrapped<StartedTask>{std::move(*this)};
  }

  template <class F>
  auto then(F&& f) && {
    using Self = StartedTask<T>;
    using FDecayed = std::decay_t<F>;
    using Awaitable = decltype(detail::make_awaitable(std::declval<FDecayed&>()(std::declval<T&&>())));
    using Ret = decltype(std::declval<Awaitable&>().await_resume());
    using U = std::conditional_t<TDResultLike<Ret>, decltype(std::declval<Ret&&>().move_as_ok()), Ret>;
    return [](Self task, FDecayed fn) mutable -> Task<U> {
      co_await become_lightweight();
      auto value = co_await std::move(task);
      co_return co_await detail::make_awaitable(fn(std::move(value)));
    }(std::move(*this), std::forward<F>(f));
  }

  struct ExternalPromise : public PromiseInterface<T> {
    ExternalPromise() = default;
    explicit ExternalPromise(promise_type* p) : promise(p) {
    }
    void set_value(T&& value) override {
      promise.release()->external_return_value(std::move(value));
    }
    void set_error(Status&& error) override {
      promise.release()->external_return_value(std::move(error));
    }

    operator bool() const {
      return bool(promise);
    }

    struct Deleter {
      void operator()(promise_type* p) {
        p->external_return_value(td::Status::Error("promise destroyed"));
      }
    };
    std::unique_ptr<promise_type, Deleter> promise{};
  };

  static std::pair<StartedTask, ExternalPromise> make_bridge() {
    auto task = []() -> Task<T> { co_return typename promise_type::ExternalResult{}; }();
    task.set_executor(Executor::on_scheduler());
    auto promise = ExternalPromise(&task.h.promise());
    auto started_task = std::move(task).start_external();
    return std::make_pair(std::move(started_task), std::move(promise));
  }
};

template <class P, class T>
void custom_connect(P&& p, StartedTask<T>&& mt) noexcept {
  if (mt.await_ready()) {
    connect(std::move(p), mt.await_resume());
    return;
  }
  [](auto promise, auto mt) mutable -> detail::FireAndForget {
    auto result = co_await mt;
    connect(std::move(promise), std::move(result));
  }(std::forward<P>(p), std::move(mt));
}

template <class P, class T>
void custom_connect(P&& p, Task<T>&& t) noexcept {
  connect(std::forward<P>(p), std::move(t).start_immediate());
}

}  // namespace td::actor
