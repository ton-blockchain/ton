#pragma once

#include "td/utils/VectorQueue.h"
#include "Ack.h"
#include "BdwStats.h"

namespace ton {
namespace rldp2 {
class SenderPackets {
 public:
  struct Packet {
    bool is_in_flight{false};
    td::Timestamp sent_at;
    td::uint32 seqno{0};
    td::uint32 size{0};

    BdwStats::PacketInfo bdw_packet_info;
  };

  struct Limits {
    td::Timestamp sent_at;
    td::uint32 seqno{0};
    bool should_drop(const Packet &packet) const {
      return !packet.is_in_flight || packet.sent_at < sent_at || packet.seqno < seqno;
    }
  };

  struct DropUpdate {
    td::uint32 new_ack{0};  // ~= new_received
    td::uint32 new_lost{0};
    td::optional<td::Timestamp> o_loss_at;
  };

  struct Update {
    bool was_max_updated{false};
    td::uint32 new_received{0};

    DropUpdate drop_update;
  };

  td::VectorQueue<Packet> packets;

  void send(Packet packet);

  td::uint32 next_seqno() const;
  DropUpdate drop_packets(const Limits &limits);

  Update on_ack(Ack ack);

  td::uint32 in_flight_count() const {
    return in_flight_count_;
  }
  td::uint32 received_count() const {
    return received_count_;
  }
  const Packet &max_packet() const {
    return max_packet_;
  }
  td::Timestamp first_sent_at(td::Timestamp now) const {
    if (!packets.empty()) {
      now.relax(packets.front().sent_at);
    }
    return now;
  }

 private:
  td::uint32 in_flight_count_{0};  // sum(packet.is_in_flight for packet in packets)
  td::uint32 received_count_{0};
  td::uint32 last_seqno_{0};
  Packet max_packet_;

  td::uint32 total_ack_{0};
  td::uint32 total_lost_{0};
  td::uint32 last_total_ack_{0};
  td::uint32 last_total_lost_{0};

  td::optional<td::Timestamp> last_loss_;
  td::uint32 left_ack_{0};

  void mark_ack_or_lost(Packet &packet);

  void mark_lost(Packet &packet);

  void mark_ack(Packet &packet);

  Packet *get_packet(td::uint32 seqno);
};
}  // namespace rldp2
}  // namespace ton
