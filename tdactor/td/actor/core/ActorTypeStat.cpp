#include <cstdlib>
#include <unordered_map>

#include "td/actor/core/Actor.h"
#include "td/actor/core/ActorTypeStat.h"
#include "td/actor/core/Scheduler.h"

#ifdef __has_include
#if __has_include(<cxxabi.h>)
#include <cxxabi.h>
#define CXXABI_AVAILABLE 1
#else
#define CXXABI_AVAILABLE 0
#endif
#else
#define CXXABI_AVAILABLE 0
#endif

namespace td {
namespace actor {
namespace core {

void CoroutineStat::add(td::uint64 finished_at, td::uint64 elapsed_ticks) {
  count_++;
  total_ticks_ += elapsed_ticks;
  last_finished_at_ = finished_at;
  max_ticks_.update(finished_at, elapsed_ticks);
}

ActorTypeStat CoroutineStat::to_stat(td::uint64 now, double inv_ticks_per_second, td::uint64 active_since) const {
  auto max_ticks = max_ticks_.get(now);
  auto seconds = [inv_ticks_per_second](td::uint64 ticks) { return double(ticks) * inv_ticks_per_second; };
  ActorTypeStat::MaxStatGroup<double> max_seconds{.value_forever = seconds(max_ticks.value_forever),
                                                  .value_10s = seconds(max_ticks.value_10s),
                                                  .value_10m = seconds(max_ticks.value_10m)};
  ActorTypeStat::MaxStatGroup<td::uint32> max_messages{.value_forever = count_ == 0 ? 0u : 1u,
                                                       .value_10s = has_recent_completion<10>(now) ? 1u : 0u,
                                                       .value_10m = has_recent_completion<10 * 60>(now) ? 1u : 0u};
  // Completed resumes only, matching ActorTypeStatImpl: a live resume is visible through
  // executing/executing_start, never as growing seconds.
  auto is_active = active_since != 0;
  return ActorTypeStat{.executions = double(count_),
                       .messages = double(count_),
                       .seconds = seconds(total_ticks_),
                       .executing = is_active ? 1 : 0,
                       .executing_start = is_active ? seconds(std::min(now, active_since)) : 1e20,
                       .max_execute_messages = max_messages,
                       .max_message_seconds = max_seconds,
                       .max_execute_seconds = max_seconds,
                       .max_delay_seconds = {}};
}

ActorTypeStatRef ActorTypeStatTable::get(td::uint32 id, Actor &actor) {
  if (id >= by_id_.size()) {
    std::lock_guard<std::mutex> guard(mutex_);
    by_id_.resize(id + 1);
  }
  auto &entry = by_id_[id];
  if (!entry.type) {
    std::lock_guard<std::mutex> guard(mutex_);
    entry.type = std::type_index(typeid(actor));
    entry.stat = std::make_unique<ActorTypeStatImpl>();
  }
  return ActorTypeStatRef{entry.stat.get()};
}

void ActorTypeStatTable::append_to(ActorTypeStats &result, double inv_ticks_per_second, WorkerKind kind) const {
  auto &worker = result.by_worker[static_cast<size_t>(kind)];
  std::lock_guard<std::mutex> guard(mutex_);
  for (const auto &entry : by_id_) {
    if (entry.type) {
      auto stat = entry.stat->to_stat(inv_ticks_per_second);
      worker.messages += stat.messages;
      result.stats[*entry.type] += stat;
    }
  }
}

ActorTypeStatTable &ActorTypeStatRegistry::thread_table(WorkerKind kind) {
  using Tables = std::array<std::weak_ptr<ThreadTable>, WORKER_KIND_COUNT>;
  static thread_local std::unordered_map<const ActorTypeStatRegistry *, Tables> cache;
  auto it = cache.find(this);
  if (it == cache.end()) {
    for (auto cached = cache.begin(); cached != cache.end();) {
      if (std::all_of(cached->second.begin(), cached->second.end(),
                      [](const auto &table) { return table.expired(); })) {
        cached = cache.erase(cached);
      } else {
        ++cached;
      }
    }
    it = cache.emplace(this, Tables{}).first;
  }

  auto &cached = it->second[static_cast<size_t>(kind)];
  auto table = cached.lock();
  if (!table) {
    table = std::make_shared<ThreadTable>(kind);
    {
      std::lock_guard<std::mutex> guard(mutex_);
      tables_.push_back(table);
    }
    cached = table;
  }
  return table->table;
}

void ActorTypeStatRegistry::append_to(ActorTypeStats &result, double inv_ticks_per_second) const {
  std::vector<std::shared_ptr<ThreadTable>> tables;
  {
    std::lock_guard<std::mutex> guard(mutex_);
    tables = tables_;
  }
  for (const auto &table : tables) {
    table->table.append_to(result, inv_ticks_per_second, table->kind);
  }
}

ActorTypeStatRef ActorTypeStatManager::get_actor_type_stat(td::uint32 id, Actor *actor) {
  if (!actor || !need_debug()) {
    return ActorTypeStatRef{nullptr};
  }
  auto *context = SchedulerContext::get_ptr();
  if (!context) {
    return ActorTypeStatRef{nullptr};
  }
  auto *table = context->actor_type_stats();
  if (!table) {
    return ActorTypeStatRef{nullptr};
  }
  return table->get(id, *actor);
}

std::string ActorTypeStatManager::get_class_name(const char *name) {
#if CXXABI_AVAILABLE
  int status;
  char *real_name = abi::__cxa_demangle(name, nullptr, nullptr, &status);
  if (status < 0) {
    return name;
  }

  std::string result = real_name;
  std::free(real_name);
  return result;
#else
  return name;
#endif
}

namespace {
void append_worker_stats(ActorTypeStats &result, const Debug &debug, WorkerKind kind, double inv_ticks_per_second) {
  auto stats = debug.stats(inv_ticks_per_second);
  auto &worker = result.by_worker[static_cast<size_t>(kind)];
  worker.seconds += double(stats.busy_ticks) * inv_ticks_per_second;
  if (stats.coroutine.messages != 0 || stats.coroutine.executing != 0) {
    worker.messages += stats.coroutine.messages;
    result.stats[typeid(CoroutineResume)] += stats.coroutine;
  }
}
}  // namespace

void ActorTypeStatManager::append_thread_stats(ActorTypeStats &result, const ActorTypeStatTable *table,
                                               const Debug &debug, WorkerKind kind, double inv_ticks_per_second) {
  if (table) {
    table->append_to(result, inv_ticks_per_second, kind);
  }
  append_worker_stats(result, debug, kind, inv_ticks_per_second);
}

void SchedulerContext::append_actor_type_stats(ActorTypeStats &result, double inv_ticks_per_second) {
  auto kind = has_poll() ? WorkerKind::Io : WorkerKind::Cpu;
  ActorTypeStatManager::append_thread_stats(result, actor_type_stats(), get_debug(), kind, inv_ticks_per_second);
}

ActorTypeStats ActorTypeStatManager::get_stats(double inv_ticks_per_second) {
  auto *context = SchedulerContext::get_ptr();
  if (!context) {
    return {};
  }
  if (auto *group = context->scheduler_group()) {
    return get_stats(*group, inv_ticks_per_second);
  }

  ActorTypeStats result;
  context->append_actor_type_stats(result, inv_ticks_per_second);
  return result;
}

ActorTypeStats ActorTypeStatManager::get_stats(const SchedulerGroupInfo &group, double inv_ticks_per_second) {
  ActorTypeStats result;
  group.actor_type_stats.append_to(result, inv_ticks_per_second);
  for (const auto &scheduler : group.schedulers) {
    if (scheduler.io_worker) {
      append_worker_stats(result, scheduler.io_worker->debug, scheduler.io_worker->kind, inv_ticks_per_second);
    }
    for (const auto &worker : scheduler.cpu_workers) {
      append_worker_stats(result, worker->debug, worker->kind, inv_ticks_per_second);
    }
  }
  return result;
}
}  // namespace core
}  // namespace actor
}  // namespace td
