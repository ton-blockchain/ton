#pragma once

#include <array>
#include <coroutine>
#include <deque>
#include <limits>
#include <utility>

#include "td/actor/actor.h"
#include "td/actor/core/ActorExecutor.h"
#include "td/actor/core/ActorInfo.h"
#include "td/actor/core/ActorInfoCreator.h"
#include "td/actor/core/Scheduler.h"
#include "td/actor/core/SchedulerContext.h"
#include "td/actor/coro_task.h"
#include "td/utils/Heap.h"
#include "td/utils/Time.h"
#include "td/utils/port/Poll.h"

namespace td::actor {

class TestScheduler {
 public:
  TestScheduler() {
    poll_.init();
  }

  ~TestScheduler() = default;

  TestScheduler(const TestScheduler &) = delete;
  TestScheduler &operator=(const TestScheduler &) = delete;

  struct WaitSyncWork {
    TestScheduler *sched;

    bool await_ready() const noexcept {
      return false;
    }
    void await_suspend(std::coroutine_handle<> h) noexcept {
      CHECK(!sched->control_continuation_);
      sched->control_continuation_ = h;
    }
    void await_resume() const noexcept {
    }
  };

  WaitSyncWork wait_sync_work() {
    return WaitSyncWork{this};
  }

  std::optional<Timestamp> next_timeout() const {
    if (heap_.empty()) {
      return std::nullopt;
    }
    return heap_.top_key();
  }

  void advance_time(auto delta)
    requires requires {
      { Timestamp::now() + delta } -> std::same_as<Timestamp>;
    }
  {
    Time::jump_in_future((Timestamp::now() + delta).get());
  }

  void advance_time_to(td::Timestamp timestamp) {
    Time::jump_in_future(timestamp.get());
  }

  template <class F>
  void run(F &&coro_factory) {
    Time::FreezeGuard time_freeze;

    ContextImpl context(this);
    core::SchedulerContext::Guard guard(&context);

    auto task = coro_factory().start_immediate();
    while (true) {
      drain_all(false);
      if (!control_continuation_) {
        break;
      }
      std::exchange(control_continuation_, nullptr).resume();
    }
    LOG_CHECK(task.await_ready())
        << "Deadlock detected: control coroutine is suspended but there is no synchronous work to do";
    drain_all(true);
  }

 private:
  std::deque<core::ActorInfoPtr> io_queue_;
  std::deque<core::SchedulerToken> cpu_queue_;

  KHeap<Timestamp> heap_;
  Poll poll_;
  std::array<core::Debug, core::WORKER_KIND_COUNT> debug_;
  std::array<core::ActorTypeStatTable, core::WORKER_KIND_COUNT> actor_type_stats_;
  core::ActorInfoCreator creator_{true};
  std::coroutine_handle<> control_continuation_;
  core::WorkerKind worker_kind_{core::WorkerKind::Cpu};

  size_t worker_index() const {
    return static_cast<size_t>(worker_kind_);
  }

  core::Debug &debug() {
    return debug_[worker_index()];
  }

  class WorkerKindGuard {
   public:
    WorkerKindGuard(TestScheduler *scheduler, core::WorkerKind kind)
        : scheduler_(scheduler), previous_(std::exchange(scheduler_->worker_kind_, kind)) {
    }

    ~WorkerKindGuard() {
      scheduler_->worker_kind_ = previous_;
    }

   private:
    TestScheduler *scheduler_;
    core::WorkerKind previous_;
  };

  class ContextImpl : public core::SchedulerContext {
   public:
    explicit ContextImpl(TestScheduler *sched) : sched_(sched) {
    }

    core::SchedulerId get_scheduler_id() const override {
      return core::SchedulerId{0};
    }

    void add_to_queue(core::ActorInfoPtr actor_info_ptr, core::SchedulerId, bool need_poll) override {
      if (!actor_info_ptr) {
        return;
      }
      if (need_poll) {
        sched_->io_queue_.push_back(std::move(actor_info_ptr));
      } else {
        auto token = static_cast<core::SchedulerToken>(actor_info_ptr.release());
        sched_->cpu_queue_.push_back(token);
      }
    }

    void add_token_to_cpu_queue(core::SchedulerToken token, core::SchedulerId) override {
      sched_->cpu_queue_.push_back(token);
    }

    core::ActorInfoCreator &get_actor_info_creator() override {
      return sched_->creator_;
    }

    bool has_poll() override {
      return true;
    }
    Poll &get_poll() override {
      return sched_->poll_;
    }

    bool has_heap() override {
      return true;
    }
    KHeap<Timestamp> &get_heap() override {
      return sched_->heap_;
    }

    bool is_stop_requested() override {
      return false;
    }
    void stop() override {
    }

    core::Debug &get_debug() override {
      return sched_->debug();
    }

    core::ActorTypeStatTable *actor_type_stats() override {
      return &sched_->actor_type_stats_[sched_->worker_index()];
    }

    void append_actor_type_stats(core::ActorTypeStats &result, double inv_ticks_per_second) override {
      for (size_t i = 0; i < core::WORKER_KIND_COUNT; ++i) {
        auto kind = static_cast<core::WorkerKind>(i);
        core::ActorTypeStatManager::append_thread_stats(result, &sched_->actor_type_stats_[i], sched_->debug_[i], kind,
                                                        inv_ticks_per_second);
      }
    }

    core::SchedulerGroupInfo *scheduler_group() const override {
      return nullptr;
    }

    void set_alarm_timestamp(const core::ActorInfoPtr &actor_info_ptr) override {
      auto &heap = sched_->heap_;
      auto *heap_node = actor_info_ptr->as_heap_node();
      auto timestamp = actor_info_ptr->get_alarm_timestamp();
      if (timestamp) {
        if (heap_node->in_heap()) {
          heap.fix(timestamp, heap_node);
        } else {
          actor_info_ptr->pin(actor_info_ptr);
          heap.insert(timestamp, heap_node);
        }
      } else {
        if (heap_node->in_heap()) {
          actor_info_ptr->unpin();
          heap.erase(heap_node);
        }
      }
    }

   private:
    TestScheduler *sched_;
  };

  void process_cpu_queue() {
    WorkerKindGuard worker_kind(this, core::WorkerKind::Cpu);
    auto &dispatcher = core::SchedulerContext::get();
    while (!cpu_queue_.empty()) {
      auto token = cpu_queue_.front();
      cpu_queue_.pop_front();
      if (!token) {
        continue;
      }
      auto encoded = reinterpret_cast<uintptr_t>(token);
      if ((encoded & 1u) == 0u) {
        auto *raw = reinterpret_cast<core::SchedulerMessage::Raw *>(token);
        core::SchedulerMessage message(core::SchedulerMessage::acquire_t{}, raw);
        auto lock = debug().start(message->get_name());
        core::ActorExecutor executor(*message, dispatcher, core::ActorExecutor::Options().with_from_queue());
      } else {
        auto h = std::coroutine_handle<>::from_address(reinterpret_cast<void *>(encoded & ~uintptr_t(1)));
        auto lock = debug().start("coro", core::Debug::Work::Coroutine);
        h.resume();
      }
    }
  }

  void process_io_queue() {
    WorkerKindGuard worker_kind(this, core::WorkerKind::Io);
    auto &dispatcher = core::SchedulerContext::get();
    while (!io_queue_.empty()) {
      auto actor_info_ptr = std::move(io_queue_.front());
      io_queue_.pop_front();
      if (!actor_info_ptr) {
        continue;
      }
      if (actor_info_ptr->state().get_flags_unsafe().is_shared()) {
        dispatcher.set_alarm_timestamp(actor_info_ptr);
      } else {
        auto lock = debug().start(actor_info_ptr->get_name());
        core::ActorExecutor executor(*actor_info_ptr, dispatcher,
                                     core::ActorExecutor::Options().with_from_queue().with_has_poll(true));
      }
    }
  }

  void process_single_alarm() {
    WorkerKindGuard worker_kind(this, core::WorkerKind::Io);
    auto &dispatcher = core::SchedulerContext::get();
    auto *heap_node = heap_.pop();
    auto *actor_info = core::ActorInfo::from_heap_node(heap_node);
    actor_info->unpin();
    auto lock = debug().start(actor_info->get_name());
    core::ActorExecutor executor(*actor_info, dispatcher, core::ActorExecutor::Options().with_has_poll(true));
    if (executor.can_send_immediate()) {
      executor.send_immediate(core::ActorSignals::one(core::ActorSignals::Alarm));
    } else {
      executor.send(core::ActorSignals::one(core::ActorSignals::Alarm));
    }
  }

  void drain_all(bool advance_time) {
    while (true) {
      while (!cpu_queue_.empty() || !io_queue_.empty()) {
        process_cpu_queue();
        process_io_queue();
      }
      if (heap_.empty()) {
        break;
      }
      if (heap_.top_key() > Timestamp::now()) {
        if (!advance_time) {
          break;
        }
        Time::jump_in_future(heap_.top_key().get());
      }
      process_single_alarm();
    }
  }
};

}  // namespace td::actor
