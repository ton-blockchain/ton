#pragma once
#include <deque>
#include <map>

#include "td/utils/RateLimiterWindow.h"
#include "td/utils/Time.h"

#include "full-node.h"

namespace ton::validator::fullnode {

struct RateLimit {
  double window_size;
  size_t window_limit;
};

template <typename RequestID = int32_t>
class RateLimiter {
 public:
  RateLimiter(RateLimit global_limit, RateLimit heavy_limit, std::set<RequestID> heavy_requests, RateLimit medium_limit,
              std::set<RequestID> medium_requests, RateLimit small_limit, std::set<RequestID> small_requests)
      : global_window_(global_limit.window_size, global_limit.window_limit) {
    category_windows_[0] = td::RateLimiterWindow{heavy_limit.window_size, heavy_limit.window_limit};
    category_windows_[1] = td::RateLimiterWindow{medium_limit.window_size, medium_limit.window_limit};
    category_windows_[2] = td::RateLimiterWindow{small_limit.window_size, small_limit.window_limit};
    for (const RequestID &id : heavy_requests) {
      request_windows_[id] = 0;
    }
    for (const RequestID &id : medium_requests) {
      request_windows_[id] = 1;
    }
    for (const RequestID &id : small_requests) {
      request_windows_[id] = 2;
    }
  }
  RateLimiter(const RateLimiter &) = delete;
  RateLimiter(RateLimiter &&) = delete;
  RateLimiter &operator=(const RateLimiter &) = delete;
  RateLimiter &operator=(RateLimiter &&) = delete;

  bool check_in(RequestID request, size_t cost = 1, td::Timestamp time = td::Timestamp::now());

 private:
  bool check(td::Timestamp time, size_t cost);
  bool check(RequestID request, td::Timestamp time, size_t cost);
  void insert(td::Timestamp time, size_t cost);
  void insert(RequestID request, td::Timestamp time, size_t cost);

  td::RateLimiterWindow global_window_;
  td::RateLimiterWindow category_windows_[3];
  std::map<RequestID, int> request_windows_;
};

template <typename RequestID>
bool RateLimiter<RequestID>::check_in(RequestID request, size_t cost, td::Timestamp time) {
  if (!request_windows_.contains(request)) {
    return true;
  }
  if (cost == 0) {
    cost = 1;
  }
  bool small = request_windows_[request] == 2;
  if ((small || check(time, cost)) && check(request, time, cost)) {
    if (!small) {
      insert(time, cost);
    }
    insert(request, time, cost);
    return true;
  }
  return false;
}

template <typename RequestID>
bool RateLimiter<RequestID>::check(td::Timestamp time, size_t cost) {
  return global_window_.check(time, cost);
}

template <typename RequestID>
bool RateLimiter<RequestID>::check(RequestID request, td::Timestamp time, size_t cost) {
  auto it = request_windows_.find(request);
  return it == request_windows_.end() || category_windows_[it->second].check(time, cost);
}

template <typename RequestID>
void RateLimiter<RequestID>::insert(td::Timestamp time, size_t cost) {
  global_window_.insert(time, cost);
}

template <typename RequestID>
void RateLimiter<RequestID>::insert(RequestID request, td::Timestamp time, size_t cost) {
  if (auto it = request_windows_.find(request); it != request_windows_.end()) {
    category_windows_[it->second].insert(time, cost);
  }
}

constexpr td::uint32 HEAVY_REQUEST_COST_UNIT = 1 << 21;

inline size_t heavy_request_cost(td::uint64 requested_max_size) {
  size_t cost = (requested_max_size + HEAVY_REQUEST_COST_UNIT - 1) / HEAVY_REQUEST_COST_UNIT;
  return cost == 0 ? 1 : cost;
}

inline size_t request_cost_for_limiter(ton_api::Function &function) {
  size_t cost = 1;
  ton_api::downcast_call(
      function, td::overloaded(
                    [&](const ton_api::tonNode_getArchiveSlice &query) {
                      cost = heavy_request_cost(query.max_size_ > 0 ? static_cast<td::uint64>(query.max_size_) : 0);
                    },
                    [&](const ton_api::tonNode_downloadPersistentStateSliceV2 &query) {
                      cost = heavy_request_cost(query.max_size_ > 0 ? static_cast<td::uint64>(query.max_size_) : 0);
                    },
                    [&](const ton_api::tonNode_downloadZeroState &) {
                      cost = heavy_request_cost(FullNode::max_zerostate_size());
                    },
                    [&](const auto &) {}));
  return cost;
}

}  // namespace ton::validator::fullnode
