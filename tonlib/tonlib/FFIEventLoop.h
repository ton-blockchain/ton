#pragma once

#include <condition_variable>
#include <optional>
#include <thread>

#include "td/actor/common.h"
#include "td/utils/ScopeGuard.h"

namespace tonlib {

class ActorCounter {
 public:
  ActorCounter() = default;

  td::unique_ptr<td::Guard> new_actor() {
    ++this->count_;
    return td::create_lambda_guard([this] {
      size_t new_value = --count_;
      if (new_value == 0) {
        std::lock_guard lk(m_);
        cv_.notify_all();
      }
    });
  }

  void wait_zero() {
    if (count_ == 0) {
      return;
    }
    std::unique_lock<std::mutex> lk(m_);
    cv_.wait(lk, [&] { return count_ == 0; });
  }

 private:
  std::atomic<size_t> count_ = 0;
  std::mutex m_;
  std::condition_variable cv_;
};

struct Continuation {
  void const* value;
};

class FFIEventLoop {
 public:
  FFIEventLoop(int threads);
  ~FFIEventLoop();

  void cancel();
  std::optional<Continuation> wait(double timeout);
  td::unique_ptr<td::Guard> new_actor();

  void put(Continuation continuation);

  template <typename Func>
  void run_in_context(Func&& func) {
    scheduler_.run_in_context(std::forward<Func>(func));
  }

 private:
  static constexpr struct CancelTag {
  } cancel_tag_struct;
  static constexpr void const* cancel_tag = &cancel_tag_struct;

  td::actor::Scheduler scheduler_;
  std::thread scheduler_thread_{};

  ActorCounter actor_counter_{};
  std::atomic<bool> is_cancelled_ = false;

  td::MpscPollableQueue<Continuation> queue_{};
  size_t queue_size_ = 0;
};

}  // namespace tonlib
