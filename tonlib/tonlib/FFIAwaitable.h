#pragma once

#include <concepts>

#include "FFIEventLoop.h"

namespace tonlib {

template <typename T>
class FFIAwaitable {
 public:
  static FFIAwaitable *create_resolved(FFIEventLoop &loop, td::Result<T> value) {
    std::shared_ptr<FFIAwaitable> awaitable{new FFIAwaitable(loop, resolved_tag, std::move(value))};
    awaitable->self_ = awaitable;
    return awaitable.get();
  }

  template <typename U>
  struct Bridge {
    FFIAwaitable *awaitable;
    td::Promise<U> promise;
  };

  template <typename U, typename F>
  static Bridge<U> create_bridge(FFIEventLoop &loop, F &&transform)
    requires requires(U &u) {
      { transform(std::move(u)) } -> std::convertible_to<T>;
    }
  {
    std::shared_ptr<FFIAwaitable> awaitable{new FFIAwaitable(loop, nullptr, {})};
    awaitable->self_ = awaitable;

    td::Promise<U> promise =
        td::lambda_promise([awaitable, transform = std::forward<F>(transform)](td::Result<U> result) mutable {
          if (result.is_error()) {
            awaitable->result_ = result.move_as_error();
          } else {
            awaitable->result_ = transform(result.move_as_ok());
          }
          auto maybe_continuation = awaitable->continuation_.exchange(resolved_tag);
          if (maybe_continuation != nullptr && maybe_continuation != resolved_tag) {
            awaitable->loop_.put({maybe_continuation});
          }
        });
    return Bridge<U>{.awaitable = awaitable.get(), .promise = std::move(promise)};
  }

  FFIAwaitable(const FFIAwaitable &) = delete;
  FFIAwaitable(FFIAwaitable &&) = delete;
  FFIAwaitable &operator=(const FFIAwaitable &) = delete;
  FFIAwaitable &operator=(FFIAwaitable &&) = delete;

  ~FFIAwaitable() {
    CHECK(continuation_ == resolved_tag);
  }

  std::shared_ptr<FFIAwaitable> destroy() {
    CHECK(self_);
    auto maybe_continuation = continuation_.exchange(resolved_tag);
    if (maybe_continuation != nullptr && maybe_continuation != resolved_tag) {
      loop_.put({maybe_continuation});
    }
    return std::move(self_);
  }

  bool await_ready() {
    return continuation_ == resolved_tag;
  }

  void await_suspend(Continuation continuation) {
    void const *expected = nullptr;
    if (!continuation_.compare_exchange_strong(expected, continuation.value)) {
      CHECK(expected == resolved_tag);
      loop_.put(continuation);
    }
  }

  td::Result<T> &result() & {
    CHECK(continuation_ == resolved_tag);
    return result_;
  }

 private:
  static constexpr struct ResolvedTag {
  } resolved_tag_obj;
  static constexpr void const *resolved_tag = &resolved_tag_obj;

  FFIAwaitable(FFIEventLoop &loop, void const *continuation, td::Result<T> value)
      : loop_(loop), continuation_(continuation), result_(std::move(value)) {
  }

  FFIEventLoop &loop_;
  std::atomic<void const *> continuation_;

  td::Result<T> result_;

  std::shared_ptr<FFIAwaitable> self_;
};

}  // namespace tonlib
