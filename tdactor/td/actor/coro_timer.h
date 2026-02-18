#pragma once

#include <atomic>
#include <coroutine>
#include <cstdint>
#include <utility>

#include "td/actor/core/SchedulerContext.h"
#include "td/actor/coro_task.h"
#include "td/actor/coro_types.h"
#include "td/utils/Heap.h"
#include "td/utils/Time.h"

namespace td::actor {

struct TimerNode : public HeapNode, public HeapCancelNode {
  enum State : uint8_t { WAITING = 0, FIRED = 1, CANCELLED = 2 };

  static std::pair<Ref<TimerNode>, Ref<TimerNode>> create(std::coroutine_handle<> continuation, Timestamp deadline) {
    auto owned_by_scheduler = Ref<TimerNode>::adopt(new TimerNode(continuation, deadline));
    auto owned_by_awaiter = Ref<TimerNode>::share(owned_by_scheduler.get());
    return {std::move(owned_by_scheduler), std::move(owned_by_awaiter)};
  }

  ~TimerNode() override {
    if (continuation_) {
      continuation_.destroy();
    }
  }

  Timestamp get_deadline() const {
    return deadline_;
  }

  std::coroutine_handle<> try_claim(State new_state) {
    auto expected = WAITING;
    if (state_.compare_exchange_strong(expected, new_state, std::memory_order_acq_rel)) {
      return std::exchange(continuation_, {});
    }
    return {};
  }

  bool is_cancelled() const {
    return state_.load(std::memory_order_acquire) == CANCELLED;
  }

  void do_cancel() override {
    auto* context = core::SchedulerContext::get_ptr();
    // Timer cancel routing uses scheduler context for queueing cancel messages.
    CHECK(context);
    if (auto cont = try_claim(CANCELLED)) {
      add_ref();  // for cancel message
      context->cancel_timer(Ref<TimerNode>::adopt(this));
      detail::resume_on_current_tls(cont);
    }
  }

  static Ref<TimerNode> from_heap_node(HeapNode* node) {
    return Ref<TimerNode>::adopt(static_cast<TimerNode*>(node));
  }

  static void process_expired(Ref<TimerNode> ref, core::SchedulerContext& dispatcher) {
    if (auto cont = ref->try_claim(FIRED)) {
      detail::resume_root(cont);
    }
  }

  static void process_register(Ref<TimerNode> ref, KHeap<double>& timer_heap) {
    if (ref->is_cancelled()) {
      return;
    }
    auto deadline = ref->deadline_;
    timer_heap.insert(deadline.at(), ref.release());
  }

  static void process_cancel(Ref<TimerNode> ref, KHeap<double>& timer_heap) {
    if (ref->in_heap()) {
      auto heap_ref = from_heap_node(ref.get());
      timer_heap.erase(heap_ref.get());
    }
  }

 private:
  // initial_refs=2: one for scheduler (register_timer), one for awaiter (timer_ref_).
  // A third ref is added if published to a cancellation topology (on_publish).
  TimerNode(std::coroutine_handle<> cont, Timestamp deadline)
      : HeapCancelNode(2), continuation_(cont), deadline_(deadline) {
  }

  std::coroutine_handle<> continuation_;
  std::atomic<State> state_{WAITING};
  Timestamp deadline_;
};

class SleepAwaitable {
 public:
  explicit SleepAwaitable(Timestamp deadline) : deadline_(deadline) {
  }

  SleepAwaitable(const SleepAwaitable&) = delete;
  SleepAwaitable& operator=(const SleepAwaitable&) = delete;

  SleepAwaitable(SleepAwaitable&& other) noexcept = default;

  SleepAwaitable& operator=(SleepAwaitable&&) = delete;

  bool await_ready() const noexcept {
    return deadline_.is_in_past();
  }

  void await_suspend(std::coroutine_handle<> handle) {
    auto [a, b] = TimerNode::create(handle, deadline_);
    core::SchedulerContext::get().register_timer(std::move(a));
    timer_ref_ = std::move(b);
    auto lease = current_scope_lease();
    if (lease) {
      lease.publish_heap_cancel_node(*timer_ref_);
    }
  }

  void await_resume() noexcept {
    if (timer_ref_) {
      timer_ref_->disarm();
      timer_ref_.reset();
    }
  }

 private:
  Timestamp deadline_;
  Ref<TimerNode> timer_ref_;
};

inline SleepAwaitable sleep_for(double seconds) {
  return SleepAwaitable(Timestamp::in(seconds));
}

inline SleepAwaitable sleep_until(Timestamp deadline) {
  return SleepAwaitable(deadline);
}

}  // namespace td::actor
