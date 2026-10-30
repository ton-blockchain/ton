/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#pragma once

#include <array>
#include <chrono>

#include "metrics/well-known.h"

namespace ton::quic {

// Bucket bounds shared by the batching histograms. They are counts, not durations.
inline constexpr std::array<double, 5> kBatchBuckets = {1, 4, 8, 16, 32};
inline constexpr std::array<double, 6> kBatchWithZeroBuckets = {0, 1, 4, 8, 16, 32};

using BatchHistogram = metrics::Histogram<kBatchBuckets>;
using BatchWithZeroHistogram = metrics::Histogram<kBatchWithZeroBuckets>;

// How a handshake ended: `completed` = the connection is ready to carry traffic, `rejected` = the
// application refused the peer (unusable key, identity mismatch, ...), `timed_out` = it never
// finished within the handshake timeout, so the application never saw it at all. Handshakes
// abandoned for any other reason are visible only as `dropped` and `connections` counts.
#define QUIC_HANDSHAKE_RESULT_LIST(F) \
  F(completed)                        \
  F(rejected)                         \
  F(timed_out)
TON_METRIC_DEFINE_LABEL(HandshakeResult, "result", QUIC_HANDSHAKE_RESULT_LIST)
#undef QUIC_HANDSHAKE_RESULT_LIST

// Which flood-guard budget refused an inbound datagram before any connection state existed. Naming
// the specific budget is the point: it separates a spoofed flood (global_rate, saturated) from a
// single-source flood (conns_per_ip, conn_rate, half_open_per_ip) from an already-punished abuser
// (discouraged) — three attacks that need three different responses.
#define QUIC_ADMIT_REJECT_LIST(F) \
  F(saturated)                    \
  F(discouraged)                  \
  F(conns_per_ip)                 \
  F(conn_rate)                    \
  F(global_rate)                  \
  F(half_open_per_ip)
TON_METRIC_DEFINE_LABEL(AdmitReject, "reason", QUIC_ADMIT_REJECT_LIST)
#undef QUIC_ADMIT_REJECT_LIST

// A connection the guard itself closed, by the code the peer was told. `ok` = an untrusted
// connection reaped for inactivity, `forbidden` = shed by reserved-only, `too_many_requests` = a
// stream-churn abuse verdict, `service_unavailable` = evicted to reclaim capacity.
#define QUIC_GUARD_CLOSE_LIST(F) \
  F(ok)                          \
  F(forbidden)                   \
  F(too_many_requests)           \
  F(service_unavailable)
TON_METRIC_DEFINE_LABEL(GuardClose, "code", QUIC_GUARD_CLOSE_LIST)
#undef QUIC_GUARD_CLOSE_LIST

// Which budget failed a peer's stream. `unclassified` means data arrived before the handshake
// classified the connection; the rest name the cap that was hit.
#define QUIC_STREAM_FAIL_LIST(F) \
  F(size_limit)                  \
  F(deadline)                    \
  F(connection_memory)           \
  F(pool_memory)                 \
  F(inflight)                    \
  F(churn)                       \
  F(unclassified)
TON_METRIC_DEFINE_LABEL(StreamFail, "reason", QUIC_STREAM_FAIL_LIST)
#undef QUIC_STREAM_FAIL_LIST

// Local resource class assigned to a remote peer path. Permanent overlay members are trusted; all
// other peers are untrusted. This controls resource budgets and metric labels, not authorization.
#define QUIC_TRUST_LIST(F) \
  F(trusted)               \
  F(untrusted)
TON_METRIC_DEFINE_LABEL(Trust, "trust", QUIC_TRUST_LIST)
#undef QUIC_TRUST_LIST

// Metrics whose remote peer is known. Trust is attached outside this node, so one snapshot labels
// the request, its answer, and its eventual latency outcome consistently.
struct PeerMetrics {
  metrics::App app;
  metrics::TlLatencyBucket query_roundtrip{"quic query roundtrip", "seconds"};
  metrics::TlLatencyBucket message_confirmation{"quic message confirmation", "seconds"};

  void collect(metrics::Context ctx) const {
    ctx.collect(app, "app");
    ctx.collect(query_roundtrip, "query_roundtrip");
    ctx.collect(message_confirmation, "message_confirmation");
    // Temporary rolling-upgrade alias for dashboards and alerts that still use the old family.
    // Keep this an exact duplicate and remove it once all consumers use message_confirmation.
    ctx.collect(message_confirmation, "message_delivery");
  }
};

// What the flood guard did, and how close it is to its caps. The counters answer "are we under
// attack, and which lever is holding"; the gauges answer "how much headroom is left" — a counter
// that stops climbing means either the attack stopped or a budget is pinned, and only the gauges
// tell those apart.
struct FloodGuardStats {
  metrics::Labeled<metrics::Counter, AdmitReject> admit_rejected;
  metrics::Labeled<metrics::Counter, GuardClose> closed;
  metrics::Labeled<metrics::Counter, StreamFail> streams_failed;
  // Shaped ingress actually handed out: the untrusted aggregate's real throughput.
  metrics::Counter ingress_granted_bytes;

  // Pressure. Gauges are re-read from live guard state at collection, never accumulated. Buffered
  // gauges are logical accounted memory: reassembly storage plus configured StreamState overhead.
  // In-flight gauges also retain request overhead until handler completion, including orphaned work.
  metrics::Gauge<td::uint64> half_open;
  metrics::Gauge<td::uint64> discouraged_ips;
  metrics::Gauge<td::uint64> untrusted_connections;
  metrics::Gauge<td::uint64> shaper_waiters;
  metrics::Labeled<metrics::Gauge<td::uint64>, Trust> buffered_bytes;
  metrics::Labeled<metrics::Gauge<td::uint64>, Trust> inflight_bytes;
  metrics::Gauge<td::uint64> reserved_only;  // 0/1 per server; summed over listening ports

  FloodGuardStats& operator+=(const FloodGuardStats& other) {
    admit_rejected += other.admit_rejected;
    closed += other.closed;
    streams_failed += other.streams_failed;
    ingress_granted_bytes += other.ingress_granted_bytes;
    half_open += other.half_open;
    discouraged_ips += other.discouraged_ips;
    untrusted_connections += other.untrusted_connections;
    shaper_waiters += other.shaper_waiters;
    buffered_bytes += other.buffered_bytes;
    inflight_bytes += other.inflight_bytes;
    reserved_only += other.reserved_only;
    return *this;
  }

  void collect(metrics::Context ctx) const {
    ctx.collect(admit_rejected, "admit_rejected");
    ctx.collect(closed, "closed");
    ctx.collect(streams_failed, "streams_failed");
    ctx.collect(ingress_granted_bytes, "ingress_granted_bytes");
    ctx.collect(half_open, "half_open");
    ctx.collect(discouraged_ips, "discouraged_ips");
    ctx.collect(untrusted_connections, "untrusted_connections");
    ctx.collect(shaper_waiters, "shaper_waiters");
    ctx.collect(buffered_bytes, "buffered_bytes");
    ctx.collect(inflight_bytes, "inflight_bytes");
    ctx.collect(reserved_only, "reserved_only");
  }
};

// Per-connection ngtcp2 counters (metrics::ConnectionStats is a different, shared aggregate).
struct QuicConnectionMetrics {
  metrics::Labeled<metrics::Counter, metrics::Direction> bytes;
  metrics::Labeled<metrics::Counter, metrics::Direction> packets;
  metrics::Labeled<metrics::Counter, metrics::Direction> stream_bytes;
  metrics::Labeled<metrics::Counter, metrics::Direction> datagrams;
  metrics::Counter bytes_lost;
  metrics::Counter packets_lost;
  metrics::Gauge<td::uint64> bytes_in_flight;
  metrics::Gauge<td::uint64> bytes_unacked;
  metrics::Gauge<td::uint64> bytes_unsent;
  metrics::Counter sids;
  metrics::Gauge<td::uint64> sids_current;
  metrics::Gauge<std::chrono::duration<double>> mean_rtt;

  QuicConnectionMetrics& operator+=(const QuicConnectionMetrics& other) {
    bytes += other.bytes;
    packets += other.packets;
    stream_bytes += other.stream_bytes;
    datagrams += other.datagrams;
    bytes_lost += other.bytes_lost;
    packets_lost += other.packets_lost;
    bytes_in_flight += other.bytes_in_flight;
    bytes_unacked += other.bytes_unacked;
    bytes_unsent += other.bytes_unsent;
    sids += other.sids;
    sids_current += other.sids_current;
    mean_rtt += other.mean_rtt;  // a sum is meaningless; the aggregate below overwrites it
    return *this;
  }

  void collect(metrics::Context ctx) const {
    ctx.collect(bytes, "bytes");
    ctx.collect(packets, "packets");
    ctx.collect(stream_bytes, "stream_bytes");
    ctx.collect(datagrams, "datagrams");
    ctx.collect(bytes_lost, "bytes_lost");
    ctx.collect(packets_lost, "packets_lost");
    ctx.collect(bytes_in_flight, "bytes_in_flight");
    ctx.collect(bytes_unacked, "bytes_unacked");
    ctx.collect(bytes_unsent, "bytes_unsent");
    ctx.collect(sids, "sids");
    ctx.collect(sids_current, "sids_current");
    ctx.collect(mean_rtt, "mean_rtt");
  }
};

struct QuicConnectionMetricsAggregate {
  // Split by who dialled, so inbound and outbound load can be told apart; `stats` stays pooled,
  // since the per-connection ngtcp2 counters carry no direction of their own.
  metrics::Labeled<metrics::Counter, metrics::Direction> connections;
  metrics::Labeled<metrics::Gauge<td::uint64>, metrics::Direction> connections_current;
  QuicConnectionMetrics stats;

  static QuicConnectionMetricsAggregate from_one(const QuicConnectionMetrics& stats, bool is_outbound) {
    QuicConnectionMetricsAggregate res;
    auto direction = is_outbound ? metrics::Direction::out : metrics::Direction::in;
    res.connections.at(direction).inc();
    res.connections_current.at(direction).set(1);
    res.stats = stats;
    return res;
  }

  td::uint64 current_total() const {
    return connections_current.at(metrics::Direction::in).value() +
           connections_current.at(metrics::Direction::out).value();
  }

  QuicConnectionMetricsAggregate& operator+=(const QuicConnectionMetricsAggregate& other) {
    // mean_rtt is an average over connections, not a sum: re-weight it by both sides' connection
    // counts, then write it back over the sum the merge below leaves in place.
    auto our = current_total();
    auto their = other.current_total();
    auto conns = our + their;
    std::chrono::duration<double> mean_rtt{};
    if (conns > 0) {
      mean_rtt = (our * stats.mean_rtt.value() + their * other.stats.mean_rtt.value()) / conns;
    }

    connections += other.connections;
    connections_current += other.connections_current;
    stats += other.stats;
    stats.mean_rtt.set(mean_rtt);
    return *this;
  }

  QuicConnectionMetricsAggregate& retire() {
    connections_current = {};
    stats.bytes_in_flight = {};
    stats.bytes_unacked = {};
    stats.bytes_unsent = {};
    stats.sids_current = {};
    stats.mean_rtt = {};
    return *this;
  }

  void collect(metrics::Context ctx) const {
    ctx.collect(connections, "connections");
    ctx.collect(connections_current, "connections_current");
    ctx.collect(stats);
  }
};

struct TransportStats {
  metrics::Labeled<metrics::Counter, metrics::Direction, metrics::Reason> dropped;
  // `in` is a peer dialling us, `out` is us dialling a peer: a rejection means something very
  // different on each side, so the two are never mixed.
  metrics::Labeled<metrics::Counter, metrics::Direction, HandshakeResult> handshakes;

  TransportStats& operator+=(const TransportStats& other) {
    dropped += other.dropped;
    handshakes += other.handshakes;
    return *this;
  }

  void collect(metrics::Context ctx) const {
    ctx.collect(dropped, "dropped");
    ctx.collect(handshakes, "handshakes");
  }
};

struct BatchingStats {
  BatchWithZeroHistogram egress_flush_packets;
  BatchHistogram egress_gso_segments;
  BatchHistogram egress_syscall_messages;

  BatchingStats& operator+=(const BatchingStats& other) {
    egress_flush_packets += other.egress_flush_packets;
    egress_gso_segments += other.egress_gso_segments;
    egress_syscall_messages += other.egress_syscall_messages;
    return *this;
  }

  void collect(metrics::Context ctx) const {
    auto egress = ctx.with_name("egress");
    egress.collect(egress_flush_packets, "flush_packets");
    egress.collect(egress_gso_segments, "gso_segments");
    egress.collect(egress_syscall_messages, "syscall_messages");
  }
};

struct ServerStats {
  struct Transport {
    QuicConnectionMetricsAggregate summary;
    TransportStats stats;

    Transport& operator+=(const Transport& other) {
      summary += other.summary;
      stats += other.stats;
      return *this;
    }

    void collect(metrics::Context ctx) const {
      ctx.collect(summary);
      ctx.collect(stats);
    }
  };

  metrics::UdpWireStats wire;
  Transport transport;
  BatchingStats batching;
  FloodGuardStats flood;

  ServerStats& operator+=(const ServerStats& other) {
    wire += other.wire;
    transport += other.transport;
    batching += other.batching;
    flood += other.flood;
    return *this;
  }

  void collect(metrics::Context ctx) const {
    ctx.collect(wire, "wire");
    ctx.collect(transport, "transport");
    ctx.collect(batching, "batching");
    ctx.collect(flood, "flood");
  }
};

}  // namespace ton::quic
