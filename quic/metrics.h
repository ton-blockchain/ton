/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#pragma once

#include <chrono>

#include "metrics/well-known.h"

namespace ton::quic {

struct ConnectionStats {
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

struct ConnectionStatsAggregate {
  metrics::Counter connections;
  metrics::Gauge<td::uint64> connections_current;
  ConnectionStats stats;

  static ConnectionStatsAggregate from_one(const ConnectionStats& stats) {
    return {
        .connections = 1,
        .connections_current = 1,
        .stats = stats,
    };
  }

  void combine(const ConnectionStatsAggregate& other) {
    auto our = connections_current.value();
    auto their = other.connections_current.value();
    auto conns = our + their;
    std::chrono::duration<double> new_mean_rtt{};
    if (conns > 0) {
      new_mean_rtt = (our * stats.mean_rtt.value() + their * other.stats.mean_rtt.value()) / conns;
    }

    connections += other.connections;
    connections_current.add(other.connections_current.value());
    stats.bytes += other.stats.bytes;
    stats.packets += other.stats.packets;
    stats.stream_bytes += other.stats.stream_bytes;
    stats.bytes_lost += other.stats.bytes_lost;
    stats.packets_lost += other.stats.packets_lost;
    stats.bytes_in_flight.add(other.stats.bytes_in_flight.value());
    stats.bytes_unacked.add(other.stats.bytes_unacked.value());
    stats.bytes_unsent.add(other.stats.bytes_unsent.value());
    stats.sids += other.stats.sids;
    stats.sids_current.add(other.stats.sids_current.value());
    stats.mean_rtt.set(new_mean_rtt);
  }

  ConnectionStatsAggregate& retire() {
    connections_current = 0;
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
  metrics::Labeled<metrics::Counter, metrics::Direction, metrics::Reason> dropped = {};

  TransportStats& operator+=(const TransportStats& other) {
    dropped += other.dropped;
    return *this;
  }

  void collect(metrics::Context ctx) const {
    ctx.collect(dropped, "dropped");
  }
};

struct ServerStats {
  struct Transport {
    ConnectionStatsAggregate summary;
    TransportStats stats;

    void combine(const Transport& other) {
      summary.combine(other.summary);
      stats += other.stats;
    }

    void collect(metrics::Context ctx) const {
      ctx.collect(summary);
      ctx.collect(stats);
    }
  };

  metrics::UdpWireStats wire;
  Transport transport;

  void combine(const ServerStats& other) {
    wire += other.wire;
    transport.combine(other.transport);
  }

  void collect(metrics::Context ctx) const {
    ctx.collect(wire, "wire");
    ctx.collect(transport, "transport");
  }
};

}  // namespace ton::quic
