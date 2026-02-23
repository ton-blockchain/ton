#pragma once

#include <atomic>

namespace td {

// 3-state futex-style mutex using C++20 atomic wait/notify.
//   0 = unlocked
//   1 = locked, no known waiters
//   2 = locked, contended (may have waiters)
class TinyMutex {
 public:
  void lock() noexcept {
    int expected = 0;
    if (state_.compare_exchange_strong(expected, 1, std::memory_order_acquire, std::memory_order_relaxed)) {
      return;
    }

    int c = expected;
    if (c != 2) {
      c = state_.exchange(2, std::memory_order_acquire);
    }
    while (c != 0) {
      state_.wait(2, std::memory_order_relaxed);
      c = state_.exchange(2, std::memory_order_acquire);
    }
  }

  bool try_lock() noexcept {
    int expected = 0;
    return state_.compare_exchange_strong(expected, 1, std::memory_order_acquire, std::memory_order_relaxed);
  }

  void unlock() noexcept {
    if (state_.fetch_sub(1, std::memory_order_release) != 1) {
      state_.store(0, std::memory_order_release);
      state_.notify_one();
    }
  }

 private:
  std::atomic<int> state_{0};
};

}  // namespace td
