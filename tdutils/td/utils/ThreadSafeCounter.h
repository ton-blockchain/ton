/*
    This file is part of TON Blockchain Library.

    TON Blockchain Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    TON Blockchain Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with TON Blockchain Library.  If not, see <http://www.gnu.org/licenses/>.

    Copyright 2017-2020 Telegram Systems LLP
*/

#pragma once

#include "td/utils/common.h"
#include "td/utils/Slice.h"
#include "td/utils/StringBuilder.h"
#include "td/utils/ThreadLocalStorage.h"

#include <array>
#include <atomic>
#include <mutex>

namespace td {
template <size_t N>
class ThreadSafeMultiCounter {
 public:
  void add(size_t index, int64 diff) {
    CHECK(index < N);
    tls_.get()[index].fetch_add(diff, std::memory_order_relaxed);
  }

  int64 sum(size_t index) const {
    CHECK(index < N);
    int64 res = 0;
    tls_.for_each([&](auto &value) { res += value[index].load(std::memory_order_relaxed); });
    return res;
  }
  void clear() {
    tls_.for_each([&](auto &value) {
      for (auto &x : value) {
        x = 0;
      }
    });
  }

 private:
  ThreadLocalStorage<std::array<std::atomic<int64>, N>> tls_;
};

class ThreadSafeCounter {
 public:
  void add(int64 diff) {
    counter_.add(0, diff);
  }

  int64 sum() const {
    return counter_.sum(0);
  }

 private:
  ThreadSafeMultiCounter<1> counter_;
};

class NamedThreadSafeCounter {
  static constexpr int N = 128;
  using Counter = ThreadSafeMultiCounter<N>;

 public:
  class CounterRef {
   public:
    CounterRef() = default;
    CounterRef(size_t index, Counter *counter) : index_(index), counter_(counter) {
    }
    void add(int64 diff) {
      counter_->add(index_, diff);
    }
    int64 sum() const {
      return counter_->sum(index_);
    }

   private:
    size_t index_{0};
    Counter *counter_{nullptr};
  };

  CounterRef get_counter(Slice name) {
    std::unique_lock<std::mutex> guard(mutex_);
    for (size_t i = 0; i < names_.size(); i++) {
      if (names_[i] == name) {
        return get_counter_ref(i);
      }
    }
    CHECK(names_.size() < N);
    names_.emplace_back(name.begin(), name.size());
    return get_counter_ref(names_.size() - 1);
  }

  CounterRef get_counter_ref(size_t index) {
    return CounterRef(index, &counter_);
  }

  static NamedThreadSafeCounter &get_default() {
    static NamedThreadSafeCounter res;
    return res;
  }

  template <class F>
  void for_each(F &&f) const {
    std::unique_lock<std::mutex> guard(mutex_);
    for (size_t i = 0; i < names_.size(); i++) {
      f(names_[i], counter_.sum(i));
    }
  }

  void clear() {
    std::unique_lock<std::mutex> guard(mutex_);
    counter_.clear();
  }

  friend StringBuilder &operator<<(StringBuilder &sb, const NamedThreadSafeCounter &counter) {
    counter.for_each([&sb](Slice name, int64 cnt) { sb << name << ": " << cnt << "\n"; });
    return sb;
  }

 private:
  mutable std::mutex mutex_;
  std::vector<std::string> names_;

  Counter counter_;
};

// another class for simplicity, it
struct NamedPerfCounter {
 public:
  static NamedPerfCounter &get_default() {
    static NamedPerfCounter res;
    return res;
  }
  struct PerfCounterRef {
    NamedThreadSafeCounter::CounterRef count;
    NamedThreadSafeCounter::CounterRef duration;
  };
  PerfCounterRef get_counter(Slice name) {
    return {.count = counter_.get_counter(PSLICE() << name << ".count"),
            .duration = counter_.get_counter(PSLICE() << name << ".duration")};
  }

  struct ScopedPerfCounterRef : public NoCopyOrMove {
    PerfCounterRef perf_counter;
    uint64 started_at_ticks{td::Clocks::rdtsc()};

    ~ScopedPerfCounterRef() {
      perf_counter.count.add(1);
      perf_counter.duration.add(td::Clocks::rdtsc() - started_at_ticks);
    }
  };

  template <class F>
  void for_each(F &&f) const {
    counter_.for_each(f);
  }

  void clear() {
    counter_.clear();
  }

  friend StringBuilder &operator<<(StringBuilder &sb, const NamedPerfCounter &counter) {
    return sb << counter.counter_;
  }
 private:
  NamedThreadSafeCounter counter_;
};

}  // namespace td

#define TD_PERF_COUNTER(name)                                                    \
  static auto perf_##name = td::NamedPerfCounter::get_default().get_counter(td::Slice(#name)); \
  auto scoped_perf_##name = td::NamedPerfCounter::ScopedPerfCounterRef{.perf_counter = perf_##name};

#define TD_PERF_COUNTER_SINCE(name, since)                                       \
  static auto perf_##name = td::NamedPerfCounter::get_default().get_counter(td::Slice(#name)); \
  auto scoped_perf_##name =                                                      \
      td::NamedPerfCounter::ScopedPerfCounterRef{.perf_counter = perf_##name, .started_at_ticks = since};
