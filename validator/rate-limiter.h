#pragma once
#include <deque>
#include <map>
#include <mutex>

#include "td/utils/Time.h"

namespace ton::validator::fullnode {
struct LimiterWindow {
  double size;
  size_t limit;
  std::deque<td::Timestamp> timestamps = {};

  bool check(td::Timestamp time);
  void insert(td::Timestamp time);
};

inline bool LimiterWindow::check(td::Timestamp time) {
  if (size == 0) {
    return true;
  }
  if (limit == 0) {
    return false;
  }
  return timestamps.size() < limit || time - timestamps.back() > size;
}

inline void LimiterWindow::insert(td::Timestamp time) {
  if (size == 0) {
    return;
  }
  if (timestamps.size() == limit) {
    timestamps.pop_back();
  }
  timestamps.push_front(time);
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
  }

  bool check_in(RequestID request, td::Timestamp time = td::Timestamp::now());

 private:
  bool check(td::Timestamp time);
  bool check(RequestID request, td::Timestamp time);
  void insert(td::Timestamp time);
  void insert(RequestID request, td::Timestamp time);

  const RateLimit global_limit_;
  const std::map<RequestID, RateLimit> request_limits_;

  LimiterWindow global_window_;
  std::map<RequestID, LimiterWindow> request_windows_;

  std::mutex mutex_;
};

template <typename RequestID>
bool RateLimiter<RequestID>::check_in(RequestID request, td::Timestamp time) {
  std::unique_lock lock(mutex_);
  if (check(time) && check(request, time)) {
    insert(time);
    insert(request, time);
    return true;
  }
  return false;
}

template <typename RequestID>
bool RateLimiter<RequestID>::check(td::Timestamp time) {
  if (global_limit_.window_size == 0) {
    return true;
  }
  return global_window_.check(time);
}

template <typename RequestID>
bool RateLimiter<RequestID>::check(RequestID request, td::Timestamp time) {
  if (!request_limits_.contains(request)) {
    return true;
  }
  if (!request_windows_.contains(request)) {
    RateLimit limit = request_limits_.at(request);
    request_windows_.emplace(std::pair{request, LimiterWindow{limit.window_size, limit.window_limit}});
  }
  return request_windows_.at(request).check(time);
}

template <typename RequestID>
void RateLimiter<RequestID>::insert(td::Timestamp time) {
  global_window_.insert(time);
}

template <typename RequestID>
void RateLimiter<RequestID>::insert(RequestID request, td::Timestamp time) {
  if (auto it = request_windows_.find(request); it != request_windows_.end()) {
    it->second.insert(time);
  }
}

}  // namespace ton::validator::fullnode