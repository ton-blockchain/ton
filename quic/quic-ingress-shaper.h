#pragma once

#include <algorithm>
#include <limits>
#include <utility>

#include "td/utils/Time.h"
#include "td/utils/int_types.h"

namespace ton::quic {

// Shared ingress token bucket for one QuicServer (one QUIC port). Owned by the server actor; every
// shaped connection draws from it through a plain pointer — all pimpls run inside the server actor,
// so this is single-threaded state with no atomics. Work-conserving: a single active connection can
// take the full rate when the others are idle. When the bucket can't cover arrivals, the server
// queues the indebted connections and drains them round-robin once per DRAIN_TICK, so a thousand
// small waiters cost one wake per tick, not one wake each.
struct IngressAggregate {
  static constexpr td::uint64 MIN_GRANT = 4096;  // don't dribble sub-4KB MAX_DATA updates
  static constexpr double BURST_SECONDS = 1.0;   // token capacity = rate * this
  static constexpr double DRAIN_TICK = 0.01;     // pace of the shared drain while connections wait

  td::uint64 total_rate = 0;  // bytes/s shared across this server's untrusted inbound connections
  double tokens = 0;
  td::Timestamp last_refill;

  double capacity() const {
    return static_cast<double>(total_rate) * BURST_SECONDS;
  }

  // Grant up to `want` bytes, once the bucket clears the anti-dribble floor. 0 = nothing yet.
  td::uint64 take(td::uint64 want, td::Timestamp now) {
    if (total_rate == 0 || want == 0) {
      return 0;
    }
    refill(now);
    double floor_grant = std::min<double>(static_cast<double>(want), static_cast<double>(MIN_GRANT));
    if (tokens < floor_grant) {
      return 0;
    }
    auto grant = std::min<td::uint64>(want, static_cast<td::uint64>(tokens));
    tokens -= static_cast<double>(grant);
    return grant;
  }

  void refill(td::Timestamp now) {
    if (!last_refill) {  // first use: start full
      last_refill = now;
      tokens = capacity();
      return;
    }
    double dt = std::max(0.0, now.at() - last_refill.at());
    tokens = std::min(capacity(), tokens + static_cast<double>(total_rate) * dt);
    last_refill = now;
  }
};

// Per-connection debt ledger over the connection-level MAX_DATA offset: bytes the peer delivered
// that we have not credited back yet. The pimpl turns each grant into ngtcp2_conn_extend_max_offset;
// the server serves debtors from the shared bucket (inline while nobody waits, paced round-robin
// otherwise). The initial_max_data transport param is the unavoidable per-connection burst;
// per-stream offsets are never shaped.
class IngressShaper {
 public:
  IngressShaper() = default;
  IngressShaper(const IngressShaper &) = delete;
  IngressShaper &operator=(const IngressShaper &) = delete;

  bool shaped() const {
    return agg_ != nullptr;
  }

  // Begin shaping (inbound connection creation, or re-shape on trust downgrade).
  void start(IngressAggregate *agg) {
    if (shaped() || agg->total_rate == 0) {
      return;
    }
    agg_ = agg;
    debt_ = 0;
  }

  // Stop shaping (trust upgrade): returns leftover debt to credit immediately (debt dump).
  td::uint64 stop() {
    if (!shaped()) {
      return 0;
    }
    agg_ = nullptr;
    return std::exchange(debt_, 0);
  }

  void on_data(td::uint64 n) {
    debt_ += n;
  }

  // Grant up to min(debt, cap) from the shared bucket; returns bytes to feed extend_max_offset. The
  // cap is the caller's fair-share ceiling, so no single connection drains the bucket in one pass.
  td::uint64 take(td::Timestamp now, td::uint64 cap = std::numeric_limits<td::uint64>::max()) {
    auto grant = shaped() ? agg_->take(std::min(debt_, cap), now) : 0;
    debt_ -= grant;
    return grant;
  }

  td::uint64 debt() const {
    return debt_;
  }

 private:
  IngressAggregate *agg_ = nullptr;
  td::uint64 debt_ = 0;
};

}  // namespace ton::quic
