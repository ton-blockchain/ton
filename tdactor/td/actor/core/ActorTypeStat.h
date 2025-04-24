#pragma once
#include "td/utils/int_types.h"
#include "td/utils/port/Clocks.h"
#include <algorithm>
#include <typeindex>
#include <map>

namespace td {
namespace actor {
namespace core {
class Actor;

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

  // max statistics (TODO: recent_max)
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
        stat_->message_finish(ts, ts - started_at_);
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
      max_execute_ticks_.update(ts, ts - load(execute_start_));

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
    auto execute_start_copy = load(execute_start_);
    auto actual_total_ticks = load(total_ticks_);
    auto ts = Clocks::rdtsc();
    if (execute_start_copy != 0) {
      actual_total_ticks += ts - execute_start_copy;
    }
    auto execute_start = ticks_to_seconds(load(execute_start_), inv_ticks_per_second);
    return ActorTypeStat{.created = double(load(total_created_)),
                         .executions = double(load(total_executions_)),
                         .messages = double(load(total_messages_)),
                         .seconds = ticks_to_seconds(actual_total_ticks, inv_ticks_per_second),

                         .alive = load(alive_),
                         .executing = load(executing_),
                         .executing_start = execute_start < 1e-9 ? 1e20 : execute_start,
                         .max_execute_messages = load(max_execute_messages_),
                         .max_message_seconds = load_seconds(max_message_ticks_, inv_ticks_per_second),
                         .max_execute_seconds = load_seconds(max_execute_ticks_, inv_ticks_per_second),
                         .max_delay_seconds = load_seconds(max_delay_ticks_, inv_ticks_per_second)};
  }

 private:
  static double ticks_to_seconds(td::uint64 ticks, double inv_tick_per_second) {
    return double(ticks) * inv_tick_per_second;
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
  template <class T>
  static void relax_max(std::atomic<T> &a, T value) {
    auto old_value = load(a);
    if (value > old_value) {
      store(a, value);
    }
  }

  template <class ValueT, int Interval>
  class MaxCounter {
    alignas(64) std::atomic<ValueT> max_values[2] = {0};
    std::atomic<td::uint64> last_update_segment_time = 0;

    void update_current_segment(uint64 current_segment_time, uint64 segment_difference) {
      if (segment_difference >= 2) {
        store(max_values[0], 0);
        store(max_values[1], 0);
      } else if (segment_difference == 1) {
        store(max_values[1 - (current_segment_time & 1)], 0);
      }
      store(last_update_segment_time, current_segment_time);
    }

   public:
    inline void update(td::uint64 rdtsc, ValueT value) {
      auto current_segment_time = rdtsc / (Clocks::rdtsc_frequency() * Interval);

      auto segment_difference = current_segment_time - last_update_segment_time;

      if (unlikely(segment_difference != 0)) {
        update_current_segment(current_segment_time, segment_difference);
      }

      relax_max(max_values[current_segment_time & 1], value);
    }

    inline ValueT get_max(uint64_t rdtsc) const {
      uint64_t current_segment_time = rdtsc / (Clocks::rdtsc_frequency() * Interval);
      uint64_t segment_difference = current_segment_time - load(last_update_segment_time);

      if (segment_difference >= 2) {
        return 0;
      } else if (segment_difference == 1) {
        return load(max_values[current_segment_time & 1]);
      } else {
        return std::max(load(max_values[0]), load(max_values[1]));
      }
    }
  };

  template <class T>
  struct MaxCounterGroup {
    std::atomic<T> max_forever{};
    MaxCounter<T, 60 * 10> max_10m;
    MaxCounter<T, 10> max_10s;

    inline void update(td::uint64 rdtsc, T value) {
      relax_max(max_forever, value);
      max_10m.update(rdtsc, value);
      max_10s.update(rdtsc, value);
    }
  };
  template <class T>
  static ActorTypeStat::MaxStatGroup<T> load(const MaxCounterGroup<T> &src) {
    auto ts = Clocks::rdtsc();
    return {.value_forever = load(src.max_forever),
            .value_10s = src.max_10s.get_max(ts),
            .value_10m = src.max_10m.get_max(ts)};
  }
  template <class T>
  static ActorTypeStat::MaxStatGroup<double> load_seconds(const MaxCounterGroup<T> &src, double inv_ticks_per_second) {
    auto ts = Clocks::rdtsc();
    return {.value_forever = ticks_to_seconds(load(src.max_forever), inv_ticks_per_second),
            .value_10s = ticks_to_seconds(src.max_10s.get_max(ts), inv_ticks_per_second),
            .value_10m = ticks_to_seconds(src.max_10m.get_max(ts), inv_ticks_per_second)};
  }

  // total (increment only statistics)
  std::atomic<td::int64> total_created_{0};
  std::atomic<td::uint64> total_executions_{0};
  std::atomic<td::uint64> total_messages_{0};
  std::atomic<td::uint64> total_ticks_{0};

  // current statistics
  std::atomic<td::int64> alive_{0};
  std::atomic<td::int32> executing_{0};

  // max statistics (TODO: recent_max)
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

// TODO: currently it is implemented via TD_THREAD_LOCAL, so the statistics is global across different schedulers
struct ActorTypeStats {
  std::map<std::type_index, ActorTypeStat> stats;
  ActorTypeStats &operator-=(const ActorTypeStats &other) {
    for (auto &it : other.stats) {
      stats.at(it.first) -= it.second;
    }
    return *this;
  }
  ActorTypeStats &operator/=(double x) {
    for (auto &it : stats) {
      it.second /= x;
    }
    return *this;
  }
};
class ActorTypeStatManager {
 public:
  static ActorTypeStatRef get_actor_type_stat(td::uint32 id, Actor *actor);
  static ActorTypeStats get_stats(double inv_ticks_per_second);
  static std::string get_class_name(const char *name);
};

}  // namespace core
}  // namespace actor
}  // namespace td