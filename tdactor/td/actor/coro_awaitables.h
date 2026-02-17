#pragma once

#include <coroutine>
#include <optional>
#include <type_traits>
#include <utility>

#include "td/actor/coro_executor.h"
#include "td/actor/coro_types.h"
#include "td/utils/Status.h"

namespace td::actor {

struct promise_common;

namespace bridge {
bool should_finish_due_to_cancellation(const promise_common& self);

inline bool should_finish_due_to_cancellation_tls() {
  auto* p = detail::get_current_promise();
  return p && should_finish_due_to_cancellation(*p);
}
}  // namespace bridge

namespace detail {

template <class OuterPromise, class Body>
struct WrappedCoroutine {
  struct promise_type {
    Body* body{};
    std::coroutine_handle<OuterPromise> outer{};

    promise_type(Body* b, std::coroutine_handle<OuterPromise> o) noexcept : body(b), outer(o) {
    }

    WrappedCoroutine get_return_object() noexcept {
      return WrappedCoroutine{std::coroutine_handle<promise_type>::from_promise(*this)};
    }
    std::suspend_always initial_suspend() noexcept {
      return {};
    }
    auto final_suspend() noexcept {
      struct A {
        promise_type* p;
        bool await_ready() noexcept {
          return false;
        }
        std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> self) noexcept {
          // Set TLS to outer promise before resuming it (for structured concurrency)
          set_current_promise(p->outer);
          auto next = p->body->route_resume(p->outer);
          self.destroy();
          return next;
        }
        void await_resume() noexcept {
        }
      };
      return A{this};
    }
    void return_void() noexcept {
    }
    void unhandled_exception() noexcept {
      std::terminate();
    }
  };

  using handle = std::coroutine_handle<promise_type>;
  handle h{};
  explicit WrappedCoroutine(handle hh) : h(hh) {
  }
  ~WrappedCoroutine() {
    if (h)
      h.destroy();
  }
  WrappedCoroutine& operator=(WrappedCoroutine&& o) = delete;
  WrappedCoroutine(const WrappedCoroutine&) = delete;
  WrappedCoroutine& operator=(const WrappedCoroutine&) = delete;
};

template <class BodyT, class OuterPromiseT>
[[nodiscard]] WrappedCoroutine<OuterPromiseT, BodyT> make_wrapped_coroutine(
    BodyT* b, std::coroutine_handle<OuterPromiseT> o) noexcept {
  co_return;
}

template <class BodyT, class OuterPromiseT>
[[nodiscard]] std::coroutine_handle<> wrap_coroutine(BodyT* body, std::coroutine_handle<OuterPromiseT> outer) noexcept {
  auto tmp = make_wrapped_coroutine(body, outer);
  return std::exchange(tmp.h, {});
}

template <class Aw>
inline constexpr bool has_peek = requires(Aw& a) { a.await_resume_peek(); };

template <class Body, class Aw, class OuterPromise>
std::coroutine_handle<> await_suspend_wrapped(Body* body, Aw& aw, std::coroutine_handle<OuterPromise> h) noexcept {
  if constexpr (requires { aw.await_ready(); }) {
    if (aw.await_ready()) {
      return body->route_resume(h);
    }
  }
  auto r_handle = wrap_coroutine(body, h);
  return await_suspend_to(aw, r_handle);
}

template <class Aw>
struct TaskUnwrapAwaiter {
  using A = std::remove_cvref_t<Aw>;
  using Res = decltype(std::declval<A&>().await_resume());
  using Ok = decltype(std::declval<Res&&>().move_as_ok());

  [[no_unique_address]] A aw;

  using Cache = std::conditional_t<has_peek<Aw>, td::Unit, std::optional<Ok>>;
  [[no_unique_address]] Cache ok_;

  bool await_ready() noexcept {
    if (bridge::should_finish_due_to_cancellation_tls())
      return false;
    if constexpr (has_peek<Aw>) {
      if (aw.await_ready() && !aw.await_resume_peek().is_error()) {
        return true;
      }
    }
    return false;
  }

  template <class OuterPromise>
  std::coroutine_handle<> route_resume(std::coroutine_handle<OuterPromise> h) noexcept {
    if constexpr (has_peek<Aw>) {
      const Res& r = aw.await_resume_peek();
      if (r.is_error()) {
        auto rr = aw.await_resume();
        return h.promise().route_finish(std::move(rr).move_as_error());
      }
      return h.promise().route_resume();
    } else {
      Res r = aw.await_resume();
      if (r.is_error()) {
        return h.promise().route_finish(std::move(r).move_as_error());
      }
      ok_.emplace(std::move(r).move_as_ok());
      return h.promise().route_resume();
    }
  }

  template <class OuterPromise>
  std::coroutine_handle<> await_suspend(std::coroutine_handle<OuterPromise> h) noexcept {
    return await_suspend_wrapped(this, aw, h);
  }

  Ok await_resume() noexcept {
    if constexpr (!has_peek<Aw>) {
      if (ok_) {
        return std::move(*ok_);
      }
    }
    Res r = aw.await_resume();
    return std::move(r).move_as_ok();
  }
};

template <class Aw>
struct TaskWrapAwaiter {
  using A = std::remove_cvref_t<Aw>;
  using Res = decltype(std::declval<A&>().await_resume());

  [[no_unique_address]] A aw;

  bool await_ready() noexcept {
    if (bridge::should_finish_due_to_cancellation_tls())
      return false;
    return aw.await_ready();
  }

  template <class OuterPromise>
  std::coroutine_handle<> route_resume(std::coroutine_handle<OuterPromise> h) noexcept {
    return h.promise().route_resume();
  }

  template <class OuterPromise>
  std::coroutine_handle<> await_suspend(std::coroutine_handle<OuterPromise> h) noexcept {
    return await_suspend_wrapped(this, aw, h);
  }

  Res await_resume() noexcept {
    return aw.await_resume();
  }
};

template <class Aw>
struct TaskTraceAwaiter {
  using A = std::remove_cvref_t<Aw>;
  using Res = decltype(std::declval<A&>().await_resume());
  using Ok = decltype(std::declval<Res&&>().move_as_ok());

  [[no_unique_address]] A aw;
  std::string trace;

  using Cache = std::conditional_t<has_peek<Aw>, td::Unit, std::optional<Ok>>;
  [[no_unique_address]] Cache ok_;

  bool await_ready() noexcept {
    if (bridge::should_finish_due_to_cancellation_tls())
      return false;
    if constexpr (has_peek<Aw>) {
      if (aw.await_ready() && !aw.await_resume_peek().is_error()) {
        return true;
      }
    }
    return false;
  }

  template <class OuterPromise>
  std::coroutine_handle<> route_resume(std::coroutine_handle<OuterPromise> h) noexcept {
    if constexpr (has_peek<Aw>) {
      const Res& r = aw.await_resume_peek();
      if (r.is_error()) {
        auto rr = aw.await_resume();
        return h.promise().route_finish(std::move(rr).move_as_error().trace(trace));
      }
      return h.promise().route_resume();
    } else {
      Res r = aw.await_resume();
      if (r.is_error()) {
        return h.promise().route_finish(std::move(r).move_as_error().trace(trace));
      }
      ok_.emplace(std::move(r).move_as_ok());
      return h.promise().route_resume();
    }
  }

  template <class OuterPromise>
  std::coroutine_handle<> await_suspend(std::coroutine_handle<OuterPromise> h) noexcept {
    return await_suspend_wrapped(this, aw, h);
  }

  Ok await_resume() noexcept {
    if constexpr (!has_peek<Aw>) {
      if (ok_) {
        return std::move(*ok_);
      }
    }
    Res r = aw.await_resume();
    return std::move(r).move_as_ok();
  }
};

template <class R>
struct ResultUnwrapAwaiter {
  using Res = std::remove_cvref_t<R>;
  using Ok = decltype(std::declval<Res&&>().move_as_ok());

  Res result;

  bool await_ready() noexcept {
    if (bridge::should_finish_due_to_cancellation_tls())
      return false;
    return result.is_ok();
  }

  template <class Promise>
  std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> h) noexcept {
    if (auto cancel_h = h.promise().finish_if_cancelled()) {
      return *cancel_h;
    }
    if (result.is_error()) {
      h.promise().return_value(std::move(result).move_as_error());
      return h.promise().final_suspend().await_suspend(h);
    }
    return h;
  }

  Ok await_resume() noexcept {
    return std::move(result).move_as_ok();
  }
};

template <class R>
struct ResultWrapAwaiter {
  using Res = std::remove_cvref_t<R>;

  Res result;

  bool await_ready() noexcept {
    if (bridge::should_finish_due_to_cancellation_tls())
      return false;
    return true;
  }

  template <class Promise>
  std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> h) noexcept {
    return h.promise().finish_if_cancelled().value_or(h);
  }

  Res await_resume() noexcept {
    return std::move(result);
  }
};

}  // namespace detail

// These helpers are used via await_transform to:
// 1) Ensure the awaiting coroutine resumes on the current task's scheduler.
// 2) Optionally unwrap td::Result<T>. If it's an error, propagate it to the parent
//    as if using co_return error; so `co_await get_error();` is equivalent to `co_return get_error();`.
template <IsAwaitable Aw>
[[nodiscard]] auto unwrap_and_resume_on_current(Aw&& aw_) noexcept {
  return detail::TaskUnwrapAwaiter<Aw>{std::forward<Aw>(aw_), {}};
}

template <IsAwaitable Aw>
[[nodiscard]] auto wrap_and_resume_on_current(Aw&& aw_) noexcept {
  return detail::TaskWrapAwaiter<Aw>{std::forward<Aw>(aw_)};
}

template <IsAwaitable Aw>
[[nodiscard]] auto trace_and_resume_on_current(Aw&& aw_, std::string trace) noexcept {
  return detail::TaskTraceAwaiter<Aw>{std::forward<Aw>(aw_), std::move(trace), {}};
}

template <class T>
[[nodiscard]] auto result_awaiter_unwrap(Result<T>&& r) noexcept {
  return detail::ResultUnwrapAwaiter<Result<T>>{std::move(r)};
}

template <class T>
[[nodiscard]] auto result_awaiter_wrap(Result<T>&& r) noexcept {
  return detail::ResultWrapAwaiter<Result<T>>{std::move(r)};
}

}  // namespace td::actor
