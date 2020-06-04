#include "Ack.h"

namespace ton {
namespace rldp2 {

bool Ack::on_got_packet(td::uint32 seqno) {
  if (seqno > max_seqno) {
    td::uint32 diff = seqno - max_seqno;
    if (diff >= 32) {
      received_mask = 0;
    } else {
      received_mask <<= diff;
    }
    max_seqno = seqno;
  }
  td::uint32 offset = max_seqno - seqno;
  if (offset < 32) {
    td::uint32 mask = 1 << offset;
    if ((received_mask & mask) == 0) {
      received_count++;
      received_mask |= mask;
      return true;
    }
  }

  return false;
}
}  // namespace rldp2
}  // namespace ton
