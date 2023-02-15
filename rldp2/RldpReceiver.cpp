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

#include "RldpReceiver.h"

namespace ton {
namespace rldp2 {
td::Variant<RldpReceiver::ActionSendAck, RldpReceiver::ActionWait> RldpReceiver::next_action(td::Timestamp now) {
  if (send_ack_at_ && (send_ack_at_.is_in_past(now))) {
    return ActionSendAck{ack};
  }
  return ActionWait{send_ack_at_};
}

void RldpReceiver::on_ack_sent(td::Timestamp now) {
  if (cnt_ != 0) {
    //LOG(ERROR) << "RESEND ACK " << cnt_;
  }
  cnt_++;
  send_ack_at_ = {};
  if (cnt_ <= 7) {
    send_ack_at_.relax(td::Timestamp::at(now.at() + config_.ack_delay * (1 << cnt_)));
  }
}

bool RldpReceiver::on_received(td::uint32 seqno, td::Timestamp now) {
  if (!ack.on_got_packet(seqno)) {
    return false;
  }
  cnt_ = 0;
  send_ack_at_.relax(td::Timestamp::at(now.at() + config_.ack_delay));
  return true;
}

}  // namespace rldp2
}  // namespace ton
