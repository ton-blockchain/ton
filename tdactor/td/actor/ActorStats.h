#pragma once
#include "td/actor/actor.h"
#include "td/utils/TimedStat.h"
namespace td {
namespace actor {

class ActorStats : public td::actor::Actor {
 public:
  ActorStats() {
  }
  void start_up() override;
  double estimate_inv_ticks_per_second();
  std::string prepare_stats();

 private:
  template <class T>
  struct StatStorer {
    void on_event(const T &event) {
      if (!first_) {
        first_ = event;
        first_ts_ = Clocks::rdtsc();
      }
    }
    double get_duration(double inv_ticks_per_second) const {
      if (first_) {
        return std::max(1.0, (Clocks::rdtsc() - first_ts_) * inv_ticks_per_second);
      }
      return 1.0;
    }
    td::optional<T> first_;
    td::uint64 first_ts_;
  };
  static constexpr std::size_t SIZE = 2;
  static constexpr const char *DESCR[SIZE] = {"10sec", "10min"};
  static constexpr int DURATIONS[SIZE] = {10, 10 * 60};
  td::TimedStat<StatStorer<td::actor::ActorTypeStats>> stat_[SIZE];
  struct PefStat {
    PefStat();
    td::TimedStat<StatStorer<td::int64>> perf_stat_[SIZE];
  };
  std::map<std::string, PefStat> pef_stats_;
  td::Timestamp begin_ts_;
  td::uint64 begin_ticks_{};
  void loop() override {
    alarm_timestamp() = td::Timestamp::in(5.0);
    update(td::Timestamp::now());
  }
  void update(td::Timestamp now);
};

}  // namespace actor
}  // namespace td
