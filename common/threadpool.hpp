#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <variant>

namespace ton {

class ThreadPool {
  template <typename T>
  class Promise;

 public:
  template <typename T>
  class Future {
   public:
    T await();

    ~Future();

   private:
    explicit Future(std::shared_ptr<Promise<T>> promise);

    friend class ThreadPool;
    std::shared_ptr<Promise<T>> promise_;
  };

  explicit ThreadPool(size_t num_threads = std::thread::hardware_concurrency());

  template <typename T, typename Functor>
  Future<T> submit(Functor fun);
  void stop();

  static ThreadPool& default_threadpool();

  ~ThreadPool();

  template <typename Iter, typename OIter>
  static void invoke_task_group(Iter tasks_beg, Iter tasks_end, OIter res_beg, size_t num_threads = 0);

 private:
  template <typename T>
  class Promise {
   public:
    Promise(ThreadPool& pool);
    void fulfill(const T& result);
    void fail(const std::exception_ptr& error);

   private:
    friend class Future<T>;
    std::mutex mutex_;
    std::condition_variable cond_;
    std::variant<std::monostate, T, std::exception_ptr> result_{std::monostate()};
    ThreadPool& pool_;
  };

  void worker_loop(size_t id);

  std::vector<std::thread> workers_;

  std::mutex mutex_;
  std::condition_variable cond_;
  std::deque<std::function<void()>> tasks_;
  bool stopped_ = false;
};

template <typename T>
T ThreadPool::Future<T>::await() {
  while (true) {
    {
      std::lock_guard lock_(promise_->mutex_);
      if (promise_->result_.index() > 0) {
        break;
      }
    }
    {
      std::unique_lock lock(promise_->pool_.mutex_);
      if (promise_->pool_.tasks_.empty()) {
        break;
      }
      auto task = promise_->pool_.tasks_.front();
      promise_->pool_.tasks_.pop_front();
      lock.unlock();
      task();
    }
  }
  std::unique_lock lock_(promise_->mutex_);
  promise_->cond_.wait(lock_, [this]() -> bool { return promise_->result_.index() > 0; });
  if (promise_->result_.index() == 2) {
    std::rethrow_exception(std::get<2>(promise_->result_));
  }
  return std::get<1>(promise_->result_);
}

template <typename T>
ThreadPool::Future<T>::~Future() {
}

template <typename T>
ThreadPool::Future<T>::Future(std::shared_ptr<Promise<T>> promise) : promise_(promise) {
}

template <typename T, typename Functor>
ThreadPool::Future<T> ThreadPool::submit(Functor fun) {
  auto promise = std::make_shared<Promise<T>>(*this);
  auto task = [promise, fun]() -> void {
    try {
      promise->fulfill(fun());
    } catch (...) {
      promise->fail(std::current_exception());
    }
  };
  std::unique_lock lock(mutex_);
  tasks_.emplace_back(task);
  lock.unlock();
  cond_.notify_one();
  return Future<T>(promise);
}

template <typename Iter, typename  OIter>
void ThreadPool::invoke_task_group(Iter tasks_beg, Iter tasks_end, OIter res_beg, size_t num_threads) {
  if (num_threads == 0) {
    num_threads = std::thread::hardware_concurrency();
  }
  size_t n = std::distance(tasks_beg, tasks_end);
  auto* in = std::addressof(*tasks_beg);
  auto* out = std::addressof(*res_beg);
  if (n == 1) {
    out[0] = in[0]();
    return;
  }
  num_threads = std::min(num_threads, n);
  std::atomic_size_t cur_pos{0};
  std::mutex panic_mutex;
  std::optional<std::exception_ptr> error;

  {
    std::vector<std::thread> workers;
    workers.reserve(num_threads);
    for (size_t id = 0; id < num_threads && cur_pos.load() < n; id++) {
      workers.emplace_back([&cur_pos, n, in, out, &error, &panic_mutex]() -> void {
        while (true) {
          size_t pos = cur_pos.fetch_add(1);
          if (pos >= n) {
            break;
          }
          try {
            out[pos] = in[pos]();
          } catch (...) {
            std::lock_guard panic_lock(panic_mutex);
            error = std::current_exception();
          }
        }
      });
    }

    for (auto &worker : workers) {
      worker.join();
    }
  }

  if (cur_pos.load() < n) {
    throw std::runtime_error("assertion failed");
  }

  if (error.has_value()) {
    std::rethrow_exception(error.value());
  }
}

template <typename T>
ThreadPool::Promise<T>::Promise(ThreadPool& pool) : pool_(pool) {
}

template <typename T>
void ThreadPool::Promise<T>::fulfill(const T& result) {
  std::unique_lock lock(mutex_);
  result_.template emplace<1>(result);
  lock.unlock();
  cond_.notify_all();
}

template <typename T>
void ThreadPool::Promise<T>::fail(const std::exception_ptr& error) {
  std::unique_lock lock(mutex_);
  result_.template emplace<2>(error);
  lock.unlock();
  cond_.notify_all();
}

}