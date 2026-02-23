#include <atomic>
#include <numeric>
#include <vector>

#include "td/utils/Mutex.h"
#include "td/utils/port/thread.h"
#include "td/utils/tests.h"

#if !TD_THREAD_UNSUPPORTED

TEST(Mutex, basic) {
  td::TinyMutex m;
  m.lock();
  m.unlock();
}

TEST(Mutex, try_lock) {
  td::TinyMutex m;
  ASSERT_TRUE(m.try_lock());
  ASSERT_TRUE(!m.try_lock());
  m.unlock();
  ASSERT_TRUE(m.try_lock());
  m.unlock();
}

TEST(Mutex, two_threads) {
  td::TinyMutex m;
  std::atomic<int> counter{0};
  constexpr int N = 100000;

  td::thread t1([&] {
    for (int i = 0; i < N; i++) {
      m.lock();
      counter.fetch_add(1, std::memory_order_relaxed);
      m.unlock();
    }
  });
  td::thread t2([&] {
    for (int i = 0; i < N; i++) {
      m.lock();
      counter.fetch_add(1, std::memory_order_relaxed);
      m.unlock();
    }
  });

  t1.join();
  t2.join();
  ASSERT_EQ(2 * N, counter.load());
}

TEST(Mutex, many_threads) {
  td::TinyMutex m;
  std::atomic<int> counter{0};
  constexpr int threads_n = 8;
  constexpr int N = 50000;

  std::vector<td::thread> threads;
  for (int t = 0; t < threads_n; t++) {
    threads.push_back(td::thread([&] {
      for (int i = 0; i < N; i++) {
        m.lock();
        counter.fetch_add(1, std::memory_order_relaxed);
        m.unlock();
      }
    }));
  }
  for (auto &th : threads) {
    th.join();
  }
  ASSERT_EQ(threads_n * N, counter.load());
}

TEST(Mutex, protects_data) {
  td::TinyMutex m;
  int shared = 0;
  constexpr int threads_n = 4;
  constexpr int N = 100000;

  std::vector<td::thread> threads;
  for (int t = 0; t < threads_n; t++) {
    threads.push_back(td::thread([&] {
      for (int i = 0; i < N; i++) {
        m.lock();
        shared++;
        m.unlock();
      }
    }));
  }
  for (auto &th : threads) {
    th.join();
  }
  ASSERT_EQ(threads_n * N, shared);
}

#endif  // !TD_THREAD_UNSUPPORTED
