#pragma once

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
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

struct promise_common;

template <class T>
struct promise_type;

namespace detail {

struct TaskStateManagerData {
  Executor executor{Executor::on_default()};
  std::atomic<uint8_t> flags{0};
  std::coroutine_handle<> continuation{};

  enum Flags : uint8_t {
    READY_FLAG = 1,
    STARTED_FLAG = 2,
    SUSPEND_FLAG = 4,
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

  // Called exactly once per coroutine lifetime — either from final_suspend (normal/error)
  // or from finish_if_cancelled (cancellation). Returns completion routing.
  [[nodiscard]] ReadyResult on_ready() {
    auto old_flags = set_flag(READY_FLAG);

    if (!(old_flags & STARTED_FLAG)) {
      return {continuation, false, true};
    }

    std::coroutine_handle<> next = std::noop_coroutine();
    if (old_flags & SUSPEND_FLAG) {
      next = continuation;
    }
    return {next, true, true};
  }

  void set_executor(Executor new_executor) noexcept {
    executor = std::move(new_executor);
  }
};

struct TaskControlBase : CancelNode {
  using CancelNode::add_ref;
  using CancelNode::dec_ref;

  static constexpr uint32_t kMagic = 0xC020C070;  // "coro ctrl"

  TaskControlBase() = default;
  ~TaskControlBase() override = default;

  void check_magic() const {
    LOG_CHECK(magic_ == kMagic) << "TaskControl magic mismatch — heap allocation was elided (HALO). "
                                   "This coroutine framework requires heap-allocated frames.";
  }

  virtual std::coroutine_handle<> get_handle() const = 0;

  bool try_destroy_frame() {
    if (!std::exchange(frame_destroyed_, true)) {
      get_handle().destroy();
      return true;
    }
    return false;
  }

  void on_cancel() override {
    cancel();
  }
  void on_cleanup() override {
    dec_ref();
  }
  void on_publish() override {
    add_ref();
  }

  bool should_finish_due_to_cancellation() const {
    return cancellation.should_finish_due_to_cancellation();
  }

  void cancel() {
    CHECK(core::SchedulerContext::get_ptr());
    cancellation.cancel(*this);
  }

  std::coroutine_handle<> complete_inline() {
    auto ready = state_manager_data.on_ready();

    TaskControlBase* parent = nullptr;
    if (ready.should_notify_parent) {
      parent = cancellation.take_parent_for_child_completed(*this);
    }

    add_ref();
    try_destroy_frame();

    if (ready.should_dec_ref) {
      dec_ref();
    }
    if (parent) {
      parent->cancellation.release_child_ref(*parent, CancellationRuntime::ChildReleasePolicy::MayComplete);
    }
    dec_ref();
    return ready.continuation;
  }

  void complete_scheduled() {
    auto continuation = complete_inline();
    if (continuation && continuation != std::noop_coroutine()) {
      SchedulerExecutor{}.schedule(continuation);
    }
  }

  TaskStateManagerData state_manager_data{};
  CancellationRuntime cancellation{};

 private:
  uint32_t magic_{kMagic};
  bool frame_destroyed_{false};
};

// TaskControl<T> is placed directly before the coroutine frame in the same allocation.
// Layout: [allocation_base ... padding] [TaskControl<T>] [coroutine frame]
// This avoids a separate heap allocation while letting the control outlive the frame.
template <class T>
struct TaskControl : TaskControlBase {
  std::coroutine_handle<promise_type<T>> handle() const {
    auto* frame = reinterpret_cast<const char*>(this) + sizeof(TaskControl);
    return std::coroutine_handle<promise_type<T>>::from_address(const_cast<char*>(frame));
  }

  promise_type<T>& promise() const {
    return handle().promise();
  }

  std::coroutine_handle<> get_handle() const override;

  void set_result(td::Result<T>&& r) {
    result_.emplace(std::move(r));
  }

  td::Result<T> extract_result() {
    CHECK(result_);
    return std::move(*result_);
  }

  const td::Result<T>& peek_result() const {
    CHECK(result_);
    return *result_;
  }

  void on_zero_refs() override;

  void free_allocation() noexcept {
    auto* base = allocation_base_;
    auto align = allocation_align_;
    std::destroy_at(this);
    ::operator delete(base, std::align_val_t(align));
  }

  static TaskControl* from_frame(void* frame_ptr) noexcept {
    return reinterpret_cast<TaskControl*>(static_cast<char*>(frame_ptr) - sizeof(TaskControl));
  }

  static void* allocate(std::size_t frame_size) {
    return allocate_impl(frame_size, alignof(std::max_align_t));
  }

  static void* allocate_aligned(std::size_t frame_size, std::size_t frame_alignment) {
    return allocate_impl(frame_size, frame_alignment);
  }

 private:
  static void* allocate_impl(std::size_t frame_size, std::size_t frame_alignment) {
    auto align = std::max(frame_alignment, alignof(TaskControl));
    auto total = sizeof(TaskControl) + frame_size + align - 1;
    void* base = ::operator new(total, std::align_val_t(align));
    auto raw = reinterpret_cast<std::uintptr_t>(base) + sizeof(TaskControl);
    auto frame_addr = (raw + align - 1) & ~(align - 1);
    auto* control = reinterpret_cast<TaskControl*>(frame_addr - sizeof(TaskControl));
    new (control) TaskControl();
    control->allocation_base_ = base;
    control->allocation_align_ = align;
    return reinterpret_cast<void*>(frame_addr);
  }

  std::optional<td::Result<T>> result_;
  void* allocation_base_{nullptr};
  std::size_t allocation_align_{alignof(std::max_align_t)};
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
};

}  // namespace detail

struct promise_common {
  detail::TaskControlBase* control_{nullptr};

  std::coroutine_handle<> get_handle() const {
    DCHECK(control_);
    return control_->get_handle();
  }

  detail::TaskControlBase& control() {
    DCHECK(control_);
    return *control_;
  }
  const detail::TaskControlBase& control() const {
    DCHECK(control_);
    return *control_;
  }
};

// Token encoding uses bottom 2 bits, so TaskControlBase must be 4-byte aligned
static_assert(alignof(detail::TaskControlBase) >= 4, "TaskControlBase must be 4-byte aligned for token encoding");

namespace bridge {
inline CancellationRuntime& runtime(detail::TaskControlBase& self) {
  return self.cancellation;
}

inline const CancellationRuntime& runtime(const detail::TaskControlBase& self) {
  return self.cancellation;
}

inline void complete_scheduled(detail::TaskControlBase& self) {
  self.complete_scheduled();
}

inline bool should_finish_due_to_cancellation(const detail::TaskControlBase& self) {
  return self.cancellation.should_finish_due_to_cancellation();
}

inline bool should_finish_due_to_cancellation_tls() {
  auto* ctrl = detail::get_current_ctrl();
  return ctrl && should_finish_due_to_cancellation(*ctrl);
}
}  // namespace bridge

inline ParentScopeLease current_scope_lease() {
  auto* ctrl = detail::get_current_ctrl();
  if (!ctrl) {
    return ParentScopeLease{};
  }
  return bridge::make_parent_scope_lease(*ctrl);
}

class CancelScope {
 public:
  CancelScope() = default;
  explicit CancelScope(detail::TaskControlBase* ctrl) : ctrl_(ctrl) {
  }

  bool is_cancelled() const {
    return ctrl_ && ctrl_->cancellation.is_cancelled();
  }

  void cancel() {
    if (ctrl_) {
      ctrl_->cancel();
    }
  }

  explicit operator bool() const {
    return ctrl_ != nullptr;
  }

  detail::TaskControlBase* get_ctrl() const {
    return ctrl_;
  }

 private:
  detail::TaskControlBase* ctrl_{nullptr};
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
  detail::TaskControlBase* ctrl_{};
  CancellationGuard() = default;
  explicit CancellationGuard(detail::TaskControlBase* c) : ctrl_(c) {
  }
  ~CancellationGuard() {
    if (ctrl_)
      ctrl_->cancellation.leave_ignore(*ctrl_);
  }
  CancellationGuard(CancellationGuard&& o) noexcept : ctrl_(std::exchange(o.ctrl_, nullptr)) {
  }
  CancellationGuard& operator=(CancellationGuard&&) = delete;
};

// Tags for connect execution mode
struct Immediate {};
struct Lazy {};

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
struct promise_type : promise_common {
  static_assert(!std::is_void_v<T>, "Task<void> is not supported; use Task<Unit> instead");
  using Handle = std::coroutine_handle<promise_type>;
  using Control = detail::TaskControl<T>;

  // we allocate Control in the same memory chunk as the promise
  static void* operator new(std::size_t size) {
    return Control::allocate(size);
  }

  static void* operator new(std::size_t size, std::align_val_t alignment) {
    return Control::allocate_aligned(size, static_cast<std::size_t>(alignment));
  }

  // do nothing in delete, actual memory will be allocated later and is owned by Control
  static void operator delete(void*) noexcept {
  }
  static void operator delete(void*, std::size_t, std::align_val_t) noexcept {
  }

  Control* typed_control() const noexcept {
    return static_cast<Control*>(this->control_);
  }
  auto& state_data() {
    return typed_control()->state_manager_data;
  }
  auto& executor() {
    return state_data().executor;
  }

  struct ExternalResult {
    explicit ExternalResult() = default;
  };

  promise_type() = default;

  ~promise_type() = default;

  void return_value(T value) noexcept {
    set_completion_result(td::Result<T>{std::move(value)});
  }

  template <class TT>
  void return_value(TT&& v) noexcept
    requires(!std::is_same_v<std::remove_cvref_t<TT>, T> && !std::is_same_v<std::remove_cvref_t<TT>, ExternalResult>)
  {
    set_completion_result(td::Result<T>{std::forward<TT>(v)});
  }

  void return_value(ExternalResult&&) noexcept {
  }

  void unhandled_exception() noexcept {
    set_completion_result(td::Status::Error("unhandled exception in coroutine"));
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
    auto* c = Control::from_frame(h.address());
    c->check_magic();
    CHECK(reinterpret_cast<const char*>(c) + sizeof(Control) == static_cast<const char*>(h.address()));
    this->control_ = c;
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
        auto& ctrl = *Control::from_frame(self.address());
        if (ctrl.cancellation.try_wait_for_children()) {
          return std::noop_coroutine();
        }
        return ctrl.complete_inline();
      }

      void await_resume() noexcept {
        LOG(FATAL) << "await_resume called at final_suspend";
      }
    };
    return FinalAwaiter{};
  }

  auto await_transform(detail::YieldOn y) {
    state_data().set_executor(y.executor);
    return y;
  }
  auto await_transform(detail::ResumeOn y) {
    state_data().set_executor(y.executor);
    return y;
  }

  template <class Aw>
  auto await_transform(SkipAwaitTransform<Aw> wrapped_aw) noexcept {
    return std::move(wrapped_aw.awaitable);
  }

  auto await_transform(Yield) noexcept {
    return yield_on(executor());
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
    return detail::ReadyAwaitable<CancelScope>{CancelScope{this->control_}};
  }

  auto await_transform(IsActive) noexcept {
    return detail::ReadyAwaitable<bool>{!this->control_->should_finish_due_to_cancellation()};
  }

  auto await_transform(EnsureActive) noexcept {
    struct EnsureActiveAwaiter {
      promise_type* promise;

      bool await_ready() noexcept {
        return !promise->control_->should_finish_due_to_cancellation();
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
        return promise->control_->cancellation.try_enter_ignore();
      }

      std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept {
        return h.promise().finish_if_cancelled().value_or(std::noop_coroutine());
      }

      CancellationGuard await_resume() noexcept {
        return CancellationGuard{promise->control_};
      }
    };
    return IgnoreCancellationAwaiter{this};
  }

  // API used by TaskAwaiter specializations
  bool is_immediate_execution_always_allowed() const noexcept {
    return executor().is_immediate_execution_always_allowed();
  }

  std::optional<std::coroutine_handle<>> finish_if_cancelled() {
    if (!this->control_->should_finish_due_to_cancellation()) {
      return std::nullopt;
    }
    set_completion_result(cancelled_status());
    if (this->control_->cancellation.try_wait_for_children()) {
      return std::noop_coroutine();
    }
    return typed_control()->complete_inline();
  }

  std::coroutine_handle<> route_resume() {
    if (auto h = finish_if_cancelled()) {
      return *h;
    }
    return executor().resume_or_schedule(self());
  }

  std::coroutine_handle<> route_schedule() {
    executor().schedule(self());
    return std::noop_coroutine();
  }

  std::coroutine_handle<> route_finish(td::Result<T> r) {
    set_completion_result(std::move(r));
    return final_suspend().await_suspend(self());
  }

 private:
  void set_completion_result(td::Result<T> result) noexcept {
    typed_control()->set_result(std::move(result));
  }

  template <class U>
  bool attach_parent_scope_and_notify_if_ready(StartedTask<U>& task, detail::TaskControlBase& inner_ctrl) {
    CHECK(!inner_ctrl.cancellation.has_parent_scope());
    inner_ctrl.cancellation.set_parent_lease(inner_ctrl, bridge::make_parent_scope_lease(*this->control_));
    if (!task.await_ready()) {
      return false;
    }
    // If task completed during attach, release the transient parent child-ref now.
    inner_ctrl.cancellation.notify_parent_child_completed(inner_ctrl);
    return true;
  }

  template <AwaiterLinkMode LinkMode, class U>
  auto normalize_task_awaitable(Task<U>&& task) noexcept {
    if constexpr (LinkMode == AwaiterLinkMode::Unlinked) {
      return std::move(task).start_immediate_without_scope();
    } else {
      return std::move(task).start_immediate_in_parent_scope(bridge::make_parent_scope_lease(*this->control_));
    }
  }

  template <AwaiterLinkMode LinkMode, class U>
  auto normalize_task_awaitable(StartedTask<U>&& task) noexcept {
    if constexpr (LinkMode == AwaiterLinkMode::Child) {
      check_child_started_task_await(task);
    } else if constexpr (LinkMode == AwaiterLinkMode::Auto) {
      auto was_ready = maybe_attach_started_task_scope(task);
      debug_check_scoped_started_task_await(task.valid() ? &task.ctrl() : nullptr, was_ready);
    }
    return std::move(task);
  }

  template <class U>
  bool maybe_attach_started_task_scope(StartedTask<U>& task) {
    auto was_ready = task.await_ready();
    if (was_ready || !this->control_->cancellation.has_parent_scope()) {
      return was_ready;
    }

    auto& inner_ctrl = task.ctrl();
    if (inner_ctrl.cancellation.has_parent_scope()) {
      return false;
    }

    return attach_parent_scope_and_notify_if_ready(task, inner_ctrl);
  }

  template <class U>
  void check_child_started_task_await(StartedTask<U>& task) {
    CHECK(task.valid());
    if (task.await_ready()) {
      return;
    }
    auto& inner_ctrl = task.ctrl();
    if (inner_ctrl.cancellation.is_parent(this->control_)) {
      return;
    }
    if (!inner_ctrl.cancellation.has_parent_scope()) {
      attach_parent_scope_and_notify_if_ready(task, inner_ctrl);
    }
    LOG_CHECK(task.await_ready() || inner_ctrl.cancellation.is_parent(this->control_))
        << "Awaiting non-child StartedTask via child(). "
           "Use unlinked() for explicit unsafe await.";
  }

  void debug_check_scoped_started_task_await(detail::TaskControlBase* inner_ctrl, bool inner_ready) const {
    if (inner_ready || !this->control_->cancellation.has_parent_scope() || !inner_ctrl ||
        inner_ctrl->cancellation.has_parent_scope()) {
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

template <class T>
inline std::coroutine_handle<> detail::TaskControl<T>::get_handle() const {
  return handle();
}

template <class T>
inline void detail::TaskControl<T>::on_zero_refs() {
  cancellation.on_last_ref_teardown(*this);
  try_destroy_frame();
  free_allocation();
}

template <class Derived, class T>
struct TaskHandle {
  using Control = detail::TaskControl<T>;
  Ref<Control> control_;

  Control& ctrl() const {
    DCHECK(control_);
    return *control_;
  }

  td::Result<T> await_resume() noexcept {
    return ctrl().extract_result();
  }

  const td::Result<T>& await_resume_peek() const noexcept {
    return ctrl().peek_result();
  }

  auto wrap() && {
    return Wrapped<Derived>{std::move(self())};
  }

  auto trace(std::string t) && {
    return Traced<Derived>{std::move(self()), std::move(t)};
  }

  auto child() && {
    return ChildAwait<Derived>{std::move(self())};
  }

  auto unlinked() && {
    return UnlinkedAwait<Derived>{std::move(self())};
  }

  template <class F>
  auto then(F&& f) && {
    using FDecayed = std::decay_t<F>;
    using Awaitable = decltype(detail::make_awaitable(std::declval<FDecayed&>()(std::declval<T&&>())));
    using Ret = decltype(std::declval<Awaitable&>().await_resume());
    using U = detail::UnwrapTDResult<Ret>::Type;
    return [](Derived task, FDecayed fn) mutable -> Task<U> {
      co_await become_lightweight();
      auto value = co_await std::move(task);
      co_return co_await detail::make_awaitable(fn(std::move(value)));
    }(std::move(self()), std::forward<F>(f));
  }

 private:
  Derived& self() {
    return *static_cast<Derived*>(this);
  }
};

template <class T = Unit>
struct [[nodiscard]] Task : TaskHandle<Task<T>, T> {
  using value_type = T;

  using promise_type = td::actor::promise_type<T>;
  using Handle = std::coroutine_handle<promise_type>;
  enum class StartMode : uint8_t { Scheduled, Immediate, External };
  using Control = detail::TaskControl<T>;

  Task() = default;
  explicit Task(Handle hh) {
    this->control_ = Ref<Control>::adopt(Control::from_frame(hh.address()));
  }
  Task(Task&&) noexcept = default;
  Task& operator=(Task&& o) = delete;
  Task(const Task&) = delete;
  Task& operator=(const Task&) = delete;

  promise_type& promise() const {
    return this->ctrl().promise();
  }
  Handle handle() const {
    return this->ctrl().handle();
  }

  auto sm() {
    return detail::TaskStateManager<promise_type>{&this->ctrl().state_manager_data};
  }

  ~Task() noexcept = default;
  void detach() {
    this->control_.reset();
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
    return bridge::make_parent_scope_lease(this->ctrl());
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
      auto& ctrl = this->ctrl();
      ctrl.cancellation.set_parent_lease(ctrl, std::move(scope));
    }
    return std::move(*this).start_registered(mode);
  }

  StartedTask<T> start_registered(StartMode mode) && {
    this->ctrl().add_ref();  // coroutine's own ref, decremented on completion
    run_registered_start(mode);
    return StartedTask<T>{std::move(this->control_)};
  }

  void run_registered_start(StartMode mode) {
    auto h = handle();
    switch (mode) {
      case StartMode::Scheduled:
        sm().start(h);
        break;
      case StartMode::Immediate: {
        detail::TlsGuard guard(h.promise().control_);
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
    sm().set_executor(std::move(new_executor));
  }

  bool await_ready() noexcept {
    return sm().is_ready();
  }

  std::coroutine_handle<> await_suspend(std::coroutine_handle<> continuation) noexcept {
    return sm().on_suspend_and_start(handle(), continuation);
  }
};

template <class T = Unit>
struct [[nodiscard]] StartedTask : TaskHandle<StartedTask<T>, T> {
  using value_type = T;

  using promise_type = td::actor::promise_type<T>;
  using Handle = std::coroutine_handle<promise_type>;
  using Control = detail::TaskControl<T>;

  bool valid() const {
    return bool(this->control_);
  }

  auto sm() {
    return detail::StartedTaskStateManager<promise_type>{&this->ctrl().state_manager_data};
  }
  Handle handle() const {
    return this->ctrl().handle();
  }
  StartedTask() = default;
  explicit StartedTask(Ref<Control> ctrl) {
    CHECK(ctrl);
    this->control_ = std::move(ctrl);
  }
  StartedTask(StartedTask&&) noexcept = default;
  StartedTask& operator=(StartedTask&& o) {
    if (this != &o) {
      reset();
      this->control_ = std::move(o.control_);
    }
    return *this;
  }

  StartedTask(const StartedTask&) = delete;
  StartedTask& operator=(const StartedTask&) = delete;

  ~StartedTask() noexcept {
    reset();
  }
  void reset() {
    if (this->control_ && !await_ready()) {
      cancel();
    }
    detach_silent();
  }
  void detach(std::string description = "UnknownTask") && {
    if (!this->control_) {
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
    this->control_.reset();
  }
  bool await_ready() noexcept {
    return sm().is_ready();
  }

  std::coroutine_handle<> await_suspend(std::coroutine_handle<> continuation) noexcept {
    return sm().on_suspend(continuation);
  }

  void cancel() {
    if (this->control_ && !sm().is_ready()) {
      this->ctrl().cancel();
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
    auto bridge_promise = ExternalPromise(&task.promise());
    auto started_task = std::move(task).start_external_in_parent_scope();
    return std::make_pair(std::move(started_task), std::move(bridge_promise));
  }
};

class TaskGroup {
 public:
  TaskGroup() = default;
  TaskGroup(TaskGroup&&) noexcept = default;
  TaskGroup& operator=(TaskGroup&&) noexcept = default;
  TaskGroup(const TaskGroup&) = delete;
  TaskGroup& operator=(const TaskGroup&) = delete;

  static TaskGroup linked() {
    auto scope = current_scope_lease();
    CHECK(scope);
    return create_impl(std::move(scope));
  }

  static TaskGroup detached() {
    return create_impl(ParentScopeLease{});
  }

  // Compatibility aliases.
  static TaskGroup create_linked() {
    return linked();
  }

  static TaskGroup create_detached() {
    return detached();
  }

  ParentScopeLease lease() {
    return get_scope_lease();
  }

  ParentScopeLease get_scope_lease() {
    CHECK(!closed_);
    CHECK(root_.valid());
    return bridge::make_parent_scope_lease(root_.ctrl());
  }

  template <class T>
  StartedTask<T> start(Task<T> task) {
    CHECK(!closed_);
    return std::move(task).start_in_parent_scope(get_scope_lease());
  }

  void cancel() {
    if (root_.valid()) {
      root_.cancel();
    }
  }

  Task<td::Unit> join() {
    CHECK(!joined_);
    joined_ = true;
    closed_ = true;
    if (external_) {
      external_.set_value(td::Unit{});
      external_ = {};
    }
    return await_root(std::move(root_));
  }

  Task<td::Unit> cancel_and_join() {
    cancel();
    return join();
  }

  explicit operator bool() const {
    return root_.valid();
  }

  ~TaskGroup() {
    cleanup();
  }

 private:
  static Task<td::Unit> await_root(StartedTask<td::Unit> root) {
    auto result = co_await std::move(root).wrap();
    if (result.is_error() && result.error().code() != kCancelledCode) {
      co_return result.move_as_error();
    }
    co_return td::Unit{};
  }

  static TaskGroup create_impl(ParentScopeLease parent_scope) {
    auto task = []() -> Task<td::Unit> { co_return Task<td::Unit>::promise_type::ExternalResult{}; }();
    task.set_executor(Executor::on_scheduler());

    TaskGroup source;
    source.external_ = StartedTask<td::Unit>::ExternalPromise(&task.promise());
    source.root_ = std::move(task).start_external_in_parent_scope(std::move(parent_scope));
    return source;
  }

  void cleanup() {
    cancel();
    root_.reset();
    external_ = {};
    closed_ = true;
    joined_ = true;
  }

  StartedTask<td::Unit>::ExternalPromise external_;
  StartedTask<td::Unit> root_;
  bool closed_{false};
  bool joined_{false};
};

using TaskCancellationSource = TaskGroup;

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
