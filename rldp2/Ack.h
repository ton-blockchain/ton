#pragma once

#include "td/utils/int_types.h"

namespace ton {
namespace rldp2 {
// Helper for receiver
// Also this information is sent to the sender as an acknowlegement.
struct Ack {
  td::uint32 max_seqno{0};
  td::uint32 received_mask{0};
  td::uint32 received_count{0};

  // returns true if we know that packet is new and hasn't been received yet
  bool on_got_packet(td::uint32 seqno);
};
}  // namespace rldp2
}  // namespace ton
