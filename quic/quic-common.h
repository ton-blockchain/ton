#pragma once

#include <compare>
#include <cstdint>
#include <cstring>

#include "td/utils/Random.h"
#include "td/utils/Slice.h"
#include "td/utils/Status.h"
#include "td/utils/StringBuilder.h"
#include "td/utils/misc.h"
#include "td/utils/port/IPAddress.h"

namespace ton::quic {
using QuicStreamID = int64_t;

struct QuicConnectionStats {
  int64_t bytes_rx = 0, bytes_tx = 0, bytes_lost = 0;
  int64_t bytes_unacked = 0, bytes_unsent = 0;
  int64_t total_sids = 0, open_sids = 0;
  double mean_rtt = 0;
  // Extended (per-conn snapshot, summed only by absolute counts):
  int64_t pkt_sent = 0, pkt_recv = 0, pkt_lost = 0;
  int64_t bytes_in_flight = 0;
  int64_t cwnd = 0;
  // RTT samples in seconds (per-conn snapshots; aggregated as a sum, not a useful average):
  double min_rtt_s = 0;
  double latest_rtt_s = 0;
  double rttvar_s = 0;
  // ngtcp2 stream-data boundary: bytes the application asked ngtcp2 to send on streams (cumulative
  // input to buffer_stream) vs bytes ngtcp2 yielded back via stream-data callbacks. The diff
  // between these and the bytes_tx/bytes_rx packet-level counters is QUIC framing overhead.
  int64_t stream_bytes_buffered = 0;
  int64_t stream_bytes_received = 0;

  // Summing QuicConnectionStats is only meaningful for the cumulative counters and the
  // snapshot "current state" gauges (bytes_in_flight, cwnd, open_sids). RTT fields are
  // per-connection observations and the outer Stats::Entry aggregator reweights them — here
  // they are left at zero so nobody can accidentally surface a sum-of-RTTs as a metric.
  QuicConnectionStats operator+(const QuicConnectionStats& other) const {
    return {
        .bytes_rx = bytes_rx + other.bytes_rx,
        .bytes_tx = bytes_tx + other.bytes_tx,
        .bytes_lost = bytes_lost + other.bytes_lost,
        .bytes_unacked = bytes_unacked + other.bytes_unacked,
        .bytes_unsent = bytes_unsent + other.bytes_unsent,
        .total_sids = total_sids + other.total_sids,
        .open_sids = open_sids + other.open_sids,
        .mean_rtt = 0,
        .pkt_sent = pkt_sent + other.pkt_sent,
        .pkt_recv = pkt_recv + other.pkt_recv,
        .pkt_lost = pkt_lost + other.pkt_lost,
        .bytes_in_flight = bytes_in_flight + other.bytes_in_flight,
        .cwnd = cwnd + other.cwnd,
        .min_rtt_s = 0,
        .latest_rtt_s = 0,
        .rttvar_s = 0,
        .stream_bytes_buffered = stream_bytes_buffered + other.stream_bytes_buffered,
        .stream_bytes_received = stream_bytes_received + other.stream_bytes_received,
    };
  }

  // Subtraction is used for two-snapshot deltas over counters; RTT fields are gauges for
  // which subtraction has no meaning, so they keep the later value.
  QuicConnectionStats operator-(const QuicConnectionStats& other) const {
    return {
        .bytes_rx = bytes_rx - other.bytes_rx,
        .bytes_tx = bytes_tx - other.bytes_tx,
        .bytes_lost = bytes_lost - other.bytes_lost,
        .bytes_unacked = bytes_unacked - other.bytes_unacked,
        .bytes_unsent = bytes_unsent - other.bytes_unsent,
        .total_sids = total_sids - other.total_sids,
        .open_sids = open_sids - other.open_sids,
        .mean_rtt = mean_rtt,
        .pkt_sent = pkt_sent - other.pkt_sent,
        .pkt_recv = pkt_recv - other.pkt_recv,
        .pkt_lost = pkt_lost - other.pkt_lost,
        .bytes_in_flight = bytes_in_flight - other.bytes_in_flight,
        .cwnd = cwnd - other.cwnd,
        .min_rtt_s = min_rtt_s,
        .latest_rtt_s = latest_rtt_s,
        .rttvar_s = rttvar_s,
        .stream_bytes_buffered = stream_bytes_buffered - other.stream_bytes_buffered,
        .stream_bytes_received = stream_bytes_received - other.stream_bytes_received,
    };
  }
};

// Cumulative wire-level UDP counters for a single side (ingress or egress).
struct UdpSideCounters {
  td::uint64 bytes = 0;
  td::uint64 packets = 0;
  td::uint64 syscalls = 0;

  UdpSideCounters& operator+=(const UdpSideCounters& o) {
    bytes += o.bytes;
    packets += o.packets;
    syscalls += o.syscalls;
    return *this;
  }
};

struct UdpCounters {
  UdpSideCounters ingress;
  UdpSideCounters egress;

  UdpCounters& operator+=(const UdpCounters& o) {
    ingress += o.ingress;
    egress += o.egress;
    return *this;
  }
};

enum class CongestionControlAlgo {
  Cubic,  // default
  Reno,
  Bbr,
};

struct QuicConnectionId {
  static constexpr size_t MAX_SIZE = 20;

  bool empty() const {
    return datalen_ == 0;
  }
  td::Slice as_slice() const {
    return {data_, datalen_};
  }

  static td::Result<QuicConnectionId> from_raw(const td::uint8* data, size_t datalen) {
    if (datalen == 0) {
      return QuicConnectionId{};
    }
    if (datalen > MAX_SIZE) {
      return td::Status::Error("CID too large");
    }
    QuicConnectionId cid;
    cid.datalen_ = datalen;
    cid.as_mutable_slice().copy_from(td::Slice(data, datalen));
    return cid;
  }

  static QuicConnectionId random(size_t size = MAX_SIZE) {
    CHECK(size <= MAX_SIZE);
    QuicConnectionId cid;
    cid.datalen_ = size;
    td::Random::secure_bytes(cid.as_mutable_slice());
    return cid;
  }

  std::strong_ordering operator<=>(const QuicConnectionId& other) const {
    if (auto cmp = datalen_ <=> other.datalen_; cmp != 0) {
      return cmp;
    }
    int cmp = std::memcmp(data_, other.data_, datalen_);
    if (cmp < 0) {
      return std::strong_ordering::less;
    }
    if (cmp > 0) {
      return std::strong_ordering::greater;
    }
    return std::strong_ordering::equal;
  }

  bool operator==(const QuicConnectionId& other) const {
    return as_slice() == other.as_slice();
  }

 private:
  td::MutableSlice as_mutable_slice() {
    return {data_, datalen_};
  }

  size_t datalen_{};
  uint8_t data_[MAX_SIZE]{};

  friend struct QuicConnectionIdAccess;
};

struct UdpMessageBuffer {
  td::MutableSlice storage;
  td::IPAddress address;
  size_t gso_size{0};
};

inline td::StringBuilder& operator<<(td::StringBuilder& sb, const QuicConnectionId& cid) {
  return sb << td::hex_encode(cid.as_slice());
}

inline td::StringBuilder& operator<<(td::StringBuilder& sb, CongestionControlAlgo algo) {
  switch (algo) {
    case CongestionControlAlgo::Cubic:
      return sb << "cubic";
    case CongestionControlAlgo::Reno:
      return sb << "reno";
    case CongestionControlAlgo::Bbr:
      return sb << "bbr";
  }
  return sb << "unknown";
}

}  // namespace ton::quic

namespace std {
template <>
struct hash<ton::quic::QuicConnectionId> {
  size_t operator()(const ton::quic::QuicConnectionId& cid) const noexcept {
    auto slice = cid.as_slice();
    size_t h = 0;
    for (size_t i = 0; i < slice.size(); ++i) {
      h = h * 31 + static_cast<uint8_t>(slice[i]);
    }
    return h;
  }
};
}  // namespace std
