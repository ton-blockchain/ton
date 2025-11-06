#pragma once

#include <concepts>
#include <coroutine>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace td::actor {

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

namespace detail {
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

inline uintptr_t encode_continuation(std::coroutine_handle<> h) noexcept {
  auto p = h.address();
  auto v = reinterpret_cast<uintptr_t>(p);
  return v | 1u;
}

inline std::coroutine_handle<> decode_continuation(uintptr_t token) noexcept {
  return std::coroutine_handle<>::from_address(reinterpret_cast<void*>(token & ~uintptr_t(1)));
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

struct [[nodiscard]] Yield {};

}  // namespace td::actor
