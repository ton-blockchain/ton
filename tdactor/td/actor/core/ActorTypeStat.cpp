#include "td/actor/core/Actor.h"
#include "td/actor/core/ActorTypeStat.h"
#include "td/actor/core/Scheduler.h"
#include "td/utils/port/thread_local.h"
#include <set>
#include <map>
#include <mutex>
#include <typeindex>
#include <typeinfo>
#include <optional>

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

class ActorTypeStatRef;
struct ActorTypeStatsTlsEntry {
  struct Entry {
    std::unique_ptr<ActorTypeStatImpl> stat;
    std::optional<std::type_index> o_type_index;
  };
  std::vector<Entry> by_id;
  std::mutex mutex;

  template <class F>
  void foreach_entry(F &&f) {
    std::lock_guard<std::mutex> guard(mutex);
    for (auto &entry : by_id) {
      f(entry);
    }
  }
  ActorTypeStatRef get_actor_type_stat(td::uint32 id, Actor &actor) {
    if (id >= by_id.size()) {
      std::lock_guard<std::mutex> guard(mutex);
      by_id.resize(id + 1);
    }
    auto &entry = by_id.at(id);
    if (!entry.o_type_index) {
      std::lock_guard<std::mutex> guard(mutex);
      entry.o_type_index = std::type_index(typeid(actor));
      entry.stat = std::make_unique<ActorTypeStatImpl>();
    }
    return ActorTypeStatRef{entry.stat.get()};
  }
};

struct ActorTypeStatsRegistry {
  std::mutex mutex;
  std::vector<std::shared_ptr<ActorTypeStatsTlsEntry>> entries;
  void registry_entry(std::shared_ptr<ActorTypeStatsTlsEntry> entry) {
    std::lock_guard<std::mutex> guard(mutex);
    entries.push_back(std::move(entry));
  }
  template <class F>
  void foreach_entry(F &&f) {
    std::lock_guard<std::mutex> guard(mutex);
    for (auto &entry : entries) {
      f(*entry);
    }
  }
};

ActorTypeStatsRegistry registry;

struct ActorTypeStatsTlsEntryRef {
  ActorTypeStatsTlsEntryRef() {
    entry_ = std::make_shared<ActorTypeStatsTlsEntry>();
    registry.registry_entry(entry_);
  }
  std::shared_ptr<ActorTypeStatsTlsEntry> entry_;
};

static TD_THREAD_LOCAL ActorTypeStatsTlsEntryRef *actor_type_stats_tls_entry = nullptr;

ActorTypeStatRef ActorTypeStatManager::get_actor_type_stat(td::uint32 id, Actor *actor) {
  if (!actor || !need_debug()) {
    return ActorTypeStatRef{nullptr};
  }
  td::init_thread_local<ActorTypeStatsTlsEntryRef>(actor_type_stats_tls_entry);
  ActorTypeStatsTlsEntry &tls_entry = *actor_type_stats_tls_entry->entry_;
  return tls_entry.get_actor_type_stat(id, *actor);
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
  std::map<std::type_index, ActorTypeStat> stats;
  registry.foreach_entry([&](ActorTypeStatsTlsEntry &tls_entry) {
    tls_entry.foreach_entry([&](ActorTypeStatsTlsEntry::Entry &entry) {
      if (entry.o_type_index) {
        stats[entry.o_type_index.value()] += entry.stat->to_stat(inv_ticks_per_second);
      }
    });
  });
  return ActorTypeStats{.stats = std::move(stats)};
}
}  // namespace core
}  // namespace actor
}  // namespace td
