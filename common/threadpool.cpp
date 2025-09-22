#include "threadpool.hpp"

namespace ton {

ThreadPool::ThreadPool(size_t num_threads) {
  workers_.reserve(num_threads);
  for (size_t id = 0; id < num_threads; id++) {
    workers_.emplace_back([this, id]() -> void { worker_loop(id); });
  }
}

void ThreadPool::stop() {
  std::unique_lock lock(mutex_);
  if (stopped_) {
    return;
  }
  stopped_ = true;
  lock.unlock();
  cond_.notify_all();
}

ThreadPool& ThreadPool::default_threadpool() {
  static ThreadPool threadpool;
  return threadpool;
}

ThreadPool::~ThreadPool() {
  stop();
  for (auto& worker : workers_) {
    worker.join();
  }
}

void ThreadPool::worker_loop(size_t id) {
  while (true) {
    std::unique_lock lock(mutex_);
    cond_.wait(lock, [this]() -> bool { return stopped_ || !tasks_.empty(); });
    if (tasks_.empty()) {  // => stopped
      break;
    }
    auto task = tasks_.front();
    tasks_.pop_front();
    lock.unlock();
    task();
  }

}
}