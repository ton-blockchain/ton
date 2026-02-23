#include <atomic>
#include <benchmark/benchmark.h>
#include <mutex>
#include <thread>
#include <vector>

#include "td/utils/Mutex.h"
#include "td/utils/SpinLock.h"

// Ticket lock (spin, FIFO) for comparison
class TicketLock {
 public:
  void lock() noexcept {
    auto my = next_.fetch_add(1, std::memory_order_relaxed);
    while (serving_.load(std::memory_order_acquire) != my) {
#if defined(__x86_64__) || defined(__i386__)
      __builtin_ia32_pause();
#endif
    }
  }

  void unlock() noexcept {
    serving_.store(serving_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
  }

 private:
  std::atomic<uint32_t> next_{0};
  std::atomic<uint32_t> serving_{0};
};

// --- Uncontended: single thread lock/unlock ---

template <class Mutex>
static void BM_Uncontended(benchmark::State &state) {
  Mutex m;
  for (auto _ : state) {
    m.lock();
    m.unlock();
  }
}

static void BM_Uncontended_SpinLock(benchmark::State &state) {
  td::SpinLock m;
  for (auto _ : state) {
    auto lock = m.lock();
  }
}

BENCHMARK(BM_Uncontended_SpinLock);
BENCHMARK_TEMPLATE(BM_Uncontended, td::Mutex);
BENCHMARK_TEMPLATE(BM_Uncontended, std::mutex);
BENCHMARK_TEMPLATE(BM_Uncontended, TicketLock);

// --- Contended: N threads increment a shared counter ---

template <class Mutex>
static void BM_Contended(benchmark::State &state) {
  static Mutex m;
  static std::atomic<int64_t> shared{0};

  if (state.thread_index() == 0) {
    shared.store(0, std::memory_order_relaxed);
  }

  for (auto _ : state) {
    m.lock();
    shared.fetch_add(1, std::memory_order_relaxed);
    m.unlock();
  }
}

static void BM_Contended_SpinLock(benchmark::State &state) {
  static td::SpinLock m;
  static std::atomic<int64_t> shared{0};

  if (state.thread_index() == 0) {
    shared.store(0, std::memory_order_relaxed);
  }

  for (auto _ : state) {
    auto lock = m.lock();
    shared.fetch_add(1, std::memory_order_relaxed);
  }
}

BENCHMARK(BM_Contended_SpinLock)->Threads(2)->Threads(4)->Threads(8);
BENCHMARK_TEMPLATE(BM_Contended, td::Mutex)->Threads(2)->Threads(4)->Threads(8);
BENCHMARK_TEMPLATE(BM_Contended, std::mutex)->Threads(2)->Threads(4)->Threads(8);
BENCHMARK_TEMPLATE(BM_Contended, TicketLock)->Threads(2)->Threads(4)->Threads(8);

// --- Contended with work: simulate a small critical section ---

template <class Mutex>
static void BM_ContendedWithWork(benchmark::State &state) {
  static Mutex m;
  static volatile int64_t shared;

  if (state.thread_index() == 0) {
    shared = 0;
  }

  for (auto _ : state) {
    m.lock();
    // small critical section: a few dependent ops
    auto v = shared;
    v = v + 1;
    shared = v;
    m.unlock();
  }
}

static void BM_ContendedWithWork_SpinLock(benchmark::State &state) {
  static td::SpinLock m;
  static volatile int64_t shared;

  if (state.thread_index() == 0) {
    shared = 0;
  }

  for (auto _ : state) {
    auto lock = m.lock();
    auto v = shared;
    v = v + 1;
    shared = v;
  }
}

BENCHMARK(BM_ContendedWithWork_SpinLock)->Threads(2)->Threads(4)->Threads(8);
BENCHMARK_TEMPLATE(BM_ContendedWithWork, td::Mutex)->Threads(2)->Threads(4)->Threads(8);
BENCHMARK_TEMPLATE(BM_ContendedWithWork, std::mutex)->Threads(2)->Threads(4)->Threads(8);
BENCHMARK_TEMPLATE(BM_ContendedWithWork, TicketLock)->Threads(2)->Threads(4)->Threads(8);

BENCHMARK_MAIN();
