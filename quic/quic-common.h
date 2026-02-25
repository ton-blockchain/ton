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

  QuicConnectionStats operator+(const QuicConnectionStats& other) const {
    return {
        .bytes_rx = bytes_rx + other.bytes_rx,
        .bytes_tx = bytes_tx + other.bytes_tx,
        .bytes_lost = bytes_lost + other.bytes_lost,
        .bytes_unacked = bytes_unacked + other.bytes_unacked,
        .bytes_unsent = bytes_unsent + other.bytes_unsent,
        .total_sids = total_sids + other.total_sids,
        .open_sids = open_sids + other.open_sids,
    };
  }

  QuicConnectionStats operator-(const QuicConnectionStats& other) const {
    return {
        .bytes_rx = bytes_rx - other.bytes_rx,
        .bytes_tx = bytes_tx - other.bytes_tx,
        .bytes_lost = bytes_lost - other.bytes_lost,
        .bytes_unacked = bytes_unacked - other.bytes_unacked,
        .bytes_unsent = bytes_unsent - other.bytes_unsent,
        .total_sids = total_sids - other.total_sids,
        .open_sids = open_sids - other.open_sids,
    };
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
