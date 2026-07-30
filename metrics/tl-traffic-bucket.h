/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#pragma once

#include <atomic>
#include <map>
#include <string>
#include <string_view>

#include "td/utils/Slice.h"
#include "td/utils/Time.h"
#include "td/utils/int_types.h"

#include "collectors.h"

namespace ton::metrics {

// The magic a payload's traffic should be attributed to: the outermost constructor is usually a
// routing envelope (overlay.query, overlay.message, ...), so it is unwrapped down to the content.
// Returns 0 for payloads shorter than a magic.
td::int32 resolve_tl_magic(td::Slice payload);

// For logs: the TL constructor name a payload resolves to ("consensus.simplex.vote"), unwrapping
// routing envelopes the same way traffic accounting does; hex when the schema doesn't know it.
std::string tl_name(td::Slice payload);
std::string tl_name(td::int32 magic);

// Accounts traffic by inner TL constructor. The payload's leading magic is usually a routing
// envelope (overlay.query, overlay.message, ...), so it is unwrapped down to the content it carries;
// a malformed envelope falls back to the envelope itself, never to whatever bytes follow. Schema-known
// magics get their own `tl` cell, while unknown or short payloads funnel into a single "unknown" cell
// so label cardinality stays bounded by the schema.
class TlTrafficBucket {
 public:
  void account(td::Slice payload);
  void account(td::int32 magic, td::uint64 size);

  TlTrafficBucket &operator+=(const TlTrafficBucket &other);

  void collect(Context ctx) const;

 private:
  struct Cell {
    td::uint64 bytes = 0;
    td::uint64 messages = 0;
  };
  std::map<td::int32, Cell> known_;
  Cell unknown_;
};

// A query round trip or a message delivery slower than this gets a log line.
inline constexpr double kSlowSeconds = 1.0;

// ...at most one per site per this many seconds. Slow-operation logging shares the fault it reports:
// when a peer goes away every one of its operations times out at once, so an unthrottled line per
// timeout turns peer loss into a log flood.
inline constexpr double kSlowLogPeriod = 10.0;

// A site keeps its own `static SlowLogThrottle`; take() may be called from any thread.
class SlowLogThrottle {
 public:
  bool take() {
    double now = td::Time::now(), last = last_.load(std::memory_order_relaxed);
    return now - last >= kSlowLogPeriod && last_.compare_exchange_strong(last, now, std::memory_order_relaxed);
  }

 private:
  std::atomic<double> last_{-kSlowLogPeriod};
};

// Query processing latency by TL constructor, with the same schema-bounded label space as
// TlTrafficBucket: magics the schema does not know collapse into a single "unknown" cell, so a peer
// cannot mint labels by sending garbage magics.
class TlLatencyBucket {
 public:
  // `duration_name` is the tail of the histogram family name: collected as "query" with the default
  // it emits <prefix>_query_duration_seconds, collected as "query_roundtrip" with "seconds" it emits
  // <prefix>_query_roundtrip_seconds. The failed counter is always <collect name>_failed_total.
  explicit TlLatencyBucket(std::string_view duration_name = "duration_seconds") : duration_name_(duration_name) {
  }

  void observe(td::int32 magic, double seconds, bool ok);

  void collect(Context ctx) const;

 private:
  struct Cell {
    Histogram<kDurationBuckets> duration;
    Counter failed;
  };
  std::map<td::int32, Cell> known_;
  Cell unknown_;
  std::string duration_name_;
};

// Records how an outbound query round trip / message delivery ended, and logs the slow ones.
// `what` names the operation for the log ("quic query roundtrip"); `throttle` is the caller's own
// static, so one noisy transport cannot mute the others' slow logs.
template <class Dst>
void record_latency(TlLatencyBucket &bucket, SlowLogThrottle &throttle, td::Slice what, td::int32 magic, const Dst &dst,
                    double seconds, bool ok) {
  bucket.observe(magic, seconds, ok);
  if (seconds > kSlowSeconds && throttle.take()) {
    LOG(INFO) << "slow " << what << " tl=" << tl_name(magic) << " dst=" << dst << " time=" << seconds
              << (ok ? "" : " (failed)");
  }
}

}  // namespace ton::metrics
