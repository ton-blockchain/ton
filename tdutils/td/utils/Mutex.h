#pragma once

#include <atomic>

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define TD_TSAN 1
#endif
#endif

#ifdef TD_TSAN
#define TD_TSAN_MUTEX_CREATE(addr) AnnotateRWLockCreate(__FILE__, __LINE__, addr)
#define TD_TSAN_MUTEX_DESTROY(addr) AnnotateRWLockDestroy(__FILE__, __LINE__, addr)
#define TD_TSAN_MUTEX_LOCK(addr) AnnotateRWLockAcquired(__FILE__, __LINE__, addr, 1)
#define TD_TSAN_MUTEX_UNLOCK(addr) AnnotateRWLockReleased(__FILE__, __LINE__, addr, 1)
extern "C" {
void AnnotateRWLockCreate(const char* f, int l, const volatile void* addr);
void AnnotateRWLockDestroy(const char* f, int l, const volatile void* addr);
void AnnotateRWLockAcquired(const char* f, int l, const volatile void* addr, long is_w);
void AnnotateRWLockReleased(const char* f, int l, const volatile void* addr, long is_w);
}
#else
#define TD_TSAN_MUTEX_CREATE(addr)
#define TD_TSAN_MUTEX_DESTROY(addr)
#define TD_TSAN_MUTEX_LOCK(addr)
#define TD_TSAN_MUTEX_UNLOCK(addr)
#endif

namespace td {

// 3-state futex-style mutex using C++20 atomic wait/notify.
//   0 = unlocked
//   1 = locked, no known waiters
//   2 = locked, contended (may have waiters)
class TinyMutex {
 public:
  TinyMutex() noexcept {
    TD_TSAN_MUTEX_CREATE(this);
  }

  ~TinyMutex() noexcept {
    TD_TSAN_MUTEX_DESTROY(this);
  }

  TinyMutex(const TinyMutex&) = delete;
  TinyMutex& operator=(const TinyMutex&) = delete;

  void lock() noexcept {
    int expected = 0;
    if (state_.compare_exchange_strong(expected, 1, std::memory_order_acquire, std::memory_order_relaxed)) {
      TD_TSAN_MUTEX_LOCK(this);
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
    TD_TSAN_MUTEX_LOCK(this);
  }

  bool try_lock() noexcept {
    int expected = 0;
    if (state_.compare_exchange_strong(expected, 1, std::memory_order_acquire, std::memory_order_relaxed)) {
      TD_TSAN_MUTEX_LOCK(this);
      return true;
    }
    return false;
  }

  void unlock() noexcept {
    TD_TSAN_MUTEX_UNLOCK(this);
    if (state_.fetch_sub(1, std::memory_order_release) != 1) {
      state_.store(0, std::memory_order_release);
      state_.notify_one();
    }
  }

 private:
  std::atomic<int> state_{0};
};

}  // namespace td
