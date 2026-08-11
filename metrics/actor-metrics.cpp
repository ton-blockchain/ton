/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include <algorithm>
#include <array>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "td/actor/ActorStats.h"
#include "td/actor/common.h"
#include "td/utils/port/Clocks.h"

#include "actor-metrics.h"

namespace ton::metrics {

namespace core = td::actor::core;
using Stat = td::actor::ActorTypeStat;

namespace {

constexpr std::array worker_kinds{
    std::pair{core::WorkerKind::Io, std::string_view{"io"}},
    std::pair{core::WorkerKind::Cpu, std::string_view{"cpu"}},
};
static_assert(worker_kinds.size() == core::WORKER_KIND_COUNT);

void collect_actor_types(Context ctx, const td::actor::ActorTypeStats &raw) {
  // Different anonymous-namespace types can demangle to the same label. Merge them so a family
  // never contains duplicate label sets.
  std::map<std::string, Stat> stats;
  for (const auto &[index, stat] : raw.stats) {
    stats[td::actor::ActorTypeStatManager::get_class_name(index.name())] += stat;
  }

  auto by_type = [&](Context family, std::string_view type, std::string_view suffix, auto value) {
    family.open_family(type, suffix);
    for (const auto &[name, stat] : stats) {
      family.with_label("type", name).push(value(stat));
    }
  };
  auto max_by_type = [&](Context family, auto value) {
    family.open_family("gauge");
    for (const auto &[name, stat] : stats) {
      family.with_label("type", name).with_label("window", "recent").push(value(stat));
    }
  };

  by_type(ctx.with_name("busy_ticks"), "counter", "total", [](const Stat &s) { return s.seconds; });
  by_type(ctx.with_name("messages"), "counter", "total", [](const Stat &s) { return s.messages; });
  by_type(ctx.with_name("executions"), "counter", "total", [](const Stat &s) { return s.executions; });
  by_type(ctx.with_name("created"), "counter", "total", [](const Stat &s) { return s.created; });
  by_type(ctx.with_name("alive"), "gauge", "", [](const Stat &s) { return double(s.alive); });
  max_by_type(ctx.with_name("max_message_ticks"), [](const Stat &s) { return s.max_message_seconds.value_10m; });
  max_by_type(ctx.with_name("max_execute_ticks"), [](const Stat &s) { return s.max_execute_seconds.value_10m; });
  max_by_type(ctx.with_name("max_batch_messages"),
              [](const Stat &s) { return double(s.max_execute_messages.value_10m); });
  max_by_type(ctx.with_name("max_queue_ticks"), [](const Stat &s) { return s.max_delay_seconds.value_10m; });
}

void collect_workers(Context ctx, const td::actor::ActorTypeStats &raw, core::SchedulerGroupInfo *group,
                     core::Debug *collector_debug) {
  auto worker = ctx.with_name("worker");
  auto by_worker = [&](Context family, auto value) {
    family.open_family("counter", "total");
    for (auto [kind, name] : worker_kinds) {
      family.with_label("worker", name).push(value(raw.by_worker[static_cast<size_t>(kind)]));
    }
  };
  by_worker(worker.with_name("busy_ticks"), [](const Stat &s) { return s.seconds; });
  by_worker(worker.with_name("messages"), [](const Stat &s) { return s.messages; });

  struct SchedulerSample {
    std::string id;
    size_t threads{0};
    size_t local_queue_length{0};
    size_t active{0};
    double longest{0};
  };
  std::vector<SchedulerSample> schedulers;
  size_t io_threads = 0;
  size_t cpu_threads = 0;
  if (group != nullptr) {
    schedulers.reserve(group->schedulers.size());
    auto now = td::Time::now();
    for (auto &info : group->schedulers) {
      if (!info.io_worker) {
        continue;
      }
      SchedulerSample sample{.id = PSTRING() << info.id.value(), .threads = info.cpu_workers.size() + 1};
      ++io_threads;
      cpu_threads += info.cpu_workers.size();
      for (const auto &queue : info.cpu_local_queue) {
        sample.local_queue_length += queue.size();
      }
      auto read_worker = [&](core::WorkerInfo &worker_info) {
        if (&worker_info.debug == collector_debug) {
          return;
        }
        core::DebugInfo debug;
        worker_info.debug.read(debug);
        sample.active += debug.is_active;
        if (debug.is_active) {
          sample.longest = std::max(sample.longest, now - debug.start_at);
        }
      };
      read_worker(*info.io_worker);
      for (auto &cpu_worker : info.cpu_workers) {
        read_worker(*cpu_worker);
      }
      schedulers.push_back(std::move(sample));
    }
  }

  auto worker_threads = worker.with_name("threads");
  worker_threads.open_family("gauge");
  if (group != nullptr) {
    worker_threads.with_label("worker", "io").push(double(io_threads));
    worker_threads.with_label("worker", "cpu").push(double(cpu_threads));
  }
  auto by_scheduler = [&](Context family, auto value) {
    family.open_family("gauge");
    for (const auto &sample : schedulers) {
      family.with_label("scheduler", sample.id).push(value(sample));
    }
  };

  auto scheduler = ctx.with_name("scheduler");
  by_scheduler(scheduler.with_name("threads"), [](const SchedulerSample &s) { return double(s.threads); });
  by_scheduler(scheduler.with_name("local_queue_length"),
               [](const SchedulerSample &s) { return double(s.local_queue_length); });
  by_scheduler(scheduler.with_name("workers_active"), [](const SchedulerSample &s) { return double(s.active); });
  by_scheduler(scheduler.with_name("current_execute_seconds"), [](const SchedulerSample &s) { return s.longest; });
}

}  // namespace

double detail::estimate_ticks_per_second(double elapsed_seconds, td::uint64 begin_ticks, td::uint64 current_ticks) {
  auto elapsed_ticks = td::actor::detail::calibration_elapsed_ticks(elapsed_seconds, begin_ticks, current_ticks);
  if (elapsed_ticks == 0) {
    return td::Clocks::ticks_per_second();
  }
  return double(elapsed_ticks) / elapsed_seconds;
}

ActorMetrics::ActorMetrics() : begin_ts_(td::Timestamp::now()), begin_ticks_(td::Clocks::rdtsc()) {
}

double ActorMetrics::estimate_ticks_per_second() const {
  auto elapsed_seconds = td::Timestamp::now().at() - begin_ts_.at();
  return detail::estimate_ticks_per_second(elapsed_seconds, begin_ticks_, td::Clocks::rdtsc());
}

void ActorMetrics::collect(Context ctx) const {
  auto ticks_per_second = estimate_ticks_per_second();
  auto *scheduler_context = td::actor::SchedulerContext::get_ptr();
  auto *group = scheduler_context == nullptr ? nullptr : scheduler_context->scheduler_group();
  auto *collector_debug = scheduler_context == nullptr ? nullptr : &scheduler_context->get_debug();

  // Keep cumulative values in raw ticks. Rescaling them with a changing estimate could make a
  // Prometheus counter decrease; dashboards apply the per-target frequency instead.
  td::actor::ActorTypeStats raw;
  if (group != nullptr) {
    raw = td::actor::ActorTypeStatManager::get_stats(*group, 1);
  }
  collect_actor_types(ctx, raw);
  collect_workers(ctx, raw, group, collector_debug);

  auto enabled = ctx.with_name("stats_enabled");
  enabled.open_family("gauge");
  enabled.push(core::need_debug() ? 1 : 0);

  auto ticks = ctx.with_name("ticks_per_second");
  ticks.open_family("gauge");
  ticks.push(ticks_per_second);
}

}  // namespace ton::metrics
