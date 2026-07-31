/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#pragma once

#include <chrono>

#include "metrics/well-known.h"

namespace ton::quic {

// How the application answered a handshake that reached it: `completed` = the connection is ready
// to carry traffic, `rejected` = the peer was refused (unusable key, identity mismatch, ...).
// Handshakes that never got that far are visible only as `dropped` and `connections` counts.
#define QUIC_HANDSHAKE_RESULT_LIST(F) \
  F(completed)                        \
  F(rejected)
TON_METRIC_DEFINE_LABEL(HandshakeResult, "result", QUIC_HANDSHAKE_RESULT_LIST)
#undef QUIC_HANDSHAKE_RESULT_LIST

// Per-connection ngtcp2 counters (metrics::ConnectionStats is a different, shared aggregate).
struct QuicConnectionMetrics {
  metrics::Labeled<metrics::Counter, metrics::Direction> bytes;
  metrics::Labeled<metrics::Counter, metrics::Direction> packets;
  metrics::Labeled<metrics::Counter, metrics::Direction> stream_bytes;
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

  ServerStats& operator+=(const ServerStats& other) {
    wire += other.wire;
    transport += other.transport;
    return *this;
  }

  void collect(metrics::Context ctx) const {
    ctx.collect(wire, "wire");
    ctx.collect(transport, "transport");
  }
};

}  // namespace ton::quic
