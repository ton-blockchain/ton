#pragma once
#include <deque>
#include <map>
#include <mutex>

#include "td/utils/Time.h"

namespace ton::validator::fullnode {
struct LimiterWindow {
  struct Entry {
    td::Timestamp time;
    size_t cost;
  };

  double size;
  size_t limit;
  std::deque<Entry> entries = {};
  size_t used_cost = 0;

  bool check(td::Timestamp time, size_t cost = 1);
  void insert(td::Timestamp time, size_t cost = 1);

 private:
  void gc(td::Timestamp time);
};

inline void LimiterWindow::gc(td::Timestamp time) {
  if (size == 0) {
    return;
  }
  while (!entries.empty() && time - entries.back().time > size) {
    used_cost -= entries.back().cost;
    entries.pop_back();
  }
}

inline bool LimiterWindow::check(td::Timestamp time, size_t cost) {
  if (size == 0) {
    return true;
  }
  if (limit == 0) {
    return false;
  }
  gc(time);
  return cost <= limit && used_cost + cost <= limit;
}

inline void LimiterWindow::insert(td::Timestamp time, size_t cost) {
  if (size == 0) {
    return;
  }
  gc(time);
  entries.push_front(Entry{time, cost});
  used_cost += cost;
}

struct RateLimit {
  double window_size;
  size_t window_limit;
};

template <typename RequestID = int32_t>
class RateLimiter {
 public:
  RateLimiter(RateLimit global_limit, std::map<RequestID, RateLimit> request_limits)
      : global_limit_(global_limit), request_limits_(std::move(request_limits)) {
    global_window_ = {.size = global_limit.window_size, .limit = global_limit.window_limit};
  }

  bool check_in(RequestID request, size_t cost = 1, td::Timestamp time = td::Timestamp::now());

 private:
  bool check(td::Timestamp time, size_t cost);
  bool check(RequestID request, td::Timestamp time, size_t cost);
  void insert(td::Timestamp time, size_t cost);
  void insert(RequestID request, td::Timestamp time, size_t cost);

  const RateLimit global_limit_;
  const std::map<RequestID, RateLimit> request_limits_;

  LimiterWindow global_window_;
  std::map<RequestID, LimiterWindow> request_windows_;

  std::mutex mutex_;
};

template <typename RequestID>
bool RateLimiter<RequestID>::check_in(RequestID request, size_t cost, td::Timestamp time) {
  if (!request_limits_.contains(request)) {
    return true;
  }
  if (cost == 0) {
    cost = 1;
  }
  std::unique_lock lock(mutex_);
  if (check(time, cost) && check(request, time, cost)) {
    insert(time, cost);
    insert(request, time, cost);
    return true;
  }
  return false;
}

template <typename RequestID>
bool RateLimiter<RequestID>::check(td::Timestamp time, size_t cost) {
  if (global_limit_.window_size == 0) {
    return true;
  }
  return global_window_.check(time, cost);
}

template <typename RequestID>
bool RateLimiter<RequestID>::check(RequestID request, td::Timestamp time, size_t cost) {
  if (!request_limits_.contains(request)) {
    return true;
  }
  if (!request_windows_.contains(request)) {
    RateLimit limit = request_limits_.at(request);
    request_windows_.emplace(std::pair{request, LimiterWindow{limit.window_size, limit.window_limit}});
  }
  return request_windows_.at(request).check(time, cost);
}

template <typename RequestID>
void RateLimiter<RequestID>::insert(td::Timestamp time, size_t cost) {
  global_window_.insert(time, cost);
}

template <typename RequestID>
void RateLimiter<RequestID>::insert(RequestID request, td::Timestamp time, size_t cost) {
  if (auto it = request_windows_.find(request); it != request_windows_.end()) {
    it->second.insert(time, cost);
  }
}

}  // namespace ton::validator::fullnode
