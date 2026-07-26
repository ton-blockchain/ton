#pragma once

#include <string>
#include <unordered_map>

#include "adnl/utils.hpp"
#include "td/utils/Status.h"
#include "td/utils/Time.h"
#include "td/utils/algorithm.h"

namespace ton::quic {

class QuicConnectionRateLimiters {
 public:
  QuicConnectionRateLimiters(td::uint32 capacity, double period) : capacity_(capacity), period_(period) {
  }

  td::Status take_new_connection(const std::string &addr) {
    if (capacity_ == 0) {
      return td::Status::OK();
    }

    auto [it, _] = limiters_.try_emplace(addr, capacity_, period_);
    if (!it->second.take()) {
      schedule_cleanup();
      return td::Status::Error("new connection rate limit exceeded");
    }
    schedule_cleanup();
    return td::Status::OK();
  }

  void cleanup() {
    if (!(cleanup_at_ && cleanup_at_.is_in_past())) {
      return;
    }
    td::table_remove_if(limiters_, [](const auto &it) { return it.second.is_full(); });
    cleanup_at_ = limiters_.empty() ? td::Timestamp::never() : td::Timestamp::in(10.0);
  }

  td::Timestamp next_cleanup_at() const {
    return cleanup_at_;
  }

 private:
  void schedule_cleanup() {
    if (!cleanup_at_) {
      cleanup_at_ = td::Timestamp::in(10.0);
    }
  }

  td::uint32 capacity_ = 0;
  double period_ = 0.0;
  std::unordered_map<std::string, adnl::RateLimiter> limiters_;
  td::Timestamp cleanup_at_ = td::Timestamp::never();
};

}  // namespace ton::quic
