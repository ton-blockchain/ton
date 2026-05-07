#pragma once

#include <coroutine>
#include <deque>
#include <limits>

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

  double next_timeout_in() const {
    if (heap_.empty()) {
      return std::numeric_limits<double>::infinity();
    }
    double result = heap_.top_key() - Time::now();
    return result > 0 ? result : 0;
  }

  void advance_time(double delta) {
    Time::jump_in_future(Time::now() + delta);
  }

  void advance_time(std::chrono::duration<double> delta) {
    Time::jump_in_future(Time::now() + delta.count());
  }

  void advance_time_to(td::Timestamp timestamp) {
    Time::jump_in_future(timestamp.at());
  }

  template <class F>
  void run(F &&coro_factory) {
    Time::freeze();
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

    Time::unfreeze();
  }

 private:
  std::deque<core::ActorInfoPtr> io_queue_;
  std::deque<core::SchedulerToken> cpu_queue_;

  KHeap<double> heap_;
  Poll poll_;
  core::Debug debug_;
  core::ActorInfoCreator creator_{true};
  std::coroutine_handle<> control_continuation_;

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
    KHeap<double> &get_heap() override {
      return sched_->heap_;
    }

    bool is_stop_requested() override {
      return false;
    }
    void stop() override {
    }

    core::Debug &get_debug() override {
      return sched_->debug_;
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
          heap.fix(timestamp.at(), heap_node);
        } else {
          actor_info_ptr->pin(actor_info_ptr);
          heap.insert(timestamp.at(), heap_node);
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
        auto lock = debug_.start(message->get_name());
        core::ActorExecutor executor(*message, dispatcher, core::ActorExecutor::Options().with_from_queue());
      } else {
        auto h = std::coroutine_handle<>::from_address(reinterpret_cast<void *>(encoded & ~uintptr_t(1)));
        auto lock = debug_.start("coro");
        h.resume();
      }
    }
  }

  void process_io_queue() {
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
        auto lock = debug_.start(actor_info_ptr->get_name());
        core::ActorExecutor executor(*actor_info_ptr, dispatcher,
                                     core::ActorExecutor::Options().with_from_queue().with_has_poll(true));
      }
    }
  }

  void process_single_alarm() {
    auto &dispatcher = core::SchedulerContext::get();
    auto *heap_node = heap_.pop();
    auto *actor_info = core::ActorInfo::from_heap_node(heap_node);
    actor_info->unpin();
    auto lock = debug_.start(actor_info->get_name());
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
      if (heap_.top_key() > Time::now()) {
        if (!advance_time) {
          break;
        }
        Time::jump_in_future(heap_.top_key());
      }
      process_single_alarm();
    }
  }
};

}  // namespace td::actor
