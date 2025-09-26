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

template <typename Iter, typename OIter>
void invoke_task_group(Iter tasks_beg, Iter tasks_end, OIter res_beg, size_t num_threads = 0) {
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
  std::mutex error_mutex;
  std::optional<std::exception_ptr> error;

  {
    std::vector<std::thread> workers;
    workers.reserve(num_threads);
    for (size_t id = 0; id < num_threads && cur_pos.load() < n; id++) {
      workers.emplace_back([&cur_pos, n, in, out, &error, &error_mutex]() -> void {
        while (true) {
          size_t pos = cur_pos.fetch_add(1);
          if (pos >= n) {
            break;
          }
          try {
            out[pos] = in[pos]();
          } catch (...) {
            std::lock_guard error_lock(error_mutex);
            error = std::current_exception();
          }
        }
      });
    }

    for (auto& worker : workers) {
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

}  // namespace ton