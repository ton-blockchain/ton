#pragma once

#include <coroutine>
#include <cstdint>
#include <variant>

#include "td/actor/ActorRef.h"
#include "td/actor/actor.h"
#include "td/actor/core/SchedulerContext.h"
#include "td/actor/core/SchedulerId.h"
#include "td/actor/coro_types.h"

namespace td::actor {

namespace detail {

inline ActorId<> get_current_actor_id() noexcept {
  auto context = core::ActorExecuteContext::get_ptr();
  if (context && context->actor_ptr()) {
    return actor_id(context->actor_ptr());
  }
  return td::actor::ActorId<>{};
}

template <class P>
[[nodiscard]] std::coroutine_handle<> continue_with_tls(std::coroutine_handle<P> cont) noexcept {
  set_current_ctrl(cont);
  return cont;
}

template <class P>
[[nodiscard]] std::coroutine_handle<> fail_actor_destroyed(std::coroutine_handle<P> cont) noexcept {
  return cont.promise().route_finish(td::Status::Error("Actor destroyed"));
}

template <class P>
class ActorMessageCoroutineSafe : public core::ActorMessageImpl {
 public:
  explicit ActorMessageCoroutineSafe(std::coroutine_handle<P> continuation) : continuation_(continuation) {
  }
  void run() override {
    resume_with_own_promise(continuation_);
    continuation_ = {};
  }
  ~ActorMessageCoroutineSafe() override;

 private:
  std::coroutine_handle<P> continuation_;
};

// Executor
// - schedule (sometime, NOT right now)
// - execute_or_schedule  - it is ok to start executing immediately
//    - for any just execute
//    - for scheduler just execute
//    - for actor try to execute immediately and schedule if can't
// - resume_or_schedule - if we are already executing we may continue otherwise schedule.

struct ActorExecutor {
  td::actor::ActorRef<> actor_ref;

  bool is_immediate_execution_allowed() const noexcept {
    if (actor_ref.empty()) {
      return false;
    }
    auto* current = core::ActorExecuteContext::get_ptr();
    return current && current->actor_ptr() == actor_ref.actor_info().actor_ptr();
  }
  bool is_immediate_execution_always_allowed() const noexcept {
    return false;
  }
  template <class P>
  [[nodiscard]] std::coroutine_handle<> resume_or_schedule(std::coroutine_handle<P> cont) noexcept {
    if (actor_ref.empty()) {
      return fail_actor_destroyed(cont);
    }
    if (is_immediate_execution_allowed()) {
      return continue_with_tls(cont);
    }
    schedule(std::move(cont));
    return std::noop_coroutine();
  }
  template <class P>
  static auto to_message(std::coroutine_handle<P> cont) noexcept {
    return td::actor::core::ActorMessage{std::make_unique<ActorMessageCoroutineSafe<P>>(std::move(cont))};
  }
  template <class P>
  [[nodiscard]] std::coroutine_handle<> execute_or_schedule(std::coroutine_handle<P> cont) noexcept {
    if (actor_ref.empty()) {
      return fail_actor_destroyed(cont);
    }
    if (is_immediate_execution_allowed()) {
      return continue_with_tls(cont);
    }
    td::actor::detail::send_immediate(
        actor_ref.as_actor_ref(), [&] { resume_with_own_promise(cont); },
        [&]() { return to_message(std::move(cont)); });
    return std::noop_coroutine();
  }
  template <class P>
  void schedule(std::coroutine_handle<P> cont) noexcept {
    td::actor::detail::send_message_later(actor_ref.as_actor_ref(), to_message(std::move(cont)));
  }
};

struct SchedulerExecutor {
  bool is_immediate_execution_allowed() const noexcept {
    return get_current_actor_id().empty();
  }
  bool is_immediate_execution_always_allowed() const noexcept {
    return false;
  }
  template <class P>
  [[nodiscard]] std::coroutine_handle<> resume_or_schedule(std::coroutine_handle<P> cont) noexcept {
    return execute_or_schedule(std::move(cont));
  }
  template <class P>
  [[nodiscard]] std::coroutine_handle<> execute_or_schedule(std::coroutine_handle<P> cont) noexcept {
    if (is_immediate_execution_allowed()) {
      return continue_with_tls(cont);
    }
    schedule(std::move(cont));
    return std::noop_coroutine();
  }
  template <class P>
  void schedule(std::coroutine_handle<P> cont) noexcept {
    // Encode control block pointer so CpuWorker can set TLS before resuming
    auto* ctrl = cont.promise().control_;
    auto token = reinterpret_cast<td::actor::core::SchedulerToken>(encode_ctrl(ctrl));
    td::actor::core::SchedulerContext::get().add_token_to_cpu_queue(token, td::actor::core::SchedulerId{});
  }

  // Overload for type-erased handles - can't get promise, use handle-based encoding
  void schedule(std::coroutine_handle<> cont) noexcept {
    auto token = reinterpret_cast<td::actor::core::SchedulerToken>(encode_continuation(cont));
    td::actor::core::SchedulerContext::get().add_token_to_cpu_queue(token, td::actor::core::SchedulerId{});
  }
};

struct AnyExecutor {
  bool is_immediate_execution_allowed() const noexcept {
    return true;
  }
  bool is_immediate_execution_always_allowed() const noexcept {
    return true;
  }
  template <class P>
  [[nodiscard]] std::coroutine_handle<> resume_or_schedule(std::coroutine_handle<P> cont) noexcept {
    return execute_or_schedule(std::move(cont));
  }
  template <class P>
  [[nodiscard]] std::coroutine_handle<> execute_or_schedule(std::coroutine_handle<P> cont) noexcept {
    return continue_with_tls(cont);
  }
  template <class P>
  void schedule(std::coroutine_handle<P> cont) noexcept {
    SchedulerExecutor{}.schedule(cont);
  }
};

struct Executor {
  std::variant<AnyExecutor, ActorExecutor, SchedulerExecutor> executor_{SchedulerExecutor{}};

  static Executor on_actor(td::actor::ActorId<> actor) noexcept {
    return {ActorExecutor{td::actor::ActorRef<>::try_from(actor)}};
  }
  template <class T>
  static Executor on_actor(const td::actor::ActorOwn<T>& actor) noexcept {
    return on_actor(actor.get());
  }
  static Executor on_scheduler() noexcept {
    return {SchedulerExecutor{}};
  }
  static Executor on_any() noexcept {
    return {AnyExecutor{}};
  }
  static Executor on_current_actor() noexcept {
    return on_actor(get_current_actor_id());
  }
  static Executor on_default() noexcept {
    auto current_actor_id = get_current_actor_id();
    return current_actor_id.empty() ? on_scheduler() : on_actor(std::move(current_actor_id));
  }

  bool is_immediate_execution_allowed() const noexcept {
    return visit([](auto& v) { return v.is_immediate_execution_allowed(); }, executor_);
  }
  bool is_immediate_execution_always_allowed() const noexcept {
    return visit([](auto& v) { return v.is_immediate_execution_always_allowed(); }, executor_);
  }
  template <class P>
  [[nodiscard]] std::coroutine_handle<> resume_or_schedule(std::coroutine_handle<P> cont) noexcept {
    return visit([&](auto& v) { return v.resume_or_schedule(std::move(cont)); }, executor_);
  }
  template <class P>
  [[nodiscard]] std::coroutine_handle<> execute_or_schedule(std::coroutine_handle<P> cont) noexcept {
    return visit([&](auto& v) { return v.execute_or_schedule(std::move(cont)); }, executor_);
  }

  template <class P>
  void schedule(std::coroutine_handle<P> cont) noexcept {
    return visit([&](auto& v) { return v.schedule(std::move(cont)); }, executor_);
  }

  ActorRef<> actor_ref_or_empty() const noexcept {
    if (auto* actor_executor = std::get_if<ActorExecutor>(&executor_)) {
      return actor_executor->actor_ref;
    }
    return {};
  }
};

template <class P>
ActorMessageCoroutineSafe<P>::~ActorMessageCoroutineSafe() {
  if (continuation_) {
    SchedulerExecutor{}.schedule(continuation_.promise().route_finish(td::Status::Error("Actor destroyed")));
  }
}

struct ResumeOn {
  Executor executor;

  bool await_ready() noexcept {
    return executor.is_immediate_execution_allowed();
  }

  template <class P>
  std::coroutine_handle<> await_suspend(std::coroutine_handle<P> cont) noexcept {
    return executor.resume_or_schedule(std::move(cont));
  }

  void await_resume() noexcept {
  }
};

struct YieldOn {
  Executor executor;
  bool await_ready() noexcept {
    return false;
  }
  template <class P>
  std::coroutine_handle<> await_suspend(std::coroutine_handle<P> self) noexcept {
    // Check cancellation before yielding - if cancelled, abort with error
    if constexpr (requires { self.promise().finish_if_cancelled(); }) {
      if (auto h = self.promise().finish_if_cancelled()) {
        return *h;
      }
    }
    executor.schedule(std::move(self));
    return std::noop_coroutine();
  }
  void await_resume() noexcept {
  }
};

}  // namespace detail

using Executor = detail::Executor;

[[nodiscard]] inline auto resume_on(Executor executor) noexcept {
  return detail::ResumeOn{std::move(executor)};
}

[[nodiscard]] inline auto yield_on(Executor executor) noexcept {
  return detail::YieldOn{std::move(executor)};
}

inline auto attach_to_actor(td::actor::ActorId<> actor_id) noexcept {
  return detail::ResumeOn{Executor::on_actor(actor_id)};
}

inline auto detach_from_actor() noexcept {
  return detail::ResumeOn{Executor::on_scheduler()};
}

inline auto become_lightweight() noexcept {
  return detail::ResumeOn{Executor::on_any()};
}

inline Yield yield_on_current() noexcept {
  return Yield{};
}

}  // namespace td::actor
