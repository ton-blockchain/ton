#pragma once

#include "td/utils/Status.h"
#include "td/utils/logging.h"

namespace ton::quic {

// A per-server accounting budget (0 = unlimited) shared by many connections: reassembly, dispatched
// work, or buffered egress.
struct MemoryPool {
  td::uint64 max_bytes = 0;
  td::uint64 buffered = 0;
  void charge(td::uint64 n) {
    buffered += n;
  }
  void release(td::uint64 n) {
    CHECK(n <= buffered);
    buffered -= n;
  }
  bool would_exceed(td::uint64 n) const {
    return max_bytes != 0 && (buffered > max_bytes || n > max_bytes - buffered);
  }
  // Once over the 90% high watermark, how much to reclaim to fall back to the 75% low watermark.
  // Refusal alone can wedge the pool at capacity for a whole holder lifetime, so past the high
  // watermark the owner sheds the fattest holders instead of only refusing newcomers.
  td::uint64 bytes_to_reclaim() const {
    if (max_bytes == 0 || buffered <= max_bytes / 10 * 9) {
      return 0;  // 0 = unlimited: never reclaim (matches would_exceed treating 0 as no cap)
    }
    return buffered - max_bytes / 4 * 3;
  }
};

// Per-connection byte accounting, mirrored into a shared pool while the connection belongs to one.
// set_pool moves the held amount when the connection's class flips, so the two counters can never
// drift apart; destruction leaves the pool, so a holder can never leak pool bytes.
class ConnectionMemory {
 public:
  ConnectionMemory() = default;
  ConnectionMemory(const ConnectionMemory &) = delete;
  ConnectionMemory &operator=(const ConnectionMemory &) = delete;
  ~ConnectionMemory() {
    set_pool(nullptr);
  }

  void set_pool(MemoryPool *pool) {
    if (pool_ == pool) {
      return;
    }
    if (pool_) {
      pool_->release(buffered_);
    }
    if (pool) {
      pool->charge(buffered_);
    }
    pool_ = pool;
  }
  void charge(td::uint64 n) {
    buffered_ += n;
    if (pool_) {
      pool_->charge(n);
    }
  }
  void release(td::uint64 n) {
    CHECK(n <= buffered_);
    buffered_ -= n;
    if (pool_) {
      pool_->release(n);
    }
  }
  td::uint64 buffered() const {
    return buffered_;
  }
  MemoryPool *pool() const {
    return pool_;
  }
  // Overflow-safe buffered_ + additional > cap. Shared with the guard's stream-open and stream-data
  // classification so their StreamFail label always names the same binding cap as check().
  bool over_own_budget(td::uint64 max_connection_buffered, td::uint64 additional) const {
    return buffered_ > max_connection_buffered || additional > max_connection_buffered - buffered_;
  }
  td::Status check(td::uint64 max_connection_buffered, td::uint64 additional = 0) const {
    if (over_own_budget(max_connection_buffered, additional)) {
      return td::Status::Error(PSLICE() << "connection buffered limit exceeded: " << buffered_ << " + " << additional
                                        << " > " << max_connection_buffered);
    }
    if (pool_ && pool_->would_exceed(additional)) {
      return td::Status::Error(PSLICE() << "pool buffered limit exceeded: " << pool_->buffered << " + " << additional
                                        << " > " << pool_->max_bytes);
    }
    return td::Status::OK();
  }

 private:
  td::uint64 buffered_ = 0;
  MemoryPool *pool_ = nullptr;
};

}  // namespace ton::quic
