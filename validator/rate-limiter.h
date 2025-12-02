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

template <typename PeerID, typename RequestID>
class RateLimiter {
 public:
  RateLimiter(RateLimit global_limit, std::map<RequestID, RateLimit> request_limits)
      : global_limit_(global_limit), request_limits_(std::move(request_limits)) {
  }

  bool check_in(PeerID peer, RequestID request, td::Timestamp time = td::Timestamp::now());

 private:
  bool check(PeerID peer, td::Timestamp time);
  bool check(PeerID peer, RequestID request, td::Timestamp time);
  void insert(PeerID peer, td::Timestamp time);
  void insert(PeerID peer, RequestID request, td::Timestamp time);

  const RateLimit global_limit_;
  const std::map<RequestID, RateLimit> request_limits_;

  std::map<PeerID, LimiterWindow> global_windows_;
  std::map<std::pair<PeerID, RequestID>, LimiterWindow> request_windows_;

  std::mutex mutex_;
};

template <typename PeerID, typename RequestID>
bool RateLimiter<PeerID, RequestID>::check_in(PeerID peer, RequestID request, td::Timestamp time) {
  std::unique_lock lock(mutex_);
  if (check(peer, time) && check(peer, request, time)) {
    insert(peer, time);
    insert(peer, request, time);
    return true;
  }
  return false;
}

template <typename PeerID, typename RequestID>
bool RateLimiter<PeerID, RequestID>::check(PeerID peer, td::Timestamp time) {
  if (global_limit_.window_size == 0) {
    return true;
  }
  if (!global_windows_.contains(peer)) {
    global_windows_.emplace(std::pair{peer, LimiterWindow{global_limit_.window_size, global_limit_.window_limit}});
  }
  return global_windows_.at(peer).check(time);
}

template <typename PeerID, typename RequestID>
bool RateLimiter<PeerID, RequestID>::check(PeerID peer, RequestID request, td::Timestamp time) {
  if (!request_limits_.contains(request)) {
    return true;
  }
  if (!request_windows_.contains({peer, request})) {
    RateLimit limit = request_limits_.at(request);
    request_windows_.emplace(std::pair{std::pair{peer, request}, LimiterWindow{limit.window_size, limit.window_limit}});
  }
  return request_windows_.at({peer, request}).check(time);
}

template <typename PeerID, typename RequestID>
void RateLimiter<PeerID, RequestID>::insert(PeerID peer, td::Timestamp time) {
  if (auto it = global_windows_.find(peer); it != global_windows_.end()) {
    it->second.insert(time);
  }
}

template <typename PeerID, typename RequestID>
void RateLimiter<PeerID, RequestID>::insert(PeerID peer, RequestID request, td::Timestamp time) {
  if (auto it = request_windows_.find({peer, request}); it != request_windows_.end()) {
    it->second.insert(time);
  }
}

}  // namespace ton::validator::fullnode