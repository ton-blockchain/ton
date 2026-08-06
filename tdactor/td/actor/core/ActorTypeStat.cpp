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

ActorTypeStatRef ActorTypeStatTable::get(td::uint32 id, const std::type_info &type) {
  if (id >= by_id_.size()) {
    std::lock_guard<std::mutex> guard(mutex_);
    by_id_.resize(id + 1);
  }
  auto &entry = by_id_[id];
  if (!entry.type) {
    std::lock_guard<std::mutex> guard(mutex_);
    entry.type = std::type_index(type);
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
  return table->get(id, typeid(*actor));
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

ActorTypeStats ActorTypeStatManager::get_stats(double inv_ticks_per_second) {
  auto *context = SchedulerContext::get_ptr();
  if (!context) {
    return {};
  }
  if (auto *group = context->scheduler_group()) {
    return get_stats(*group, inv_ticks_per_second);
  }

  ActorTypeStats result;
  if (auto *table = context->actor_type_stats()) {
    auto kind = context->has_poll() ? WorkerKind::Io : WorkerKind::Cpu;
    table->append_to(result, inv_ticks_per_second, kind);
  }
  return result;
}

ActorTypeStats ActorTypeStatManager::get_stats(const SchedulerGroupInfo &group, double inv_ticks_per_second) {
  ActorTypeStats result;
  group.actor_type_stats.append_to(result, inv_ticks_per_second);
  auto append = [&](const WorkerInfo &worker) {
    auto kind = worker.type == WorkerInfo::Type::Io ? WorkerKind::Io : WorkerKind::Cpu;
    auto &worker_stat = result.by_worker[static_cast<size_t>(kind)];
    worker_stat.seconds += double(worker.debug.busy_ticks()) * inv_ticks_per_second;
  };
  for (const auto &scheduler : group.schedulers) {
    if (scheduler.io_worker) {
      append(*scheduler.io_worker);
    }
    for (const auto &worker : scheduler.cpu_workers) {
      append(*worker);
    }
  }
  return result;
}
}  // namespace core
}  // namespace actor
}  // namespace td
