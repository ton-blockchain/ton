#pragma once

#include <cstdint>

namespace ton::quic {
using QuicStreamID = int64_t;

struct UdpMessageBuffer {
  td::MutableSlice storage;
  td::IPAddress address;
};
}  // namespace ton::quic
