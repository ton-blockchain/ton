#pragma once

#include <optional>
#include <thread>

#include "td/actor/common.h"
#include "td/utils/ScopeGuard.h"

namespace tonlib {

class ObjectCounter {
 public:
  ObjectCounter() = default;

  td::unique_ptr<td::Guard> new_actor() {
    ++this->count_;
    return td::create_lambda_guard([this] { --this->count_; });
  }

  bool is_zero() {
    return count_ == 0;
  }

 private:
  std::atomic<size_t> count_ = 0;
};

struct Continuation {
  static constexpr uintptr_t resolved_tag = UINTPTR_MAX;
  static constexpr uintptr_t cancel_tag = 0;

  Continuation(const void* value) : Continuation(reinterpret_cast<uintptr_t>(value)) {
  }

  Continuation(uintptr_t value) {
    CHECK(value != resolved_tag && value != cancel_tag);
    this->value = value;
  }

  const void* ptr() const {
    return reinterpret_cast<const void*>(value);
  }

  uintptr_t value;
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
  auto run_in_context(Func&& func) {
    return scheduler_.run_in_context(std::forward<Func>(func));
  }

 private:
  td::actor::Scheduler scheduler_;
  td::thread scheduler_thread_{};

  ObjectCounter object_counter_{};
  std::atomic<bool> is_cancelled_ = false;

  td::MpscPollableQueue<uintptr_t> queue_{};
  size_t queue_size_ = 0;
};

}  // namespace tonlib
