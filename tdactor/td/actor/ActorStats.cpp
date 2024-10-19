#include "ActorStats.h"

#include "td/utils/ThreadSafeCounter.h"
namespace td {
namespace actor {
void td::actor::ActorStats::start_up() {
  auto now = td::Time::now();
  for (std::size_t i = 0; i < SIZE; i++) {
    stat_[i] = td::TimedStat<StatStorer<ActorTypeStats>>(DURATIONS[i], now);
    stat_[i].add_event(ActorTypeStats(), now);
  }
  begin_ts_ = td::Timestamp::now();
  begin_ticks_ = Clocks::rdtsc();
  loop();
}
double ActorStats::estimate_inv_ticks_per_second() {
  auto now = td::Timestamp::now();
  auto elapsed_seconds = now.at() - begin_ts_.at();
  auto now_ticks = td::Clocks::rdtsc();
  auto elapsed_ticks = now_ticks - begin_ticks_;
  auto estimated_inv_ticks_per_second =
      elapsed_seconds > 0.1 ? elapsed_seconds / double(elapsed_ticks) : Clocks::inv_ticks_per_second();
  return estimated_inv_ticks_per_second;
}

std::string ActorStats::prepare_stats() {
  auto estimated_inv_ticks_per_second = estimate_inv_ticks_per_second();

  auto current_stats = td::actor::ActorTypeStatManager::get_stats(estimated_inv_ticks_per_second);
  auto now = td::Timestamp::now();
  auto now_ticks = Clocks::rdtsc();

  update(now);

  // Lets look at recent stats first
  auto load_stats = [&](auto &timed_stat) {
    auto res = current_stats;
    auto &since = timed_stat.get_stat(now.at());
    auto duration = since.get_duration(estimated_inv_ticks_per_second);
    if (since.first_) {
      res -= since.first_.value();
    }
    res /= duration;
    return res.stats;
  };
  auto stats_10s = load_stats(stat_[0]);
  auto stats_10m = load_stats(stat_[1]);
  current_stats /= double(now_ticks - begin_ticks_) * estimated_inv_ticks_per_second;
  auto stats_forever = current_stats.stats;

  std::map<std::string, double> current_perf_map;
  std::map<std::string, double> perf_map_10s;
  std::map<std::string, double> perf_map_10m;
  std::map<std::string, double> perf_values;
  td::NamedPerfCounter::get_default().for_each(
      [&](td::Slice name, td::int64 value_int64) { perf_values[name.str()] = double(value_int64); });
  for (auto &value_it : perf_values) {
    const auto &name = value_it.first;
    auto value = value_it.second;

    auto &perf_stat = pef_stats_[name];
    auto load_perf_stats = [&](auto &timed_stat, auto &m) {
      double res = double(value);
      auto &since = timed_stat.get_stat(now.at());
      auto duration = since.get_duration(estimated_inv_ticks_per_second);
      if (since.first_) {
        res -= since.first_.value();
      }
      if (td::ends_with(name, ".duration")) {
        res *= estimated_inv_ticks_per_second;
      }
      // m[name + ".raw"] = res;
      // m[name + ".range"] = duration;
      res /= duration;
      return res;
    };
    perf_map_10s[name] = load_perf_stats(perf_stat.perf_stat_[0], perf_map_10s);
    perf_map_10m[name] = load_perf_stats(perf_stat.perf_stat_[1], perf_map_10m);

    auto current_duration = (double(now_ticks - begin_ticks_) * estimated_inv_ticks_per_second);
    if (td::ends_with(name, ".duration")) {
      value *= estimated_inv_ticks_per_second;
    }
    current_perf_map[name] = double(value) / current_duration;
    // current_perf_map[name + ".raw"] = double(value);
    // current_perf_map[name + ".range"] = double(now_ticks - begin_ticks_) * estimated_inv_ticks_per_second;
  };

  td::StringBuilder sb;
  sb << "================================= PERF COUNTERS ================================\n";
  sb << "ticks_per_second_estimate\t" << 1.0 / estimated_inv_ticks_per_second << "\n";
  for (auto &it : perf_map_10s) {
    const std::string &name = it.first;
    auto dot_at = name.rfind('.');
    CHECK(dot_at != std::string::npos);
    auto base_name = name.substr(0, dot_at);
    auto rest_name = name.substr(dot_at + 1);
    td::Slice new_rest_name = rest_name;
    if (rest_name == "count") {
      new_rest_name = "qps";
    }
    if (rest_name == "duration") {
      new_rest_name = "load";
    }
    auto rewrite_name = PSTRING() << base_name << "." << new_rest_name;
    sb << rewrite_name << "\t" << perf_map_10s[name] << " " << perf_map_10m[name] << " " << current_perf_map[name]
       << "\n";
  }
  sb << "\n";
  sb << "================================= ACTORS STATS =================================\n";
  double max_delay = 0;
  ActorTypeStat sum_stat_forever;
  ActorTypeStat sum_stat_10m;
  ActorTypeStat sum_stat_10s;
  for (auto &it : stats_forever) {
    sum_stat_forever += it.second;
  }
  for (auto &it : stats_10m) {
    sum_stat_10m += it.second;
  }
  for (auto &it : stats_10s) {
    sum_stat_10s += it.second;
  }
  sb << "\n";

  auto do_describe = [&](auto &&sb, const ActorTypeStat &stat_10s, const ActorTypeStat &stat_10m,
                         const ActorTypeStat &stat_forever) {
    sb() << "load_per_second:\t" << stat_10s.seconds << " " << stat_10m.seconds << " " << stat_forever.seconds << "\n";
    sb() << "messages_per_second:\t" << stat_10s.messages << " " << stat_10m.messages << " " << stat_forever.messages
         << "\n";

    sb() << "max_execute_messages:\t" << stat_forever.max_execute_messages.value_10s << " "
         << stat_forever.max_execute_messages.value_10m << " " << stat_forever.max_execute_messages.value_forever
         << "\n";

    sb() << "max_execute_seconds:\t" << stat_forever.max_execute_seconds.value_10s << "s"
         << " " << stat_forever.max_execute_seconds.value_10m << "s"
         << " " << stat_forever.max_execute_seconds.value_forever << "s\n";
    sb() << "max_message_seconds:\t" << stat_forever.max_message_seconds.value_10s << " "
         << stat_forever.max_message_seconds.value_10m << " " << stat_forever.max_message_seconds.value_forever << "\n";
    sb() << "created_per_second:\t" << stat_10s.created << " " << stat_10m.created << " " << stat_forever.created
         << "\n";

    auto executing_for =
        stat_forever.executing_start > 1e15
            ? 0
            : double(td::Clocks::rdtsc()) * estimated_inv_ticks_per_second - stat_forever.executing_start;
    sb() << "max_delay:\t" << stat_forever.max_delay_seconds.value_10s << "s "
         << stat_forever.max_delay_seconds.value_10m << "s " << stat_forever.max_delay_seconds.value_forever << "s\n";
    sb() << ""
         << "alive: " << stat_forever.alive << " executing: " << stat_forever.executing
         << " max_executing_for: " << executing_for << "s\n";
  };

  auto describe = [&](td::StringBuilder &sb, std::type_index actor_type_index) {
    auto stat_10s = stats_10s[actor_type_index];
    auto stat_10m = stats_10m[actor_type_index];
    auto stat_forever = stats_forever[actor_type_index];
    do_describe([&sb]() -> td::StringBuilder & { return sb << "\t\t"; }, stat_10s, stat_10m, stat_forever);
  };

  sb << "Cummulative stats:\n";
  do_describe([&sb]() -> td::StringBuilder & { return sb << "\t"; }, sum_stat_10s, sum_stat_10m, sum_stat_forever);
  sb << "\n";

  auto top_k_by = [&](auto &stats_map, size_t k, std::string description, auto by) {
    auto stats = td::transform(stats_map, [](auto &it) { return std::make_pair(it.first, it.second); });
    k = std::min(k, stats.size());
    std::partial_sort(stats.begin(), stats.begin() + k, stats.end(), [&](auto &a, auto &b) { return by(a) > by(b); });
    bool is_first = true;
    for (size_t i = 0; i < k; i++) {
      auto value = by(stats[i]);
      if (value < 1e-9) {
        break;
      }
      if (is_first) {
        sb << "top actors by " << description << "\n";
        is_first = false;
      }
      sb << "\t#" << i << ": " << ActorTypeStatManager::get_class_name(stats[i].first.name()) << "\t" << value << "\n";
    }
    sb << "\n";
  };
  using Entry = std::pair<std::type_index, td::actor::ActorTypeStat>;
  static auto cutoff = [](auto x, auto min_value) { return x < min_value ? decltype(x){} : x; };
  top_k_by(stats_10s, 10, "load_10s", [](auto &x) { return cutoff(x.second.seconds, 0.005); });

  top_k_by(stats_10m, 10, "load_10m", [](auto &x) { return cutoff(x.second.seconds, 0.005); });
  top_k_by(stats_forever, 10, "max_execute_seconds_10m",
           [](Entry &x) { return cutoff(x.second.max_execute_seconds.value_10m, 0.5); });
  auto rdtsc_seconds = double(now_ticks) * estimated_inv_ticks_per_second;
  top_k_by(stats_forever, 10, "executing_for", [&](Entry &x) {
    if (x.second.executing_start > 1e15) {
      return 0.0;
    }
    return rdtsc_seconds - x.second.executing_start;
  });
  top_k_by(stats_forever, 10, "max_execute_messages_10m",
           [](Entry &x) { return cutoff(x.second.max_execute_messages.value_10m, 10u); });

  auto stats = td::transform(stats_forever, [](auto &it) { return std::make_pair(it.first, it.second); });

  auto main_key = [&](std::type_index actor_type_index) {
    auto stat_10s = stats_10s[actor_type_index];
    auto stat_10m = stats_10m[actor_type_index];
    auto stat_forever = stats_forever[actor_type_index];
    return std::make_tuple(cutoff(std::max(stat_10s.seconds, stat_10m.seconds), 0.1),
                           cutoff(stat_forever.max_execute_seconds.value_10m, 0.5), stat_forever.seconds);
  };
  std::sort(stats.begin(), stats.end(),
            [&](auto &left, auto &right) { return main_key(left.first) > main_key(right.first); });
  auto debug = Debug(SchedulerContext::get()->scheduler_group());
  debug.dump(sb);
  sb << "All actors:\n";
  for (auto &it : stats) {
    sb << "\t" << ActorTypeStatManager::get_class_name(it.first.name()) << "\n";
    auto key = main_key(it.first);
    describe(sb, it.first);
  }
  sb << "\n";
  return sb.as_cslice().str();
}
ActorStats::PefStat::PefStat() {
  for (std::size_t i = 0; i < SIZE; i++) {
    perf_stat_[i] = td::TimedStat<StatStorer<td::int64>>(DURATIONS[i], td::Time::now());
    perf_stat_[i].add_event(0, td::Time::now());
  }
}

void ActorStats::update(td::Timestamp now) {
  auto stat = td::actor::ActorTypeStatManager::get_stats(estimate_inv_ticks_per_second());
  for (auto &timed_stat : stat_) {
    timed_stat.add_event(stat, now.at());
  }
  NamedPerfCounter::get_default().for_each([&](td::Slice name, td::int64 value) {
    auto &stat = pef_stats_[name.str()].perf_stat_;
    for (auto &timed_stat : stat) {
      timed_stat.add_event(value, now.at());
    }
  });
}
constexpr int ActorStats::DURATIONS[SIZE];
constexpr const char *ActorStats::DESCR[SIZE];
}  // namespace actor
}  // namespace td
