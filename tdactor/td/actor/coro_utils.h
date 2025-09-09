#pragma once

#include "td/actor/coro_types.h"
#include "td/actor/coro_executor.h"
#include "td/actor/coro_task.h"
#include "td/actor/actor.h"
#include "td/utils/Status.h"
#include "td/utils/Slice.h"

#include <tuple>
#include <vector>
#include <type_traits>
#include <utility>

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

template <class... Awaitables, std::enable_if_t<(sizeof...(Awaitables) > 1), int> = 0>
auto all(Awaitables&&... awaitables) -> Task<std::tuple<await_result_t<Awaitables>...>> {
  co_await become_lightweight();
  co_return std::tuple<await_result_t<Awaitables>...>{co_await std::forward<Awaitables>(awaitables)...};
}

template <class... Awaitables, std::enable_if_t<(sizeof...(Awaitables) > 1), int> = 0>
auto all_wrap(Awaitables&&... awaitables) -> Task<std::tuple<await_result_t<Wrapped<Awaitables>>...>> {
  co_await become_lightweight();
  co_return std::tuple<await_result_t<Wrapped<Awaitables>>...>{
      co_await Wrapped{std::forward<Awaitables>(awaitables)}...};
}

template <CoroTask TaskType>
Task<std::vector<await_result_t<TaskType>>> all(std::vector<TaskType> tasks) {
  co_await become_lightweight();
  std::vector<await_result_t<TaskType>> results;
  results.reserve(tasks.size());
  // TODO: auto start
  for (auto& task : tasks) {
    results.push_back(co_await std::move(task));
  }
  co_return results;
}

template <CoroTask TaskType>
Task<std::vector<await_result_t<Wrapped<TaskType>>>> all_wrap(std::vector<TaskType> tasks) {
  co_await become_lightweight();
  std::vector<await_result_t<Wrapped<TaskType>>> results;
  // TODO: auto start
  results.reserve(tasks.size());
  for (auto& task : tasks) {
    results.push_back(co_await Wrapped<TaskType>{std::move(task)});
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

template <bool Later, class TargetId, class MemFn, class... Args>
auto ask_impl(TargetId&& to, MemFn mf, Args&&... args) {
  using Meta = unified_result<MemFn>;
  using T = Meta::type;
  using Ret = detail::memfn_meta<MemFn>::ret;

  using TT = std::conditional_t<std::is_void_v<T>, td::Unit, T>;

  static_assert(Meta::kind == UnifiedKind::TaskReturn || Meta::kind == UnifiedKind::PromiseArgument ||
                    Meta::kind == UnifiedKind::ReturnValue || Meta::kind == UnifiedKind::Void,
                "ask: method must return Task<T> or take td::Promise<T> as last parameter");

  if constexpr (Meta::kind == UnifiedKind::TaskReturn) {
    return ask_new_impl<Later>(std::forward<TargetId>(to), mf, std::forward<Args>(args)...);
  }

  auto [task, promise] = StartedTask<TT>::make_bridge();
  td::actor::internal::send_closure_dispatch<Later>(std::forward<TargetId>(to), mf, std::forward<Args>(args)...,
                                                    std::move(promise));
  return std::move(task);
}

template <bool Later, class TargetId, class MemFn, class... Args>
auto ask_new_impl(TargetId&& to, MemFn mf, Args&&... args) {
  using Meta = unified_result<MemFn>;
  using T = Meta::type;
  static_assert(Meta::kind == UnifiedKind::TaskReturn, "ask: method must return Task<T>");
  if constexpr (Later) {
    auto task = [](auto closure) -> Task<T> {
      co_return co_await detail::run_on_current_actor(closure);
    }(create_delayed_closure(mf, std::forward<Args>(args)...));
    task.set_executor(Executor::on_actor(to));
    return std::move(task).start();
  } else {
    std::optional<StartedTask<T>> o_task;
    td::actor::detail::send_immediate(
        to.as_actor_ref(),
        [&] {
          o_task.emplace(detail::run_on_current_actor(create_immediate_closure(mf, std::forward<Args>(args)...))
                             .start_immediate());
        },
        [&]() {
          auto task = [](auto closure) -> Task<T> {
            co_return co_await detail::run_on_current_actor(closure);
          }(create_delayed_closure(mf, std::forward<Args>(args)...));
          task.set_executor(Executor::on_actor(to));
          o_task.emplace(std::move(task).start_external());
          return detail::ActorExecutor::to_message(o_task->h);
        });
    return std::move(*o_task);
  }
}

template <class... Args>
auto ask_new(Args&&... args) {
  return ask_new_impl<true>(std::forward<Args>(args)...);
}

template <class... Args>
auto ask_new_immediate(Args&&... args) {
  return ask_new_impl<false>(std::forward<Args>(args)...);
}

template <class... Args>
auto ask(Args&&... args) {
  return ask_impl<true>(std::forward<Args>(args)...);
}

template <class... Args>
auto ask_immediate(Args&&... args) {
  return ask_impl<false>(std::forward<Args>(args)...);
}

template <class... Args>
auto ask_promise(Args&&... args) {
  return ask(std::forward<Args>(args)...);
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

}  // namespace td::actor
