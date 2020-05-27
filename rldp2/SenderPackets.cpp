#include "SenderPackets.h"

#include "td/utils/bits.h"

namespace ton {
namespace rldp2 {
td::uint32 SenderPackets::next_seqno() const {
  return last_seqno_ + 1;
}

SenderPackets::DropUpdate SenderPackets::drop_packets(const Limits &limits) {
  while (!packets.empty()) {
    auto &packet = packets.front();
    if (!limits.should_drop(packet)) {
      break;
    }
    mark_ack_or_lost(packet);
    packets.pop();
  }
  DropUpdate update;
  update.new_ack = total_ack_ - last_total_ack_;
  update.new_lost = total_lost_ - last_total_lost_;
  last_total_ack_ = total_ack_;
  last_total_lost_ = total_lost_;
  update.o_loss_at = std::move(last_loss_);
  return update;
}

SenderPackets::Update SenderPackets::on_ack(Ack ack) {
  ack.max_seqno = td::min(ack.max_seqno, last_seqno_);
  ack.received_count = td::min(ack.received_count, ack.max_seqno);

  // TODO: seqno of rldp and seqno of a packet must be completly separate seqnos
  Update update;
  if (received_count_ < ack.received_count) {
    update.new_received = ack.received_count - received_count_;
    left_ack_ += update.new_received;
    left_ack_ = td::min(left_ack_, in_flight_count_);
    received_count_ = ack.received_count;
  }

  if (max_packet_.seqno > ack.max_seqno) {
    return update;
  }

  auto packet = get_packet(ack.max_seqno);
  if (!packet) {
    return update;
  }

  if (max_packet_.seqno < ack.max_seqno) {
    update.was_max_updated = true;
    max_packet_ = *packet;
  }

  for (td::uint32 i : td::BitsRange(ack.received_mask)) {
    if (ack.max_seqno < i) {
      break;
    }
    auto seqno = ack.max_seqno - i;
    auto packet = get_packet(seqno);
    if (!packet) {
      break;
    }
    mark_ack(*packet);
  }

  return update;
}
void SenderPackets::mark_ack_or_lost(Packet &packet) {
  if (left_ack_) {
    mark_ack(packet);
  } else {
    mark_lost(packet);
  }
}

void SenderPackets::mark_lost(Packet &packet) {
  if (!packet.is_in_flight) {
    return;
  }
  total_lost_++;
  in_flight_count_--;
  packet.is_in_flight = false;
  last_loss_ = packet.sent_at;
}

void SenderPackets::mark_ack(Packet &packet) {
  if (!packet.is_in_flight) {
    return;
  }
  if (left_ack_ > 0) {
    left_ack_--;
  }
  total_ack_++;
  in_flight_count_--;
  packet.is_in_flight = false;
}

SenderPackets::Packet *SenderPackets::get_packet(td::uint32 seqno) {
  if (packets.empty()) {
    return nullptr;
  }
  auto front_seqno = packets.front().seqno;
  if (front_seqno > seqno) {
    return nullptr;
  }
  td::uint32 index = seqno - front_seqno;
  if (index >= packets.size()) {
    return nullptr;
  }
  auto packet = packets.data() + index;
  CHECK(packet->seqno == seqno);
  return packet;
}

void SenderPackets::send(Packet packet) {
  CHECK(next_seqno() == packet.seqno);
  packets.push(packet);
  last_seqno_++;
  in_flight_count_ += packet.is_in_flight;
}

}  // namespace rldp2
}  // namespace ton
