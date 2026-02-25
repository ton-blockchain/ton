#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "td/actor/actor.h"
#include "td/actor/coro_executor.h"
#include "td/actor/coro_task.h"
#include "td/actor/coro_timer.h"
#include "td/actor/coro_types.h"
#include "td/utils/Mutex.h"
#include "td/utils/Slice.h"
#include "td/utils/Status.h"
#include "td/utils/VectorQueue.h"

namespace td::actor {

namespace detail {

template <class M>
struct memfn_meta;

template <class T>
struct remove_memfn_const {
  using type = T;
};
template <class C, class R, class... Args>
struct remove_memfn_const<R (C::*)(Args...) const> {
  using type = R (C::*)(Args...);
};
template <class T>
using remove_memfn_const_t = remove_memfn_const<T>::type;

template <class M>
struct memfn_meta : memfn_meta<remove_memfn_const_t<M>> {};
template <class C, class R, class... Args>
struct memfn_meta<R (C::*)(Args...)> {
  using cls = C;
  using ret = R;
};

template <class P>
struct unwrap_promise {
  using type = void;
};
template <class T>
struct unwrap_promise<td::Promise<T>> {
  using type = T;
};

template <typename... Args>
using last_t = std::tuple_element_t<sizeof...(Args), std::tuple<void, Args...>>;

template <class T>
struct is_task : std::false_type {};
template <class T>
struct is_task<Task<T>> : std::true_type {};

}  // namespace detail

template <class T>
td::Result<std::vector<T>> collect(std::vector<td::Result<T>>&& results) {
  for (auto& result : results) {
    if (result.is_error()) {
      return result.move_as_error();
    }
  }
  std::vector<T> values;
  values.reserve(results.size());
  for (auto& result : results) {
    values.push_back(result.move_as_ok());
  }
  return values;
}

template <class... Ts>
td::Result<std::tuple<Ts...>> collect(std::tuple<td::Result<Ts>...> results) {
  return std::apply(
      [](auto&&... results) -> td::Result<std::tuple<Ts...>> {
        td::Status error;
        bool has_error = false;

        (void)((results.is_error() ? (has_error = true, error = results.move_as_error(), false) : true) && ...);

        if (has_error) {
          return std::move(error);
        }

        return std::tuple<Ts...>{std::move(results).move_as_ok()...};
      },
      std::move(results));
}

template <class Awaitable>
using await_result_t =
    decltype(std::declval<Task<td::Unit>::promise_type>().await_transform(std::declval<Awaitable>()).await_resume());

template <class Awaitable>
using wrap_awaitable_t = decltype(std::declval<Awaitable>().wrap());

template <class... Awaitables, std::enable_if_t<(sizeof...(Awaitables) > 1), int> = 0>
auto all(Awaitables&&... awaitables) -> Task<std::tuple<await_result_t<Awaitables>...>> {
  co_await become_lightweight();
  co_return std::tuple<await_result_t<Awaitables>...>{co_await std::forward<Awaitables>(awaitables)...};
}

template <class... Awaitables, std::enable_if_t<(sizeof...(Awaitables) > 1), int> = 0>
auto all_wrap(Awaitables&&... awaitables) -> Task<std::tuple<await_result_t<wrap_awaitable_t<Awaitables>>...>> {
  co_await become_lightweight();
  co_return std::tuple<await_result_t<wrap_awaitable_t<Awaitables>>...>{
      co_await std::forward<Awaitables>(awaitables).wrap()...};
}

template <CoroTask TaskType>
Task<std::vector<await_result_t<TaskType>>> all(std::vector<TaskType> tasks) {
  co_await become_lightweight();
  std::vector<await_result_t<TaskType>> results;
  results.reserve(tasks.size());
  for (auto& task : tasks) {
    results.push_back(co_await std::move(task));
  }
  co_return results;
}

template <CoroTask TaskType>
Task<std::vector<await_result_t<wrap_awaitable_t<TaskType>>>> all_wrap(std::vector<TaskType> tasks) {
  co_await become_lightweight();
  std::vector<await_result_t<wrap_awaitable_t<TaskType>>> results;
  results.reserve(tasks.size());
  for (auto& task : tasks) {
    results.push_back(co_await std::move(task).wrap());
  }
  co_return results;
}

enum class UnifiedKind : uint8_t { None, Void, TaskReturn, PromiseArgument, ReturnValue };

template <class M>
struct unified_result;

template <class MemFn>
struct unified_result : unified_result<detail::remove_memfn_const_t<MemFn>> {};

template <class C, class T, class... Args>
struct unified_result<td::Result<T> (C::*)(Args...)> {
  using type = T;
  static constexpr UnifiedKind kind = UnifiedKind::ReturnValue;
};

template <class C, class T, class... Args>
struct unified_result<Task<T> (C::*)(Args...)> {
  using type = T;
  static constexpr UnifiedKind kind = UnifiedKind::TaskReturn;
};

template <class C, class T, class... Args>
struct unified_result<T (C::*)(Args...)> {
  using type = T;
  static constexpr UnifiedKind kind = UnifiedKind::ReturnValue;
};

template <class C, class... Args>
struct unified_result<void (C::*)(Args...)> {
  using Last = std::remove_cvref_t<detail::last_t<Args...>>;
  static constexpr bool is_promise = std::is_same_v<Last, td::Promise<typename detail::unwrap_promise<Last>::type>>;
  using type = std::conditional_t<is_promise, typename detail::unwrap_promise<Last>::type, void>;
  static constexpr UnifiedKind kind = is_promise ? UnifiedKind::PromiseArgument : UnifiedKind::Void;
};

// Concept: arguments must match the member function signature
template <class MemFn, class... Args>
concept AskArgsValid = [] {
  using Meta = unified_result<MemFn>;
  using C = detail::memfn_meta<std::remove_cvref_t<MemFn>>::cls;

  if constexpr (Meta::kind == UnifiedKind::PromiseArgument) {
    using T = Meta::type;
    using P = StartedTask<std::conditional_t<std::is_void_v<T>, td::Unit, T>>::ExternalPromise;
    return std::is_invocable_v<MemFn, C&, Args..., P>;
  } else {
    return std::is_invocable_v<MemFn, C&, Args...>;
  }
}();

template <bool Later, class TargetId, class MemFn, class... Args>
auto ask_impl(TargetId&& to, MemFn mf, Args&&... args) {
  using Meta = unified_result<MemFn>;
  using T = Meta::type;

  using TT = std::conditional_t<std::is_void_v<T>, td::Unit, T>;

  static_assert(Meta::kind == UnifiedKind::TaskReturn || Meta::kind == UnifiedKind::PromiseArgument ||
                    Meta::kind == UnifiedKind::ReturnValue || Meta::kind == UnifiedKind::Void,
                "ask: method must return Task<T> or take td::Promise<T> as last parameter");

  if constexpr (Meta::kind == UnifiedKind::TaskReturn) {
    return ask_new_impl<Later>(std::forward<TargetId>(to), mf, std::forward<Args>(args)...);
  } else {
    auto [task, promise] = StartedTask<TT>::make_bridge();
    td::actor::internal::send_closure_dispatch<Later>(std::forward<TargetId>(to), mf, std::forward<Args>(args)...,
                                                      std::move(promise));
    return std::move(task);
  }
}

template <bool Later, class TargetId, class MemFn, class... Args>
auto ask_new_impl(TargetId&& to, MemFn mf, Args&&... args) {
  using Meta = unified_result<MemFn>;
  using T = Meta::type;
  static_assert(Meta::kind == UnifiedKind::TaskReturn, "ask: method must return Task<T>");

  if constexpr (Later) {
    // ask(Task-return) stays lazy.
    auto task = [](auto closure) -> Task<T> {
      co_return co_await detail::run_on_current_actor(closure);
    }(create_delayed_closure(mf, std::forward<Args>(args)...));
    task.set_executor(Executor::on_actor(to));
    return task;
  } else {
    // ask_immediate(Task-return): preserve legacy eager fast path via send_immediate.
    std::optional<StartedTask<T>> o_task;
    td::actor::detail::send_immediate(
        to.as_actor_ref(),
        [&] {
          o_task.emplace(detail::run_on_current_actor(create_immediate_closure(mf, std::forward<Args>(args)...))
                             .start_immediate_in_parent_scope());
        },
        [&]() {
          auto task = [](auto closure) -> Task<T> {
            co_return co_await detail::run_on_current_actor(closure);
          }(create_delayed_closure(mf, std::forward<Args>(args)...));
          task.set_executor(Executor::on_actor(to));
          o_task.emplace(std::move(task).start_external_in_parent_scope());
          return detail::ActorExecutor::to_message(o_task->handle());
        });
    return std::move(*o_task);
  }
}

template <class TargetId, class MemFn, class... Args>
  requires AskArgsValid<MemFn, Args...>
auto ask_new(TargetId&& to, MemFn mf, Args&&... args) {
  return ask_new_impl<true>(std::forward<TargetId>(to), mf, std::forward<Args>(args)...);
}

template <class TargetId, class MemFn, class... Args>
  requires AskArgsValid<MemFn, Args...>
auto ask_new_immediate(TargetId&& to, MemFn mf, Args&&... args) {
  return ask_new_impl<false>(std::forward<TargetId>(to), mf, std::forward<Args>(args)...);
}

template <class TargetId, class MemFn, class... Args>
  requires AskArgsValid<MemFn, Args...>
auto ask(TargetId&& to, MemFn mf, Args&&... args) {
  return ask_impl<true>(std::forward<TargetId>(to), mf, std::forward<Args>(args)...);
}

template <class TargetId, class MemFn, class... Args>
  requires AskArgsValid<MemFn, Args...>
auto ask_immediate(TargetId&& to, MemFn mf, Args&&... args) {
  return ask_impl<false>(std::forward<TargetId>(to), mf, std::forward<Args>(args)...);
}

template <typename TaskType>
auto spawn_actor(td::Slice name, TaskType task) {
  using StartedTaskType = StartedTask<typename TaskType::value_type>;
  using PromiseType = StartedTaskType::ExternalPromise;
  auto [result_task, result_promise] = StartedTaskType::make_bridge();

  struct TaskAwaiter : public td::actor::Actor {
    TaskAwaiter(TaskType task, PromiseType promise) : task_(std::move(task)), promise_(std::move(promise)) {
    }

   private:
    TaskType task_;
    PromiseType promise_;
    void start_up() {
      task_.set_executor(Executor::on_current_actor());
      connect(std::move(promise_), std::move(task_));
    }
  };
  td::actor::create_actor<TaskAwaiter>(name, std::move(task), std::move(result_promise)).release();
  return std::move(result_task);
}

namespace detail {

struct TaskActorBase : public td::actor::core::Actor {
 protected:
  void loop() final {
    // will be ignored till task_loop is called
    want_loop_ = true;
    detail::resume_on_current_tls(loop_cont_);
  }

  bool want_loop_{false};

  std::coroutine_handle<> loop_cont_ = std::noop_coroutine();

  struct Loop {
    TaskActorBase* self;
    bool await_ready() {
      return false;
    }
    void await_suspend(std::coroutine_handle<> handle) {
      self->loop_cont_ = handle;
      if (self->want_loop_) {
        self->yield();
      }
    }
    std::coroutine_handle<> await_resume() {
      return std::exchange(self->loop_cont_, std::noop_coroutine());
    }
  };

  td::Status error;
  Task<td::Unit> task_loop_inner() {
    while (true) {
      co_await std::move(error);
      want_loop_ = false;
      auto action = co_await task_loop_once();
      co_await std::move(error);
      if (action == Action::Finish) {
        co_return td::Unit{};
      }
      co_await Loop{this};
    }
  }

  void on_error(td::Status error_to_set) {
    if (error.is_ok()) {
      error = std::move(error_to_set);
    } else {
      LOG(WARNING) << "Dropping error (already have one): " << error_to_set;
    }
    loop();
  }

  void hangup() override {
    on_error(Status::Error("Actor hangup"));
  }
  void alarm() override {
    on_error(Status::Error("Actor timeout"));
  }

  enum class Action { KeepRunning, Finish };
  // Note that while task_loop_once is coroutine, it is atomic in a sense that on_error won't stop its executions
  virtual Task<Action> task_loop_once() {
    co_return Action::Finish;  // will move straight to finish
  }
};

}  // namespace detail

template <class T>
struct TaskActor : public detail::TaskActorBase {
 protected:
  using TaskT = T;
  using Action = detail::TaskActorBase::Action;
  virtual Task<T> finish(td::Status status) = 0;

  Task<T> task_loop() {
    auto r = co_await task_loop_inner().wrap();
    auto res = co_await finish(r.is_ok() ? td::Status::OK() : r.move_as_error()).wrap();
    stop();
    co_return res;
  }

 public:
  template <class ActorT, class... ArgsT>
  friend auto spawn_task_actor(td::Slice name, ArgsT... args);
};

template <class ActorT, class... ArgsT>
auto spawn_task_actor(td::Slice name, ArgsT... args) {
  auto actor = td::actor::create_actor<ActorT>(name, std::forward<ArgsT>(args)...).release();
  return ask(std::move(actor), &ActorT::task_loop).start_in_parent_scope();
}

inline StartedTask<td::Unit> coro_sleep(td::Timestamp t) {
  auto [task, promise] = td::actor::StartedTask<td::Unit>::make_bridge();
  struct S : public td::actor::Actor {
    S(td::actor::StartedTask<td::Unit>::ExternalPromise promise, td::Timestamp t)
        : promise_(std::move(promise)), t_(t) {
    }

   private:
    td::actor::StartedTask<td::Unit>::ExternalPromise promise_;
    td::Timestamp t_;
    void start_up() override {
      alarm_timestamp() = t_;
    }
    void alarm() override {
      promise_.set_value(td::Unit{});
    }
  };
  create_actor<S>("sleep", std::move(promise), t).release();
  return std::move(task);
}

struct CoroMutex {
  CoroMutex() = default;
  CoroMutex(const CoroMutex&) = delete;
  CoroMutex(const CoroMutex&&) = delete;
  CoroMutex& operator=(const CoroMutex&) = delete;
  CoroMutex& operator=(const CoroMutex&&) = delete;

  struct [[nodiscard("Lock must be held to maintain mutual exclusion")]] Lock {
    Lock(CoroMutex* m) : mutex_(m) {
    }
    ~Lock() {
      reset();
    }
    Lock(Lock&& o) noexcept : mutex_(std::exchange(o.mutex_, nullptr)) {
    }
    void reset() {
      if (mutex_) {
        mutex_->unlock();
        mutex_ = nullptr;
      }
    }
    Lock& operator=(Lock&&) = delete;
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;

   private:
    CoroMutex* mutex_;
  };

  bool await_ready() {
    return !is_locked_;
  }
  Lock lock_unsafe() {
    CHECK(++lock_cnt_ == 1);
    is_locked_ = true;
    return Lock{this};
  }

  Lock await_resume() {
    return lock_unsafe();
  }

  template <class OuterPromise>
  std::coroutine_handle<> route_resume(std::coroutine_handle<OuterPromise> h) noexcept {
    return h.promise().route_schedule();
  }

  template <class OuterPromise>
  std::coroutine_handle<> await_suspend(std::coroutine_handle<OuterPromise> h) noexcept {
    auto r_handle = detail::wrap_coroutine(this, h);
    pending_.push(r_handle);
    return std::noop_coroutine();
  }

  struct LockAwaitable {
    CoroMutex* m;
    bool await_ready() {
      return m->await_ready();
    }
    template <class OuterPromise>
    void await_suspend(std::coroutine_handle<OuterPromise> h) noexcept {
      m->await_suspend(h);
    }
    Lock await_resume() {
      return m->await_resume();
    }
  };

  [[nodiscard]] SkipAwaitTransform<LockAwaitable> lock() {
    return {LockAwaitable{this}};
  }

 private:
  bool is_locked_{false};
  int lock_cnt_{0};
  VectorQueue<std::coroutine_handle<>> pending_;

  void unlock() {
    CHECK(is_locked_);
    CHECK(--lock_cnt_ == 0);
    if (!pending_.empty()) {
      auto handle = pending_.pop();
      detail::resume_on_current_tls(handle);
      return;
    }
    is_locked_ = false;
  }
};

// CoroCoalesce: coalesce concurrent requests for the same key
// - Multiple callers requesting same key → only one computes
// - Result shared among all waiters
// - Entry auto-removed when all callers done (via shared_ptr ref counting)
//
// Usage:
//   CoroCoalesce<int, std::string> coalesce;
//   Task<std::string> get(int key) {
//     co_return co_await coalesce.run(key, [key]() -> Task<std::string> {
//       co_return expensive_compute(key);
//     });
//   }
template <class K, class V>
class CoroCoalesce {
 public:
  template <class F>
  Task<V> run(K key, F compute) {
    auto entry = get_or_create(std::move(key));
    auto lock = co_await entry->gate.lock();
    if (!entry->result) {
      entry->result = co_await compute().wrap();
    }
    co_return entry->result->clone();
  }

 private:
  struct Entry {
    CoroMutex gate;
    std::optional<Result<V>> result;
  };

  std::map<K, std::weak_ptr<Entry>> entries_;

  std::shared_ptr<Entry> get_or_create(K key) {
    auto& slot = entries_[key];
    if (auto p = slot.lock()) {
      return p;
    }
    auto p = std::shared_ptr<Entry>(new Entry{}, [this, key](Entry* e) {
      entries_.erase(key);
      delete e;
    });
    slot = p;
    return p;
  }
};

inline void ParentScopeLease::publish_cancel_promise(td::Promise<td::Unit> p) {
  struct CancelPromiseNode : HeapCancelNode {
    td::Promise<td::Unit> promise_;
    explicit CancelPromiseNode(td::Promise<td::Unit> promise) : promise_(std::move(promise)) {
    }
    void do_cancel() override {
      promise_.set_value({});
    }
    void do_cleanup() override {
      promise_.set_error(td::Status::Error("scope completed without cancellation"));
    }
  };
  publish_heap_cancel_node(*td::actor::make_ref<CancelPromiseNode>(std::move(p)));
}

// with_timeout: await a StartedTask with a timeout.
// If timeout wins the race, cancel the task and return Error(653, "timeout").
template <class T>
Task<Result<T>> with_timeout(StartedTask<T> task, double seconds) {
  if (seconds <= 0) {
    task.cancel();
    co_return co_await std::move(task).wrap();
  }

  auto [bridge, promise] = StartedTask<Result<T>>::make_bridge();

  struct State {
    std::atomic<bool> done{false};
    typename StartedTask<Result<T>>::ExternalPromise promise;
    Ref<detail::TaskControlBase> awaited;

    explicit State(detail::TaskControlBase* ctrl) : awaited(Ref<detail::TaskControlBase>::share(ctrl)) {
    }

    bool try_mark_done() {
      return !done.exchange(true);
    }

    void try_set_result(Result<T> result) {
      if (try_mark_done()) {
        promise.set_value(std::move(result));
      }
    }

    void try_timeout() {
      if (!try_mark_done()) {
        return;
      }
      if (awaited) {
        awaited->cancel();
      }
      promise.set_value(Result<T>(Status::Error(653, "timeout")));
    }
  };

  auto state = std::make_shared<State>(&task.ctrl());
  state->promise = std::move(promise);

  // Timer: sleep, set timeout
  auto timer = [](std::shared_ptr<State> state, double s) -> Task<Unit> {
    co_await sleep_for(s);
    if (!(co_await is_active())) {
      co_return Unit{};
    }
    state->try_timeout();
    co_return Unit{};
  }(state, seconds)
                                                                 .start_in_parent_scope();

  // Main: await task, set result (task cancelled via destructor when main cancelled)
  auto main = [](std::shared_ptr<State> state, StartedTask<T> task) -> Task<Unit> {
    auto result = co_await std::move(task).wrap();
    state->try_set_result(std::move(result));
    co_return Unit{};
  }(state, std::move(task))
                                                                           .start_in_parent_scope();

  co_return co_await std::move(bridge).child();
}

// Overload that takes a Timestamp instead of seconds
template <class T>
Task<Result<T>> with_timeout(StartedTask<T> task, td::Timestamp deadline) {
  co_return co_await with_timeout(std::move(task), deadline.at() - td::Timestamp::now().at());
}

template <class T>
Task<T> any(std::vector<Task<T>> tasks) {
  if (tasks.empty()) {
    co_return td::Status::Error("any: empty tasks");
  }

  auto group = TaskGroup::linked();
  auto [winner_task, winner_promise] = StartedTask<T>::make_bridge();
  struct State {
    td::TinyMutex mutex;
    size_t remaining{0};
    bool resolved{false};
    std::optional<td::Status> first_error;
    typename StartedTask<T>::ExternalPromise winner;

    State(size_t count, typename StartedTask<T>::ExternalPromise promise)
        : remaining(count), winner(std::move(promise)) {
    }
  };
  auto state = std::make_shared<State>(tasks.size(), std::move(winner_promise));

  for (auto& task : tasks) {
    auto waiter = [](Task<T> task, std::shared_ptr<State> state) -> Task<td::Unit> {
      auto result = co_await std::move(task).wrap();
      std::optional<T> value;
      std::optional<td::Status> error;
      {
        std::lock_guard<td::TinyMutex> guard(state->mutex);
        if (state->resolved) {
          co_return td::Unit{};
        }

        if (result.is_ok()) {
          state->resolved = true;
          value.emplace(result.move_as_ok());
        } else {
          if (!state->first_error) {
            state->first_error = result.error().clone();
          }
          CHECK(state->remaining > 0);
          state->remaining--;
          if (state->remaining == 0) {
            state->resolved = true;
            if (state->first_error) {
              error = state->first_error->clone();
            } else {
              error = td::Status::Error("any: all tasks failed");
            }
          }
        }
      }

      if (value) {
        state->winner.set_value(std::move(*value));
      } else if (error) {
        state->winner.set_error(std::move(*error));
      }
      co_return td::Unit{};
    }(std::move(task), state);
    group.start(std::move(waiter)).detach_silent();
  }

  auto winner = co_await std::move(winner_task).wrap();
  if (winner.is_ok()) {
    group.cancel();
  }
  co_await group.join();
  co_return std::move(winner);
}

template <class T>
Task<std::vector<T>> all_fail_fast(std::vector<Task<T>> tasks) {
  if (tasks.empty()) {
    co_return td::Status::Error("all_fail_fast: empty tasks");
  }

  auto group = TaskGroup::linked();
  auto [ready_task, ready_promise] = StartedTask<td::Unit>::make_bridge();
  struct State {
    td::TinyMutex mutex;
    size_t remaining{0};
    bool ready_signaled{false};
    std::vector<std::optional<td::Result<T>>> results;
    typename StartedTask<td::Unit>::ExternalPromise ready;

    State(size_t count, typename StartedTask<td::Unit>::ExternalPromise promise)
        : remaining(count), results(count), ready(std::move(promise)) {
    }
  };
  auto state = std::make_shared<State>(tasks.size(), std::move(ready_promise));

  for (size_t i = 0; i < tasks.size(); i++) {
    auto waiter = [](size_t index, Task<T> task, std::shared_ptr<State> state) -> Task<td::Unit> {
      auto result = co_await std::move(task).wrap();
      std::optional<td::Status> fail_fast_error;
      bool set_ready_ok = false;
      {
        std::lock_guard<td::TinyMutex> guard(state->mutex);
        bool should_fail_fast = !state->ready_signaled && result.is_error() && result.error().code() != kCancelledCode;
        if (should_fail_fast) {
          fail_fast_error = result.error().clone();
        }

        CHECK(index < state->results.size());
        state->results[index] = std::move(result);

        CHECK(state->remaining > 0);
        state->remaining--;
        if (should_fail_fast) {
          state->ready_signaled = true;
        } else if (!state->ready_signaled && state->remaining == 0) {
          state->ready_signaled = true;
          set_ready_ok = true;
        }
      }

      if (fail_fast_error) {
        state->ready.set_error(std::move(*fail_fast_error));
      } else if (set_ready_ok) {
        state->ready.set_value(td::Unit{});
      }
      co_return td::Unit{};
    }(i, std::move(tasks[i]), state);
    group.start(std::move(waiter)).detach_silent();
  }

  auto ready = co_await std::move(ready_task).wrap();
  if (ready.is_error()) {
    group.cancel();
  }
  co_await group.join();
  if (ready.is_error()) {
    co_return ready.move_as_error();
  }

  std::vector<td::Result<T>> collected;
  collected.reserve(state->results.size());
  for (auto& result : state->results) {
    CHECK(result.has_value());
    collected.push_back(std::move(*result));
  }
  co_return collect(std::move(collected));
}

}  // namespace td::actor
