#pragma once

#include <compare>
#include <cstdint>
#include <cstring>

#include "td/utils/Slice.h"
#include "td/utils/StringBuilder.h"
#include "td/utils/format.h"

namespace ton::quic {
using QuicStreamID = int64_t;

constexpr size_t QUIC_MAX_CIDLEN = 20;

struct QuicConnectionId {
  size_t datalen;
  uint8_t data[QUIC_MAX_CIDLEN];

  std::strong_ordering operator<=>(const QuicConnectionId& other) const {
    if (auto cmp = datalen <=> other.datalen; cmp != 0) {
      return cmp;
    }
    int cmp = std::memcmp(data, other.data, datalen);
    if (cmp < 0)
      return std::strong_ordering::less;
    if (cmp > 0)
      return std::strong_ordering::greater;
    return std::strong_ordering::equal;
  }

  bool operator==(const QuicConnectionId& other) const {
    return datalen == other.datalen && std::memcmp(data, other.data, datalen) == 0;
  }
};

struct UdpMessageBuffer {
  td::MutableSlice storage;
  td::IPAddress address;
};

inline td::StringBuilder& operator<<(td::StringBuilder& sb, const QuicConnectionId& cid) {
  return sb << td::hex_encode(td::Slice(cid.data, cid.datalen));
}

}  // namespace ton::quic

namespace std {
template <>
struct hash<ton::quic::QuicConnectionId> {
  size_t operator()(const ton::quic::QuicConnectionId& cid) const noexcept {
    size_t h = 0;
    for (size_t i = 0; i < cid.datalen; ++i) {
      h = h * 31 + cid.data[i];
    }
    return h;
  }
};
}  // namespace std
