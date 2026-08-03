/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#pragma once

#include <atomic>
#include <map>
#include <optional>
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

// The constructor's name as the schema spells it, or nullopt when the schema does not know the
// magic. Only a known magic pays for the owning string, so a miss stays allocation-free.
std::optional<std::string> tl_schema_name(td::int32 magic);

// For logs: the TL constructor name a payload resolves to ("consensus.simplex.vote"), unwrapping
// routing envelopes the same way traffic accounting does; hex when the schema doesn't know it.
std::string tl_name(td::Slice payload);
std::string tl_name(td::int32 magic);

// Cells keyed by TL constructor, with the label space bounded by the schema: magics the schema does
// not know share one "unknown" cell, so a peer cannot mint labels by sending garbage magics.
template <class Cell>
class TlCells {
 public:
  Cell &at(td::int32 magic) {
    // tl_schema_name() builds an owning string, so it is only asked about a magic we have not seen.
    auto it = known_.find(magic);
    if (it != known_.end()) {
      return it->second;
    }
    if (!tl_schema_name(magic).has_value()) {
      return unknown_;
    }
    return known_.emplace(magic, Cell{}).first->second;
  }

  // `emit` renders one cell into a Context already labelled with its constructor. Every cell re-emits
  // the same family sequence into the same slots, so each one rewinds first.
  void collect(Context ctx, auto &&emit) const {
    size_t start = ctx.mark();
    for (const auto &[magic, cell] : known_) {
      ctx.rewind(start);
      emit(ctx.with_label("tl", *tl_schema_name(magic)), cell);
    }
    // The unknown cell goes last and is emitted even when empty: the Sink identifies families by
    // position, so the family sequence must not depend on which cells happen to be populated.
    ctx.rewind(start);
    emit(ctx.with_label("tl", "unknown"), unknown_);
  }

  TlCells &operator+=(const TlCells &other) {
    for (const auto &[magic, cell] : other.known_) {
      known_[magic] += cell;
    }
    unknown_ += other.unknown_;
    return *this;
  }

 private:
  std::map<td::int32, Cell> known_;
  Cell unknown_;
};

// Accounts traffic by inner TL constructor. The payload's leading magic is usually a routing
// envelope (overlay.query, overlay.message, ...), so it is unwrapped down to the content it carries;
// a malformed envelope falls back to the envelope itself, never to whatever bytes follow.
class TlTrafficBucket {
 public:
  void account(td::Slice payload);
  void account(td::int32 magic, td::uint64 size);

  TlTrafficBucket &operator+=(const TlTrafficBucket &other);

  void collect(Context ctx) const;

 private:
  struct Cell {
    Counter bytes;
    Counter messages;

    Cell &operator+=(const Cell &other) {
      bytes += other.bytes;
      messages += other.messages;
      return *this;
    }
  };
  TlCells<Cell> tl_;
};

// A query round trip or a message delivery slower than this gets a log line.
inline constexpr double kSlowSeconds = 1.0;

// ...at most one per site per this many seconds. Slow-operation logging shares the fault it reports:
// when a peer goes away every one of its operations times out at once, so an unthrottled line per
// timeout turns peer loss into a log flood.
inline constexpr double kSlowLogPeriod = 10.0;

// TEMPORARY: the slow-operation logs are a diagnostic to be removed once the latency histograms are
// trusted in production.
// take() may be called from any thread: the standalone throttles fire from arbitrary threads, and
// even where the caller is single-threaded (a bucket's record() runs only on its owning actor
// thread) the atomic is the minimal guard that covers both.
class SlowLogThrottle {
 public:
  bool take() {
    double now = td::Time::now(), last = last_.load(std::memory_order_relaxed);
    return now - last >= kSlowLogPeriod && last_.compare_exchange_strong(last, now, std::memory_order_relaxed);
  }

 private:
  std::atomic<double> last_{-kSlowLogPeriod};
};

// Latency of one kind of operation, split by TL constructor.
class TlLatencyBucket {
 public:
  // `what` names the operation in the slow log ("quic query roundtrip"). `duration_name` is the tail
  // of the histogram family name: collected as "query" with the default it emits
  // <prefix>_query_duration_seconds, collected as "query_roundtrip" with "seconds" it emits
  // <prefix>_query_roundtrip_seconds. The failed counter is always <collect name>_failed_total.
  explicit TlLatencyBucket(std::string_view what, std::string_view duration_name = "duration_seconds")
      : what_(what), duration_name_(duration_name) {
  }

  void observe(td::int32 magic, double seconds, bool ok);

  // Records how an operation ended and logs it when slow. The throttle is per bucket, so one noisy
  // transport cannot mute the others' slow logs.
  void record(td::int32 magic, const auto &peer, double seconds, bool ok) {
    observe(magic, seconds, ok);
    if (seconds > kSlowSeconds && throttle_.take()) {
      LOG(INFO) << "slow " << what_ << " tl=" << tl_name(magic) << " peer=" << peer << " time=" << seconds
                << (ok ? "" : " (failed)");
    }
  }

  void collect(Context ctx) const;

 private:
  struct Cell {
    Histogram<kDurationBuckets> duration;
    Counter failed;
  };
  TlCells<Cell> tl_;
  SlowLogThrottle throttle_;
  std::string what_;
  std::string duration_name_;
};

}  // namespace ton::metrics
