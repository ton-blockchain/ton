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

#pragma once

#include "Ack.h"
#include "RldpSender.h"

namespace ton {
namespace rldp2 {
class RldpReceiver {
 public:
  RldpReceiver() = default;
  RldpReceiver(RldpSender::Config config) : config_(config) {
  }

  struct ActionSendAck {
    Ack ack;
  };

  struct ActionWait {
    td::Timestamp wait_till;
  };

  td::Variant<ActionSendAck, ActionWait> next_action(td::Timestamp now);

  bool on_received(td::uint32 seqno, td::Timestamp now);

  void on_ack_sent(td::Timestamp now);

 private:
  Ack ack;
  td::Timestamp send_ack_at_;
  td::uint32 cnt_{0};

  RldpSender::Config config_;
};
}  // namespace rldp2
}  // namespace ton
