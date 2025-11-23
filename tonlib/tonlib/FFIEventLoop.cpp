#include "FFIEventLoop.h"

namespace tonlib {

FFIEventLoop::FFIEventLoop(int threads) : scheduler_(td::actor::Scheduler({threads})) {
  queue_.init();
  scheduler_thread_ = std::thread([&] { scheduler_.run(); });
}

FFIEventLoop::~FFIEventLoop() {
  actor_counter_.wait_zero();
  scheduler_.run_in_context([] { td::actor::SchedulerContext::get()->stop(); });
  scheduler_thread_.join();
}

void FFIEventLoop::cancel() {
  is_cancelled_ = true;
  queue_.writer_put(Continuation::cancel_tag);
}

std::optional<Continuation> FFIEventLoop::wait(double timeout) {
  if (is_cancelled_) {
    return std::nullopt;
  }
  if (queue_size_ == 0) {
    queue_size_ = queue_.reader_wait_nonblock();
  }
  if (queue_size_ == 0 && static_cast<int>(timeout * 1000) != 0) {
    if (timeout < 0) {
      queue_.reader_get_event_fd().wait(-1);
    } else {
      queue_.reader_get_event_fd().wait(static_cast<int>(timeout * 1000));
    }
    queue_size_ = queue_.reader_wait_nonblock();
  }
  if (queue_size_ == 0) {
    return std::nullopt;
  }
  auto entry = queue_.reader_get_unsafe();
  --queue_size_;
  if (entry == Continuation::cancel_tag) {
    CHECK(is_cancelled_);
    return std::nullopt;
  }
  return Continuation{entry};
}

td::unique_ptr<td::Guard> FFIEventLoop::new_actor() {
  return actor_counter_.new_actor();
}

void FFIEventLoop::put(Continuation continuation) {
  queue_.writer_put(continuation.value);
}

}  // namespace tonlib
