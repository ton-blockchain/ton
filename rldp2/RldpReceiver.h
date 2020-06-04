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
