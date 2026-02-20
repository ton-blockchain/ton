#pragma once

#include <atomic>
#include <coroutine>
#include <cstdint>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

#include "td/actor/PromiseFuture.h"
#include "td/actor/coro_awaitables.h"
#include "td/actor/coro_cancellation_runtime.h"
#include "td/actor/coro_executor.h"
#include "td/actor/coro_ref.h"
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

  struct ReadyResult {
    std::coroutine_handle<> continuation;
    bool should_dec_ref;
    bool should_notify_parent;
  };

  // Returns completion routing and ownership transitions for finalization.
  // First caller that marks READY wins; duplicate callers receive no-op work.
  [[nodiscard]] ReadyResult on_ready() {
    auto old_flags = flags.fetch_or(READY_FLAG, std::memory_order_acq_rel);
    if (old_flags & READY_FLAG) {
      return {std::noop_coroutine(), false, false};
    }

    if (!(old_flags & STARTED_FLAG)) {
      return {continuation, false, true};
    }

    std::coroutine_handle<> next = std::noop_coroutine();
    if (old_flags & SUSPEND_FLAG) {
      next = continuation;
    }
    // Always dec_ref when started - the coroutine's own reference
    return {next, true, true};
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
    detail::resume_on_current_tls(data->executor.execute_or_schedule(self));
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
    self.promise().dec_ref();
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
    data->set_flag(Data::DETACH_FLAG);
    // Always dec_ref - StartedTask is releasing its reference
    self.promise().dec_ref();
  }
};

}  // namespace detail

struct promise_common;

struct promise_common : CancelNode {
  using CancelNode::add_ref;
  using CancelNode::dec_ref;

  detail::TaskStateManagerData state_manager_data;
  CancellationRuntime cancellation_{};
  std::coroutine_handle<> self_handle_{};

  void on_cancel() override {
    cancellation_.cancel(*this);
  }
  void on_cleanup() override {
    dec_ref();
  }
  void on_publish() override {
    add_ref();
  }
  void on_zero_refs() override {
    cancellation_.on_last_ref_teardown();
    self_handle_.destroy();
  }

  bool is_cancelled() const {
    return cancellation_.is_cancelled();
  }

  bool should_finish_due_to_cancellation() const {
    return cancellation_.should_finish_due_to_cancellation();
  }

  void cancel() {
    // Cancellation walk and timer cancel callbacks require scheduler context.
    CHECK(core::SchedulerContext::get_ptr());
    cancellation_.cancel(*this);
  }

  std::pair<std::coroutine_handle<>, bool> mark_done() {
    auto ready = state_manager_data.on_ready();
    if (ready.should_notify_parent) {
      cancellation_.notify_parent_child_completed();
    }
    return {ready.continuation, ready.should_dec_ref};
  }

  std::coroutine_handle<> complete_inline() {
    auto [continuation, should_dec_ref] = mark_done();
    if (should_dec_ref) {
      dec_ref();
    }
    return continuation;
  }

  std::coroutine_handle<> complete_scheduled() {
    auto [continuation, should_dec_ref] = mark_done();
    // Schedule continuation BEFORE dec_ref (dec_ref might destroy frame)
    if (continuation && continuation != std::noop_coroutine()) {
      detail::SchedulerExecutor{}.schedule(continuation);
    }
    if (should_dec_ref) {
      dec_ref();
    }
    return std::noop_coroutine();
  }
};

// Token encoding uses bottom 2 bits, so promise_common must be 4-byte aligned
static_assert(alignof(promise_common) >= 4, "promise_common must be 4-byte aligned for token encoding");

namespace bridge {
inline CancellationRuntime& runtime(promise_common& self) {
  return self.cancellation_;
}

inline const CancellationRuntime& runtime(const promise_common& self) {
  return self.cancellation_;
}

inline std::coroutine_handle<> complete_scheduled(promise_common& self) {
  return self.complete_scheduled();
}

inline CancelNode& cancel_node(promise_common& self) {
  return self;
}

inline bool should_finish_due_to_cancellation(const promise_common& self) {
  return self.should_finish_due_to_cancellation();
}
}  // namespace bridge

inline ParentScopeLease current_scope_lease() {
  auto* p = detail::get_current_promise();
  if (!p) {
    return ParentScopeLease{};
  }
  return bridge::make_parent_scope_lease(*p);
}

class CancelScope {
 public:
  CancelScope() = default;
  explicit CancelScope(promise_common* promise) : promise_(promise) {
  }

  bool is_cancelled() const {
    return promise_ && promise_->is_cancelled();
  }

  void cancel() {
    if (promise_) {
      promise_->cancel();
    }
  }

  explicit operator bool() const {
    return promise_ != nullptr;
  }

  promise_common* get_promise() const {
    return promise_;
  }

 private:
  promise_common* promise_{nullptr};
};

// Markers for co_await
struct ThisScope {};
struct IsActive {};
struct EnsureActive {};
struct IgnoreCancellation {};

inline ThisScope this_scope() {
  return {};
}

inline IsActive is_active() {
  return {};
}

inline EnsureActive ensure_active() {
  return {};
}

inline IgnoreCancellation ignore_cancellation() {
  return {};
}

struct CancellationGuard {
  promise_common* promise_{};
  CancellationGuard() = default;
  explicit CancellationGuard(promise_common* p) : promise_(p) {
  }
  ~CancellationGuard() {
    if (promise_)
      promise_->cancellation_.leave_ignore(*promise_);
  }
  CancellationGuard(CancellationGuard&& o) noexcept : promise_(std::exchange(o.promise_, nullptr)) {
  }
  CancellationGuard& operator=(CancellationGuard&&) = delete;
};

// Tags for connect execution mode
struct Immediate {};
struct Lazy {};

template <class ResultT>
struct promise_value : promise_common {
  [[no_unique_address]] ResultT result;

  template <class TT>
  void return_value(TT&& v) noexcept {
    result = std::forward<TT>(v);
  }

  struct ExternalResult {
    explicit ExternalResult() = default;
  };
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

namespace detail {

template <class T>
struct IsTaskAwaitable : std::false_type {};

template <class T>
struct IsTaskAwaitable<Task<T>> : std::true_type {};

template <class T>
struct IsTaskAwaitable<StartedTask<T>> : std::true_type {};

template <class T>
inline constexpr bool IsTaskAwaitableV = IsTaskAwaitable<std::remove_cvref_t<T>>::value;

}  // namespace detail

template <class T>
struct promise_type : promise_value<td::Result<T>> {
  static_assert(!std::is_void_v<T>, "Task<void> is not supported; use Task<Unit> instead");
  using Handle = std::coroutine_handle<promise_type>;

  // Bring base class return_value overloads into scope
  using promise_value<td::Result<T>>::return_value;

  // Allow co_return {}; to work by constructing T{} (e.g., Unit{})
  // This fixes a bug where co_return {}; was equivalent to co_return td::Status::Error(-1);
  void return_value(T v) noexcept {
    this->result = std::move(v);
  }

  auto self() noexcept {
    return Handle::from_promise(*this);
  }

  template <class TT>
  void external_return_value(TT&& v) noexcept
    requires(!std::is_void_v<T>)
  {
    detail::resume_on_current_tls(route_finish(std::forward<TT>(v)));
  }

  Task<T> get_return_object() noexcept {
    auto h = Handle::from_promise(*this);
    this->self_handle_ = h;  // Store for TLS-aware scheduling
    return Task<T>{h};
  }

  std::suspend_always initial_suspend() noexcept {
    return {};
  }

  auto final_suspend() noexcept {
    struct FinalAwaiter {
      bool await_ready() noexcept {
        return false;
      }

      std::coroutine_handle<> await_suspend(Handle self) noexcept {
        auto& promise = self.promise();
        if (promise.cancellation_.try_wait_for_children()) {
          return std::noop_coroutine();
        }
        return promise.complete_inline();
      }

      void await_resume() noexcept {
        LOG(FATAL) << "await_resume called at final_suspend";
      }
    };
    return FinalAwaiter{};
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
    return std::suspend_never{};
  }

  template <class U>
  auto await_transform(td::ResultUnwrap<U> wrapped) noexcept {
    return await_transform(std::move(wrapped.result));
  }
  template <class U>
  auto await_transform(td::ResultWrap<U> wrapped) noexcept {
    return result_awaiter_wrap(std::move(wrapped.result));
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
    return await_transform(AwaiterOptions<Task<U>>{std::move(task)});
  }
  template <class U>
  [[deprecated("co_await StartedTask is legacy; use std::move(task).child() or std::move(task).unlinked()")]]
  auto await_transform(StartedTask<U>&& task) noexcept {
    return await_transform(AwaiterOptions<StartedTask<U>>{std::move(task)});
  }

  template <class Aw, AwaiterLinkMode LinkMode, AwaiterUnwrapMode UnwrapMode, AwaiterTraceMode TraceMode>
    requires(detail::IsTaskAwaitableV<Aw>)
  auto await_transform(AwaiterOptions<Aw, LinkMode, UnwrapMode, TraceMode>&& options) noexcept {
    auto started = normalize_task_awaitable<LinkMode>(std::move(options.value));
    return make_task_awaiter<UnwrapMode, TraceMode>(std::move(started), std::move(options.trace_text_));
  }

  template <class Aw>
  auto await_transform(Aw&& aw) noexcept {
    return wrap_and_resume_on_current(std::forward<Aw>(aw));
  }

  auto await_transform(ThisScope) noexcept {
    return detail::ReadyAwaitable<CancelScope>{CancelScope{this}};
  }

  auto await_transform(IsActive) noexcept {
    return detail::ReadyAwaitable<bool>{!this->should_finish_due_to_cancellation()};
  }

  auto await_transform(EnsureActive) noexcept {
    struct EnsureActiveAwaiter {
      promise_type* promise;

      bool await_ready() noexcept {
        return !promise->should_finish_due_to_cancellation();
      }

      std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> handle) noexcept {
        return handle.promise().finish_if_cancelled().value_or(std::noop_coroutine());
      }

      void await_resume() noexcept {
      }
    };
    return EnsureActiveAwaiter{this};
  }

  auto await_transform(IgnoreCancellation) noexcept {
    struct IgnoreCancellationAwaiter {
      promise_type* promise;

      bool await_ready() noexcept {
        return promise->cancellation_.try_enter_ignore();
      }

      std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept {
        return h.promise().finish_if_cancelled().value_or(std::noop_coroutine());
      }

      CancellationGuard await_resume() noexcept {
        return CancellationGuard{promise};
      }
    };
    return IgnoreCancellationAwaiter{this};
  }

  // API used by TaskAwaiter specializations
  bool is_immediate_execution_always_allowed() const noexcept {
    return this->state_manager_data.executor.is_immediate_execution_always_allowed();
  }

  std::optional<std::coroutine_handle<>> finish_if_cancelled() {
    if (!this->should_finish_due_to_cancellation()) {
      return std::nullopt;
    }
    this->result = cancelled_status();
    if (this->cancellation_.try_wait_for_children()) {
      return std::noop_coroutine();
    }
    return this->complete_inline();
  }

  std::coroutine_handle<> route_resume() {
    if (auto h = finish_if_cancelled()) {
      return *h;
    }
    return this->state_manager_data.executor.resume_or_schedule(self());
  }

  std::coroutine_handle<> route_schedule() {
    this->state_manager_data.executor.schedule(self());
    return std::noop_coroutine();
  }

  std::coroutine_handle<> route_finish(td::Result<T> r) {
    this->return_value(std::move(r));
    return final_suspend().await_suspend(self());
  }

 private:
  template <AwaiterLinkMode LinkMode, class U>
  auto normalize_task_awaitable(Task<U>&& task) noexcept {
    if constexpr (LinkMode == AwaiterLinkMode::Unlinked) {
      return std::move(task).start_immediate_without_scope();
    } else {
      return std::move(task).start_immediate_in_parent_scope(bridge::make_parent_scope_lease(*this));
    }
  }

  template <AwaiterLinkMode LinkMode, class U>
  auto normalize_task_awaitable(StartedTask<U>&& task) noexcept {
    if constexpr (LinkMode == AwaiterLinkMode::Child) {
      check_child_started_task_await(task);
    } else if constexpr (LinkMode == AwaiterLinkMode::Auto) {
      debug_check_scoped_started_task_await(task.get_promise());
    }
    return std::move(task);
  }

  template <class U>
  void check_child_started_task_await(StartedTask<U>& task) const {
    CHECK(task.valid());
    auto* inner = task.get_promise();
    CHECK(inner);

    if (inner->cancellation_.is_parent(this)) {
      return;
    }
    if (task.await_ready()) {
      return;
    }
    if (inner->cancellation_.is_parent(this)) {
      return;
    }
    if (task.await_ready()) {
      return;
    }
    LOG(FATAL) << "Awaiting non-child StartedTask via child(). "
                  "Use unlinked() for explicit unsafe await.";
  }

  void debug_check_scoped_started_task_await(promise_common* inner) const {
    if (!this->cancellation_.has_parent_scope() || !inner || inner->cancellation_.has_parent_scope()) {
      return;
    }
#ifdef TD_DEBUG
    static std::atomic<bool> warned{false};
    if (warned.load(std::memory_order_relaxed)) {
      return;
    }
    if (!warned.exchange(true, std::memory_order_relaxed)) {
      LOG(WARNING) << "Awaiting StartedTask without parent scope inside a scoped coroutine. "
                      "Use start_in_parent_scope() to register parent scope.";
    }
#endif
  }
};

template <class T = Unit>
struct [[nodiscard]] Task {
  using value_type = T;

  using promise_type = td::actor::promise_type<T>;
  using Handle = std::coroutine_handle<promise_type>;
  enum class StartMode : uint8_t { Scheduled, Immediate, External };
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

  // Preferred: auto-peek parent scope from TLS
  auto start_in_parent_scope() && {
    return std::move(*this).start_impl(current_scope_lease(), StartMode::Scheduled);
  }
  auto start_immediate_in_parent_scope() && {
    return std::move(*this).start_impl(current_scope_lease(), StartMode::Immediate);
  }
  auto start_external_in_parent_scope() && {
    return std::move(*this).start_impl(current_scope_lease(), StartMode::External);
  }

  // Explicit parent scope
  auto start_in_parent_scope(ParentScopeLease scope) && {
    return std::move(*this).start_impl(std::move(scope), StartMode::Scheduled);
  }
  auto start_immediate_in_parent_scope(ParentScopeLease scope) && {
    return std::move(*this).start_impl(std::move(scope), StartMode::Immediate);
  }
  auto start_external_in_parent_scope(ParentScopeLease scope) && {
    return std::move(*this).start_impl(std::move(scope), StartMode::External);
  }

  // No parent scope
  auto start_without_scope() && {
    return std::move(*this).start_registered(StartMode::Scheduled);
  }
  auto start_immediate_without_scope() && {
    return std::move(*this).start_registered(StartMode::Immediate);
  }
  auto start_external_without_scope() && {
    return std::move(*this).start_registered(StartMode::External);
  }

  [[deprecated("use start_in_parent_scope() or start_without_scope()")]]
  auto start() && {
    return std::move(*this).start_without_scope();
  }

  ParentScopeLease lease() {
    CHECK(h);
    return bridge::make_parent_scope_lease(h.promise());
  }

  template <class F>
  auto then(F&& f) && {
    using Self = Task<T>;
    using FDecayed = std::decay_t<F>;
    using Awaitable = decltype(detail::make_awaitable(std::declval<FDecayed&>()(std::declval<T&&>())));
    using Ret = decltype(std::declval<Awaitable&>().await_resume());
    using U = detail::UnwrapTDResult<Ret>::Type;
    return [](Self task, FDecayed fn) mutable -> Task<U> {
      co_await become_lightweight();
      auto value = co_await std::move(task);
      co_return co_await detail::make_awaitable(fn(std::move(value)));
    }(std::move(*this), std::forward<F>(f));
  }

  template <class F>
  auto transform(F&& f) && {
    using Self = Task<T>;
    using FDecayed = std::decay_t<F>;
    using Awaitable = decltype(detail::make_awaitable(std::declval<FDecayed&>()(std::declval<td::Result<T>&&>())));
    using Ret = decltype(std::declval<Awaitable&>().await_resume());
    using U = detail::UnwrapTDResult<Ret>::Type;
    return [](Self task, FDecayed fn) mutable -> Task<U> {
      co_await become_lightweight();
      auto value = co_await std::move(task).wrap();
      co_return co_await detail::make_awaitable(fn(std::move(value)));
    }(std::move(*this), std::forward<F>(f));
  }

 private:
  StartedTask<T> start_impl(ParentScopeLease scope, StartMode mode) && {
    if (scope) {
      auto& promise = h.promise();
      promise.cancellation_.set_parent_lease(promise, std::move(scope));
    }
    return std::move(*this).start_registered(mode);
  }

  StartedTask<T> start_registered(StartMode mode) && {
    h.promise().add_ref();  // For StartedTask, before start
    run_registered_start(mode);
    return StartedTask<T>{std::exchange(h, {})};
  }

  void run_registered_start(StartMode mode) {
    switch (mode) {
      case StartMode::Scheduled:
        sm().start(h);
        break;
      case StartMode::Immediate: {
        detail::TlsGuard guard(&h.promise());
        sm().start_immediate(h);
        break;
      }
      case StartMode::External:
        sm().start_external();
        break;
    }
  }

 public:
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

  auto trace(std::string t) && {
    return Traced<Task>{std::move(*this), std::move(t)};
  }

  auto child() && {
    return ChildAwait<Task>{std::move(*this)};
  }

  auto unlinked() && {
    return UnlinkedAwait<Task>{std::move(*this)};
  }
};

template <class T = Unit>
struct [[nodiscard]] StartedTask {
  using value_type = T;

  using promise_type = td::actor::promise_type<T>;
  using Handle = std::coroutine_handle<promise_type>;
  Handle h{};

  bool valid() const {
    return h.address() != nullptr;
  }

  auto sm() {
    CHECK(h);
    return detail::StartedTaskStateManager<promise_type>{&h.promise().state_manager_data};
  }
  promise_common* get_promise() {
    return h ? &h.promise() : nullptr;
  }
  StartedTask() = default;
  explicit StartedTask(Handle hh) : h(hh) {
    CHECK(h);
  }
  StartedTask(StartedTask&& o) noexcept : h(std::exchange(o.h, {})) {
  }
  StartedTask& operator=(StartedTask&& o) {
    if (this != &o) {
      reset();
      h = std::exchange(o.h, {});
    }
    return *this;
  }

  StartedTask(const StartedTask&) = delete;
  StartedTask& operator=(const StartedTask&) = delete;

  ~StartedTask() noexcept {
    reset();
  }
  void reset() {
    // A completed task has already run final_suspend and has no live children.
    // Avoid redundant cancel() on this common path.
    if (h && !await_ready()) {
      cancel();
    }
    detach_silent();
  }
  void detach(std::string description = "UnknownTask") && {
    if (!h) {
      return;
    }
    [](auto self, std::string description) -> Task<Unit> {
      co_await become_lightweight();
      auto r = co_await std::move(self).wrap();
      LOG_IF(ERROR, r.is_error()) << "Detached task <" << description << "> failed: " << r.error();
      co_return td::Unit{};
    }(std::move(*this), std::move(description))
                                                  .start_immediate_in_parent_scope()
                                                  .detach_silent();
  }
  void detach_silent() {
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

  auto trace(std::string t) && {
    return Traced<StartedTask>{std::move(*this), std::move(t)};
  }

  auto child() && {
    return ChildAwait<StartedTask>{std::move(*this)};
  }

  auto unlinked() && {
    return UnlinkedAwait<StartedTask>{std::move(*this)};
  }

  template <class F>
  auto then(F&& f) && {
    using Self = StartedTask<T>;
    using FDecayed = std::decay_t<F>;
    using Awaitable = decltype(detail::make_awaitable(std::declval<FDecayed&>()(std::declval<T&&>())));
    using Ret = decltype(std::declval<Awaitable&>().await_resume());
    using U = detail::UnwrapTDResult<Ret>::Type;
    return [](Self task, FDecayed fn) mutable -> Task<U> {
      co_await become_lightweight();
      auto value = co_await std::move(task);
      co_return co_await detail::make_awaitable(fn(std::move(value)));
    }(std::move(*this), std::forward<F>(f));
  }

  // Cancel this task and all its children
  void cancel() {
    if (h) {
      h.promise().cancel();
    }
  }

  struct ExternalPromise : public PromiseInterface<T> {
    ExternalPromise() = default;
    explicit ExternalPromise(promise_type* promise_ptr) : promise(promise_ptr) {
    }

    void set_value(T&& value) override {
      promise.release()->external_return_value(std::move(value));
    }

    void set_error(Status&& error) override {
      promise.release()->external_return_value(std::move(error));
    }

    void set_result(Result<T>&& result) override {
      if (result.is_ok()) {
        set_value(result.move_as_ok());
      } else {
        set_error(result.move_as_error());
      }
    }

    explicit operator bool() const {
      return bool(promise);
    }

   private:
    struct Deleter {
      void operator()(promise_type* ptr) {
        ptr->external_return_value(td::Status::Error("promise destroyed"));
      }
    };
    std::unique_ptr<promise_type, Deleter> promise;
  };

  static std::pair<StartedTask, ExternalPromise> make_bridge() {
    auto task = []() -> Task<T> { co_return typename promise_type::ExternalResult{}; }();
    task.set_executor(Executor::on_scheduler());
    auto bridge_promise = ExternalPromise(&task.h.promise());
    auto started_task = std::move(task).start_external_in_parent_scope();
    return std::make_pair(std::move(started_task), std::move(bridge_promise));
  }
};

class TaskCancellationSource {
 public:
  TaskCancellationSource() = default;
  TaskCancellationSource(TaskCancellationSource&&) noexcept = default;
  TaskCancellationSource& operator=(TaskCancellationSource&&) noexcept = default;
  TaskCancellationSource(const TaskCancellationSource&) = delete;
  TaskCancellationSource& operator=(const TaskCancellationSource&) = delete;

  static TaskCancellationSource create_linked() {
    auto scope = current_scope_lease();
    CHECK(scope);
    return create_impl(std::move(scope));
  }

  static TaskCancellationSource create_detached() {
    return create_impl(ParentScopeLease{});
  }

  ParentScopeLease get_scope_lease() {
    auto* promise = root_.get_promise();
    CHECK(promise);
    // We know that the task is not finished. This is the only reason it is OK to get scope from outside the coroutine.
    return bridge::make_parent_scope_lease(*promise);
  }

  void cancel() {
    root_.reset();
    external_ = {};
  }

  explicit operator bool() const {
    return root_.valid();
  }

  ~TaskCancellationSource() {
    cancel();
  }

 private:
  static TaskCancellationSource create_impl(ParentScopeLease parent_scope) {
    auto task = []() -> Task<td::Unit> { co_return Task<td::Unit>::promise_type::ExternalResult{}; }();
    task.set_executor(Executor::on_scheduler());

    TaskCancellationSource source;
    source.external_ = StartedTask<td::Unit>::ExternalPromise(&task.h.promise());
    if (parent_scope) {
      source.root_ = std::move(task).start_external_in_parent_scope(std::move(parent_scope));
    } else {
      source.root_ = std::move(task).start_external_without_scope();
    }
    return source;
  }

  StartedTask<td::Unit>::ExternalPromise external_;
  StartedTask<td::Unit> root_;
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
void custom_connect(P&& p, Task<T>&& task, Lazy = {}) noexcept {
  connect(std::forward<P>(p), std::move(task).start_in_parent_scope(current_scope_lease()));
}

template <class P, class T>
void custom_connect(P&& p, Task<T>&& task, Immediate) noexcept {
  connect(std::forward<P>(p), std::move(task).start_immediate_in_parent_scope(current_scope_lease()));
}

}  // namespace td::actor
