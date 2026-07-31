/*
    This file is part of TON Blockchain Library.

    TON Blockchain Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    TON Blockchain Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with TON Blockchain Library.  If not, see <http://www.gnu.org/licenses/>.

    Copyright 2017-2020 Telegram Systems LLP
*/
#pragma once

#include "metrics/well-known.h"

namespace ton {

namespace rldp2 {

// Terminal state of a transfer.
#define TON_METRIC_STATE_LIST(F) \
  F(completed)                   \
  F(failed)                      \
  F(timeout)
TON_METRIC_DEFINE_LABEL(State, "state", TON_METRIC_STATE_LIST)
#undef TON_METRIC_STATE_LIST

// RldpIn-owned metric tree (single-threaded on the RldpIn actor). The per-connection wire/dropped
// counters live in RldpConnMetrics and are scraped + accumulated separately; what stays here is the
// transfer outcomes, the gauges, and the app tier. Kept out of rldp-in.hpp so the family names can
// be rendered without pulling in RldpIn, whose Connection type is only complete in rldp.cpp.
struct RldpMetrics {
  struct Transport {
    metrics::Labeled<metrics::Counter, metrics::Direction, State> transfers;
    metrics::Gauge<td::uint64> connections;
    metrics::Gauge<td::uint64> queries_pending;

    void collect(metrics::Context ctx) const {
      ctx.collect(transfers, "transfers");
      ctx.collect(connections, "connections");
      ctx.collect(queries_pending, "queries_pending");
    }
  };

  Transport transport;
  metrics::App app;
  metrics::TlLatencyBucket query_roundtrip{"rldp2 query roundtrip", "seconds"};
  metrics::TlLatencyBucket message_delivery{"rldp2 message delivery", "seconds"};

  void collect(metrics::Context ctx) const {
    ctx.collect(transport, "transport");
    ctx.collect(app, "app");
    ctx.collect(query_roundtrip, "query_roundtrip");
    ctx.collect(message_delivery, "message_delivery");
  }
};

}  // namespace rldp2

}  // namespace ton
