#pragma once

#include <concepts>
#include <coroutine>
#include <cstdint>
#include <type_traits>
#include <utility>

#include "td/utils/Status.h"

namespace td::actor {

inline constexpr int kCancelledCode = 653;

inline td::Status cancelled_status() {
  return td::Status::Error(kCancelledCode, "cancelled");
}

template <class R>
concept TDResultLike = requires(R r) {
  { r.is_error() } -> std::convertible_to<bool>;
  r.move_as_ok();
  r.move_as_error();
};

template <typename T>
concept IsAwaitable = requires(T& t, std::coroutine_handle<> h) {
  t.await_suspend(h);
  t.await_resume();
};

template <typename T>
concept CoroTask = requires(T t) {
  typename T::promise_type;
  typename T::value_type;
  { t.await_suspend(std::coroutine_handle<>{}) };
  { t.await_resume() };
};

// Forward declarations for structured concurrency
struct promise_common;

namespace detail {

// Thread-local current promise for structured concurrency
inline thread_local promise_common* current_promise_ = nullptr;

inline promise_common* get_current_promise() {
  return current_promise_;
}

inline void set_current_promise(promise_common* p) {
  current_promise_ = p;
}

template <class P>
inline promise_common* to_promise_common(std::coroutine_handle<P> h) noexcept {
  return static_cast<promise_common*>(&h.promise());
}

template <class P>
inline void set_current_promise(std::coroutine_handle<P> h) noexcept {
  set_current_promise(to_promise_common(h));
}

// RAII guard for TLS - saves current value and restores on destruction
// Use this around .resume() calls to prevent stale TLS from leaking
class TlsGuard {
  promise_common* old_value_;

 public:
  explicit TlsGuard(promise_common* new_value) : old_value_(current_promise_) {
    current_promise_ = new_value;
  }
  ~TlsGuard() {
    current_promise_ = old_value_;
  }
  TlsGuard(const TlsGuard&) = delete;
  TlsGuard& operator=(const TlsGuard&) = delete;
};

// Centralized raw-resume helpers.
// Callers should not call .resume() directly; use one of these wrappers.
template <class P>
inline void resume_with_tls(std::coroutine_handle<P> h, promise_common* promise) noexcept {
  TlsGuard guard(promise);
  h.resume();
}

inline void resume_with_tls(std::coroutine_handle<> h, promise_common* promise) noexcept {
  TlsGuard guard(promise);
  h.resume();
}

template <class P>
inline void resume_with_own_promise(std::coroutine_handle<P> h) noexcept {
  resume_with_tls(h, to_promise_common(h));
}

template <class P>
inline void resume_on_current_tls(std::coroutine_handle<P> h) noexcept {
  resume_with_tls(h, get_current_promise());
}

inline void resume_root(std::coroutine_handle<> h) noexcept {
  resume_with_tls(h, nullptr);
}

template <typename T>
struct UnwrapTDResult {
  using Type = T;
};

template <TDResultLike T>
struct UnwrapTDResult<T> {
  using Type = decltype(std::declval<T>().move_as_ok());
};

template <class Aw>
inline std::coroutine_handle<> await_suspend_to(Aw& aw, std::coroutine_handle<> cont) noexcept {
  if constexpr (std::is_same_v<decltype(aw.await_suspend(cont)), void>) {
    aw.await_suspend(cont);
    return std::noop_coroutine();
  } else if constexpr (std::is_same_v<decltype(aw.await_suspend(cont)), bool>) {
    if (!aw.await_suspend(cont)) {
      return cont;
    }
    return std::noop_coroutine();
  } else {
    return aw.await_suspend(cont);
  }
}

struct FireAndForget {
  struct promise_type {
    FireAndForget get_return_object() noexcept {
      return {};
    }
    std::suspend_never initial_suspend() noexcept {
      return {};
    }
    std::suspend_never final_suspend() noexcept {
      return {};
    }
    void return_void() noexcept {
    }
    void unhandled_exception() noexcept {
    }
  };
};

template <class T>
struct ReadyAwaitable {
  [[no_unique_address]] T value;
  constexpr bool await_ready() const noexcept {
    return true;
  }
  constexpr void await_suspend(std::coroutine_handle<>) const noexcept {
  }
  T await_resume() noexcept {
    return std::move(value);
  }
};

template <class X>
auto make_awaitable(X&& x) {
  using DX = std::decay_t<X>;
  if constexpr (IsAwaitable<DX>) {
    return std::forward<X>(x);
  } else {
    return ReadyAwaitable<DX>{std::forward<X>(x)};
  }
}

// Token encoding scheme for scheduler queue:
// - Bit 0 = 1: coroutine token (vs actor message at 0)
// - Bit 1 = 0: handle-encoded (just coroutine handle, no TLS info)
// - Bit 1 = 1: promise-encoded (promise_common*, can set TLS)

inline uintptr_t encode_continuation(std::coroutine_handle<> h) noexcept {
  auto p = h.address();
  auto v = reinterpret_cast<uintptr_t>(p);
  return v | 1u;  // Bits: ...01 = handle-encoded continuation
}

inline uintptr_t encode_promise(promise_common* p) noexcept {
  auto v = reinterpret_cast<uintptr_t>(p);
  return v | 3u;  // Bits: ...11 = promise-encoded continuation
}

inline bool is_promise_encoded(uintptr_t token) noexcept {
  return (token & 3u) == 3u;
}

inline std::coroutine_handle<> decode_continuation(uintptr_t token) noexcept {
  return std::coroutine_handle<>::from_address(reinterpret_cast<void*>(token & ~uintptr_t(1)));
}

inline promise_common* decode_promise(uintptr_t token) noexcept {
  return reinterpret_cast<promise_common*>(token & ~uintptr_t(3));
}

}  // namespace detail

template <class T>
struct [[nodiscard]] SkipAwaitTransform {
  [[no_unique_address]] T awaitable;
};

template <class T>
struct Wrapped {
  [[no_unique_address]] T value;
};

template <class T>
struct Traced {
  [[no_unique_address]] T value;
  std::string trace;
};

struct [[nodiscard]] Yield {};

}  // namespace td::actor
