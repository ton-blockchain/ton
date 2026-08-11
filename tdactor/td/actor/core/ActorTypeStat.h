#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <typeindex>
#include <typeinfo>
#include <vector>

#include "td/utils/common.h"
#include "td/utils/int_types.h"
#include "td/utils/port/Clocks.h"

namespace td {
namespace actor {
namespace core {
class Actor;
struct Debug;
struct SchedulerGroupInfo;

enum class WorkerKind : td::uint8 { Io, Cpu, Count };
inline constexpr size_t WORKER_KIND_COUNT = static_cast<size_t>(WorkerKind::Count);

inline td::uint64 elapsed_ticks(td::uint64 now, td::uint64 started_at) {
  return now >= started_at ? now - started_at : 0;
}

struct ActorTypeStat {
  // diff (speed)
  double created{0};
  double executions{0};
  double messages{0};
  double seconds{0};

  // current statistics
  td::int64 alive{0};
  td::int32 executing{0};
  double executing_start{1e20};

  // max statistics
  template <class T>
  struct MaxStatGroup {
    T value_forever{};
    T value_10s{};
    T value_10m{};
    MaxStatGroup &operator+=(const MaxStatGroup<T> &other) {
      value_forever = std::max(value_forever, other.value_forever);
      value_10s = std::max(value_10s, other.value_10s);
      value_10m = std::max(value_10m, other.value_10m);
      return *this;
    }
  };
  MaxStatGroup<td::uint32> max_execute_messages;
  MaxStatGroup<double> max_message_seconds;
  MaxStatGroup<double> max_execute_seconds;
  MaxStatGroup<double> max_delay_seconds;

  ActorTypeStat &operator+=(const ActorTypeStat &other) {
    created += other.created;
    executions += other.executions;
    messages += other.messages;
    seconds += other.seconds;

    alive += other.alive;
    executing += other.executing;
    executing_start = std::min(other.executing_start, executing_start);

    max_execute_messages += other.max_execute_messages;
    max_message_seconds += other.max_message_seconds;
    max_execute_seconds += other.max_execute_seconds;
    max_delay_seconds += other.max_delay_seconds;
    return *this;
  }

  ActorTypeStat &operator-=(const ActorTypeStat &other) {
    created -= other.created;
    executions -= other.executions;
    messages -= other.messages;
    seconds -= other.seconds;
    return *this;
  }
  ActorTypeStat &operator/=(double t) {
    if (t > 1e-2) {
      created /= t;
      executions /= t;
      messages /= t;
      seconds /= t;
    } else {
      created = 0;
      executions = 0;
      messages = 0;
      seconds = 0;
    }
    return *this;
  }
};

template <class ValueT, int Interval>
class RecentMaxCounter {
 public:
  void update(td::uint64 now, ValueT value) {
    if (unlikely(next_segment_at_ == 0 || now < segment_started_at_ || now >= next_segment_at_)) {
      update_segment(now);
    }
    auto &current = values_[current_segment_ & 1];
    auto old_value = current.load(std::memory_order_relaxed);
    if (value > old_value) {
      current.store(value, std::memory_order_relaxed);
    }
  }

  ValueT get_max(td::uint64 now) const {
    auto current_segment = now / interval_ticks();
    auto last_segment = last_segment_.load(std::memory_order_acquire);
    if (current_segment < last_segment) {
      return 0;
    }
    if (current_segment == last_segment) {
      return std::max(load(values_[0]), load(values_[1]));
    }
    if (current_segment - last_segment == 1) {
      return load(values_[last_segment & 1]);
    }
    return 0;
  }

 private:
  static ValueT load(const std::atomic<ValueT> &value) {
    return value.load(std::memory_order_relaxed);
  }
  static td::uint64 interval_ticks() {
    return Clocks::rdtsc_frequency() * Interval;
  }
  void update_segment(td::uint64 now) {
    auto ticks = interval_ticks();
    auto segment = now / ticks;
    auto last_segment = last_segment_.load(std::memory_order_relaxed);
    if (next_segment_at_ == 0 || segment < last_segment || segment - last_segment >= 2) {
      values_[0].store(0, std::memory_order_relaxed);
      values_[1].store(0, std::memory_order_relaxed);
    } else if (segment != last_segment) {
      values_[segment & 1].store(0, std::memory_order_relaxed);
    }
    current_segment_ = segment;
    segment_started_at_ = segment * ticks;
    next_segment_at_ = (segment + 1) * ticks;
    last_segment_.store(segment, std::memory_order_release);
  }

  alignas(64) std::atomic<ValueT> values_[2] = {0};
  std::atomic<td::uint64> last_segment_{0};
  td::uint64 current_segment_{0};
  td::uint64 segment_started_at_{0};
  td::uint64 next_segment_at_{0};
};

template <class ValueT>
class MaxCounterGroup {
 public:
  void update(td::uint64 now, ValueT value) {
    auto old_value = max_forever_.load(std::memory_order_relaxed);
    if (value > old_value) {
      max_forever_.store(value, std::memory_order_relaxed);
    }
    max_10s_.update(now, value);
    max_10m_.update(now, value);
  }

  ActorTypeStat::MaxStatGroup<ValueT> get(td::uint64 now) const {
    return {.value_forever = max_forever_.load(std::memory_order_relaxed),
            .value_10s = max_10s_.get_max(now),
            .value_10m = max_10m_.get_max(now)};
  }

 private:
  std::atomic<ValueT> max_forever_{0};
  RecentMaxCounter<ValueT, 10> max_10s_;
  RecentMaxCounter<ValueT, 10 * 60> max_10m_;
};

class CoroutineStat {
 public:
  void add(td::uint64 finished_at, td::uint64 elapsed_ticks);
  ActorTypeStat to_stat(td::uint64 now, double inv_ticks_per_second, td::uint64 active_since = 0) const;

 private:
  template <int Interval>
  bool has_recent_completion(td::uint64 now) const {
    if (count_ == 0) {
      return false;
    }
    if (now < last_finished_at_) {
      return false;
    }
    auto interval_ticks = Clocks::rdtsc_frequency() * Interval;
    auto current_segment = now / interval_ticks;
    auto last_segment = last_finished_at_ / interval_ticks;
    return current_segment <= last_segment || current_segment - last_segment == 1;
  }

  td::uint64 count_{0};
  td::uint64 total_ticks_{0};
  td::uint64 last_finished_at_{0};
  MaxCounterGroup<td::uint64> max_ticks_;
};

struct ActorTypeStatImpl {
 public:
  ActorTypeStatImpl() {
  }

  class MessageTimer {
   public:
    MessageTimer(ActorTypeStatImpl *stat, td::uint64 started_at = Clocks::rdtsc())
        : stat_(stat), started_at_(started_at) {
    }
    MessageTimer(const MessageTimer &) = delete;
    MessageTimer(MessageTimer &&) = delete;
    MessageTimer &operator=(const MessageTimer &) = delete;
    MessageTimer &operator=(MessageTimer &&) = delete;
    ~MessageTimer() {
      if (stat_) {
        auto ts = td::Clocks::rdtsc();
        stat_->message_finish(ts, elapsed_ticks(ts, started_at_));
      }
    }

   private:
    ActorTypeStatImpl *stat_;
    td::uint64 started_at_;
  };

  void created() {
    inc(total_created_);
    inc(alive_);
  }
  void destroyed() {
    dec(alive_);
  }
  MessageTimer create_run_timer() {
    return MessageTimer{this};
  }

  void message_finish(td::uint64 ts, td::uint64 ticks) {
    inc(total_messages_);
    inc(execute_messages_);
    add(total_ticks_, ticks);
    max_message_ticks_.update(ts, ticks);
  }
  void on_delay(td::uint64 ts, td::uint64 ticks) {
    max_delay_ticks_.update(ts, ticks);
  }

  void execute_start(td::uint64 ts) {
    // TODO: this is mostly protection for recursive actor calls, which curretly should be almost impossible
    // But too full handle it, one would use one executing_cnt per thread, so only upper level execution is counted
    if (inc(executing_) == 1) {
      store(execute_start_, ts);
      store(execute_messages_, 0);
    }
  }
  void execute_finish(td::uint64 ts) {
    CHECK(executing_ > 0);
    if (dec(executing_) == 0) {
      max_execute_messages_.update(ts, load(execute_messages_));
      max_execute_ticks_.update(ts, elapsed_ticks(ts, load(execute_start_)));

      inc(total_executions_);
      store(execute_start_, 0);
      store(execute_messages_, 0);
    }
  }

  template <class T>
  static td::uint32 get_unique_id() {
    static td::uint32 value = get_next_unique_id();
    return value;
  }

  static td::uint32 get_next_unique_id() {
    static std::atomic<td::uint32> next_id_{};
    return ++next_id_;
  }
  ActorTypeStat to_stat(double inv_ticks_per_second) const {
    auto now = Clocks::rdtsc();
    auto execute_start = ticks_to_seconds(load(execute_start_), inv_ticks_per_second);
    return ActorTypeStat{.created = double(load(total_created_)),
                         .executions = double(load(total_executions_)),
                         .messages = double(load(total_messages_)),
                         .seconds = ticks_to_seconds(load(total_ticks_), inv_ticks_per_second),

                         .alive = load(alive_),
                         .executing = load(executing_),
                         .executing_start = execute_start < 1e-9 ? 1e20 : execute_start,
                         .max_execute_messages = max_execute_messages_.get(now),
                         .max_message_seconds = ticks_to_seconds(max_message_ticks_.get(now), inv_ticks_per_second),
                         .max_execute_seconds = ticks_to_seconds(max_execute_ticks_.get(now), inv_ticks_per_second),
                         .max_delay_seconds = ticks_to_seconds(max_delay_ticks_.get(now), inv_ticks_per_second)};
  }

 private:
  static double ticks_to_seconds(td::uint64 ticks, double inv_tick_per_second) {
    return double(ticks) * inv_tick_per_second;
  }
  template <class T>
  static ActorTypeStat::MaxStatGroup<double> ticks_to_seconds(ActorTypeStat::MaxStatGroup<T> ticks,
                                                              double inv_ticks_per_second) {
    return {.value_forever = ticks_to_seconds(ticks.value_forever, inv_ticks_per_second),
            .value_10s = ticks_to_seconds(ticks.value_10s, inv_ticks_per_second),
            .value_10m = ticks_to_seconds(ticks.value_10m, inv_ticks_per_second)};
  }

  template <class T>
  static T load(const std::atomic<T> &a) {
    return a.load(std::memory_order_relaxed);
  }
  template <class T, class S>
  static void store(std::atomic<T> &a, S value) {
    a.store(value, std::memory_order_relaxed);
  }
  template <class T, class S>
  static T add(std::atomic<T> &a, S value) {
    T new_value = load(a) + value;
    store(a, new_value);
    return new_value;
  }
  template <class T>
  static T inc(std::atomic<T> &a) {
    return add(a, 1);
  }

  template <class T>
  static T dec(std::atomic<T> &a) {
    return add(a, -1);
  }
  // total (increment only statistics)
  std::atomic<td::int64> total_created_{0};
  std::atomic<td::uint64> total_executions_{0};
  std::atomic<td::uint64> total_messages_{0};
  std::atomic<td::uint64> total_ticks_{0};

  // current statistics
  std::atomic<td::int64> alive_{0};
  std::atomic<td::int32> executing_{0};

  // max statistics
  MaxCounterGroup<td::uint32> max_execute_messages_;
  MaxCounterGroup<td::uint64> max_message_ticks_;
  MaxCounterGroup<td::uint64> max_execute_ticks_;
  MaxCounterGroup<td::uint64> max_delay_ticks_;

  // execute state
  std::atomic<td::uint64> execute_start_{0};
  std::atomic<td::uint32> execute_messages_{0};
};

class ActorTypeStatRef {
 public:
  ActorTypeStatImpl *ref_{nullptr};

  void created() {
    if (!ref_) {
      return;
    }
    ref_->created();
  }
  void destroyed() {
    if (!ref_) {
      return;
    }
    ref_->destroyed();
  }
  void pop_from_queue(td::uint64 in_queue_since) {
    if (!ref_) {
      return;
    }
    CHECK(in_queue_since);
    auto ts = td::Clocks::rdtsc();
    if (unlikely(ts < in_queue_since)) {
      return;
    }
    ref_->on_delay(ts, ts - in_queue_since);
  }
  void start_execute() {
    if (!ref_) {
      return;
    }
    ref_->execute_start(td::Clocks::rdtsc());
  }
  void finish_execute() {
    if (!ref_) {
      return;
    }
    ref_->execute_finish(td::Clocks::rdtsc());
  }
  ActorTypeStatImpl::MessageTimer create_message_timer() {
    if (!ref_) {
      return ActorTypeStatImpl::MessageTimer{nullptr, 0};
    }
    return ActorTypeStatImpl::MessageTimer{ref_};
  }
  struct ExecuteTimer {
    ExecuteTimer() = delete;
    ExecuteTimer(const ExecuteTimer &) = delete;
    ExecuteTimer(ExecuteTimer &&) = delete;
    ExecuteTimer &operator=(const ExecuteTimer &) = delete;
    ExecuteTimer &operator=(ExecuteTimer &&) = delete;

    ExecuteTimer(ActorTypeStatRef *stat) : stat(stat) {
      stat->start_execute();
    }
    ActorTypeStatRef *stat{};
    ~ExecuteTimer() {
      stat->finish_execute();
    }
  };
  ExecuteTimer create_execute_timer() {
    return ExecuteTimer(this);
  }
};

struct ActorTypeStats {
  std::map<std::type_index, ActorTypeStat> stats;
  // Aggregate message counts and busy time over every actor type.
  std::array<ActorTypeStat, WORKER_KIND_COUNT> by_worker{};
  ActorTypeStats &operator-=(const ActorTypeStats &other) {
    for (auto &it : other.stats) {
      stats.at(it.first) -= it.second;
    }
    for (size_t i = 0; i < by_worker.size(); i++) {
      by_worker[i] -= other.by_worker[i];
    }
    return *this;
  }
  ActorTypeStats &operator/=(double x) {
    for (auto &it : stats) {
      it.second /= x;
    }
    for (auto &stat : by_worker) {
      stat /= x;
    }
    return *this;
  }
};

class ActorTypeStatTable {
 public:
  ActorTypeStatTable() = default;
  ActorTypeStatTable(const ActorTypeStatTable &) = delete;
  ActorTypeStatTable &operator=(const ActorTypeStatTable &) = delete;

 private:
  friend class ActorTypeStatManager;
  friend class ActorTypeStatRegistry;

  struct Entry {
    std::unique_ptr<ActorTypeStatImpl> stat;
    std::optional<std::type_index> type;
  };

  ActorTypeStatRef get(td::uint32 id, Actor &actor);
  void append_to(ActorTypeStats &result, double inv_ticks_per_second, WorkerKind kind) const;

  mutable std::mutex mutex_;
  std::vector<Entry> by_id_;
};

class ActorTypeStatRegistry {
 public:
  ActorTypeStatTable &thread_table(WorkerKind kind);
  void append_to(ActorTypeStats &result, double inv_ticks_per_second) const;

 private:
  struct ThreadTable {
    explicit ThreadTable(WorkerKind kind) : kind(kind) {
    }

    WorkerKind kind;
    ActorTypeStatTable table;
  };

  mutable std::mutex mutex_;
  std::vector<std::shared_ptr<ThreadTable>> tables_;
};

struct CoroutineResume {};

class ActorTypeStatManager {
 public:
  static ActorTypeStatRef get_actor_type_stat(td::uint32 id, Actor *actor);
  static void append_thread_stats(ActorTypeStats &result, const ActorTypeStatTable *table, const Debug &debug,
                                  WorkerKind kind, double inv_ticks_per_second);
  static ActorTypeStats get_stats(double inv_ticks_per_second);
  static ActorTypeStats get_stats(const SchedulerGroupInfo &group, double inv_ticks_per_second);
  static std::string get_class_name(const char *name);
};

}  // namespace core
}  // namespace actor
}  // namespace td
