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
  if (cnt_ > 7) {
    send_ack_at_ = {};
  } else {
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
