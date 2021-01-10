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
