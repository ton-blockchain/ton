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

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <utility>

#include "td/actor/core/Actor.h"
#include "td/actor/core/ActorExecuteContext.h"
#include "td/actor/core/ActorExecutor.h"
#include "td/actor/core/ActorInfo.h"
#include "td/actor/core/ActorInfoCreator.h"
#include "td/actor/core/ActorLocker.h"
#include "td/actor/core/ActorMailbox.h"
#include "td/actor/core/ActorMessage.h"
#include "td/actor/core/Context.h"
#include "td/actor/core/SchedulerContext.h"
#include "td/actor/core/SchedulerId.h"
#include "td/actor/core/SchedulerMessage.h"
#include "td/utils/Closure.h"
#include "td/utils/Heap.h"
#include "td/utils/List.h"
#include "td/utils/MpmcQueue.h"
#include "td/utils/MpmcWaiter.h"
#include "td/utils/MpscLinkQueue.h"
#include "td/utils/MpscPollableQueue.h"
#include "td/utils/ScopeGuard.h"
#include "td/utils/Slice.h"
#include "td/utils/StealingQueue.h"
#include "td/utils/Time.h"
#include "td/utils/common.h"
#include "td/utils/format.h"
#include "td/utils/logging.h"
#include "td/utils/optional.h"
#include "td/utils/port/Clocks.h"
#include "td/utils/port/Poll.h"
#include "td/utils/port/detail/Iocp.h"
#include "td/utils/port/thread.h"
#include "td/utils/port/thread_local.h"
#include "td/utils/type_traits.h"

namespace td {
namespace actor {
namespace core {
class IoWorker;

struct DebugInfo {
  bool is_active{false};
  double start_at{0};
  static constexpr size_t name_size{32};
  char name[name_size] = {};
  void set_name(td::Slice from) {
    from.truncate(name_size - 1);
    std::memcpy(name, from.data(), from.size());
    name[from.size()] = 0;
  }
};

void set_debug(bool flag);
bool need_debug();

struct Debug {
 public:
  enum class Work { Actor, Coroutine };
  struct Stats {
    td::uint64 busy_ticks;
    ActorTypeStat coroutine;
  };

  bool is_on() const {
    return need_debug();
  }
  struct Destructor {
    void operator()(Debug *debug) {
      std::lock_guard<std::mutex> lock(debug->info_mutex_);
      if (!debug->info_.is_active) {
        return;
      }
      auto finished_at = Clocks::rdtsc();
      auto duration = elapsed_ticks(finished_at, debug->started_at_);
      debug->busy_ticks_ += duration;
      if (debug->work_ == Work::Coroutine) {
        debug->coroutine_.add(finished_at, duration);
      }
      debug->started_at_ = 0;
      debug->info_.is_active = false;
    }
  };

  void read(DebugInfo &info) {
    std::lock_guard<std::mutex> lock(info_mutex_);
    info = info_;
  }

  std::unique_ptr<Debug, Destructor> start(td::Slice name, Work work = Work::Actor) {
    if (!is_on()) {
      return {};
    }
    {
      std::lock_guard<std::mutex> lock(info_mutex_);
      if (info_.is_active) {
        return {};
      }
      started_at_ = Clocks::rdtsc();
      work_ = work;
      info_.is_active = true;
      info_.start_at = Time::now();
      info_.set_name(name);
    }
    return std::unique_ptr<Debug, Destructor>(this);
  }

  Stats stats(double inv_ticks_per_second) const {
    std::lock_guard<std::mutex> lock(info_mutex_);
    auto now = Clocks::rdtsc();
    auto active_since = info_.is_active && work_ == Work::Coroutine ? started_at_ : 0;
    auto busy_ticks = busy_ticks_ + (info_.is_active ? elapsed_ticks(now, started_at_) : 0);
    return {.busy_ticks = busy_ticks, .coroutine = coroutine_.to_stat(now, inv_ticks_per_second, active_since)};
  }

 private:
  mutable std::mutex info_mutex_;
  DebugInfo info_;
  td::uint64 started_at_{0};
  td::uint64 busy_ticks_{0};
  Work work_{Work::Actor};
  CoroutineStat coroutine_;
};

struct WorkerInfo {
  WorkerKind kind{WorkerKind::Io};
  WorkerInfo() = default;
  explicit WorkerInfo(WorkerKind kind, bool allow_shared, CpuWorkerId cpu_worker_id)
      : kind(kind), actor_info_creator(allow_shared), cpu_worker_id(cpu_worker_id) {
  }
  ActorInfoCreator actor_info_creator;
  CpuWorkerId cpu_worker_id;
  Debug debug;
};

template <class T>
struct LocalQueue {
 public:
  template <class F>
  bool push(T value, F &&overflow_f) {
    queue_.local_push(std::move(value), overflow_f);
    return true;
  }
  bool try_pop(T &message) {
    return queue_.local_pop(message);
  }
  bool steal(T &message, LocalQueue &other) {
    return queue_.steal(message, other.queue_);
  }
  size_t size() const {
    return queue_.size();
  }

 private:
  StealingQueue<T> queue_;
  char pad[TD_CONCURRENCY_PAD - sizeof(optional<T>)];
};

struct SchedulerInfo {
  SchedulerId id;
  // will be read by all workers is any thread
  std::unique_ptr<MpmcQueue<SchedulerToken>> cpu_queue;
  std::unique_ptr<MpmcWaiter> cpu_queue_waiter;

  std::vector<LocalQueue<SchedulerToken>> cpu_local_queue;
  //std::vector<td::StealingQueue<SchedulerMessage>> cpu_stealing_queue;

  // only scheduler itself may read from io_queue_
  std::unique_ptr<MpscPollableQueue<SchedulerMessage>> io_queue;
  size_t cpu_threads_count{0};

  std::unique_ptr<WorkerInfo> io_worker;
  std::vector<std::unique_ptr<WorkerInfo>> cpu_workers;
};

struct SchedulerGroupInfo {
  explicit SchedulerGroupInfo(size_t n) : schedulers(n) {
  }
  std::atomic<bool> is_stop_requested{false};

  int active_scheduler_count{0};
  std::mutex active_scheduler_count_mutex;
  std::condition_variable active_scheduler_count_condition_variable;
  ActorTypeStatRegistry actor_type_stats;

#if TD_PORT_WINDOWS
  td::detail::Iocp iocp;
  td::thread iocp_thread;
#endif
  std::vector<SchedulerInfo> schedulers;
};

class Scheduler {
 public:
  static constexpr int32 max_thread_count() {
    return MAX_THREADS;
  }

  static int32 get_thread_id() {
    auto thread_id = ::td::get_thread_id();
    CHECK(thread_id < max_thread_count());
    return thread_id;
  }

  Scheduler(std::shared_ptr<SchedulerGroupInfo> scheduler_group_info, SchedulerId id, size_t cpu_threads_count,
            bool skip_timeouts = false);

  Scheduler(const Scheduler &) = delete;
  Scheduler &operator=(const Scheduler &) = delete;
  Scheduler(Scheduler &&other) = delete;
  Scheduler &operator=(Scheduler &&other) = delete;
  ~Scheduler();

  void start();

  bool run(double timeout);

  SchedulerId get_scheduler_id() const {
    return info_->id;
  }

 private:
  std::shared_ptr<SchedulerGroupInfo> scheduler_group_info_;
  SchedulerInfo *info_;
  std::vector<td::thread> cpu_threads_;
  bool is_stopped_{false};
  Poll poll_;
  KHeap<Timestamp> heap_;
  std::unique_ptr<IoWorker> io_worker_;
  bool skip_timeouts_{false};

  class ContextImpl : public SchedulerContext {
   public:
    ContextImpl(ActorInfoCreator *creator, SchedulerId scheduler_id, CpuWorkerId cpu_worker_id,
                SchedulerGroupInfo *scheduler_group, Poll *poll, KHeap<Timestamp> *heap, Debug *debug);

    SchedulerId get_scheduler_id() const override;
    void add_to_queue(ActorInfoPtr actor_info_ptr, SchedulerId scheduler_id, bool need_poll) override;
    void add_token_to_cpu_queue(SchedulerToken token, SchedulerId scheduler_id) override;

    ActorInfoCreator &get_actor_info_creator() override;

    bool has_poll() override;
    Poll &get_poll() override;

    bool has_heap() override;
    KHeap<Timestamp> &get_heap() override;

    Debug &get_debug() override;
    ActorTypeStatTable *actor_type_stats() override;

    void set_alarm_timestamp(const ActorInfoPtr &actor_info_ptr) override;

    bool is_stop_requested() override;
    void stop() override;

    SchedulerGroupInfo *scheduler_group() const override {
      return scheduler_group_;
    }

   private:
    ActorInfoCreator *creator_;
    SchedulerId scheduler_id_;
    CpuWorkerId cpu_worker_id_;
    SchedulerGroupInfo *scheduler_group_;
    Poll *poll_;

    KHeap<Timestamp> *heap_;

    Debug *debug_;
    ActorTypeStatTable *actor_type_stats_{nullptr};
  };

  template <class F>
  auto run_in_context_impl(WorkerInfo &worker_info, F &&f) {
#if TD_PORT_WINDOWS
    td::detail::Iocp::Guard iocp_guard(&scheduler_group_info_->iocp);
#endif
    bool is_io_worker = worker_info.kind == WorkerKind::Io;
    ContextImpl context(&worker_info.actor_info_creator, info_->id, worker_info.cpu_worker_id,
                        scheduler_group_info_.get(), is_io_worker ? &poll_ : nullptr, is_io_worker ? &heap_ : nullptr,
                        &worker_info.debug);
    SchedulerContext::Guard guard(&context);
    return f();
  }

  void do_stop();

 public:
  template <class F>
  auto run_in_context(F &&f) {
    return run_in_context_impl(*info_->io_worker, std::forward<F>(f));
  }

  // Just syntactic sugar
  void stop() {
    run_in_context([] { SchedulerContext::get().stop(); });
  }

  static void close_scheduler_group(SchedulerGroupInfo &group_info);
};

// Actor messages
class ActorMessageHangup : public core::ActorMessageImpl {
 public:
  void run() override {
    ActorExecuteContext::get().actor().hangup();
  }
};
class ActorMessageHangupShared : public core::ActorMessageImpl {
 public:
  void run() override {
    ActorExecuteContext::get().actor().hangup_shared();
  }
};
}  // namespace core
}  // namespace actor
}  // namespace td
