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

#include "RldpSender.h"

#include "RttStats.h"
#include "LossStats.h"
#include "BdwStats.h"

#include "td/utils/misc.h"

namespace ton {
namespace rldp2 {
td::Variant<RldpSender::ActionWait, RldpSender::ActionSend> RldpSender::next_action(td::Timestamp now,
                                                                                    bool only_probe) {
  if (!only_probe && extra_symbols_ > get_inflight_symbols_count()) {
    //LOG(ERROR) << fec_helper_.symbols_count << " " << fec_helper_.get_extra_symbols_count();
    return ActionSend{packets_.next_seqno(), false};
  }

  return next_probe(now);
}

td::Variant<RldpSender::ActionWait, RldpSender::ActionSend> RldpSender::next_probe(td::Timestamp now) {
  if (probe_timeout_.is_in_past(now)) {
    return ActionSend{packets_.next_seqno(), true};
  }
  return ActionWait{probe_timeout_};
}

SenderPackets::Update RldpSender::on_ack(const Ack &ack, double ack_delay, td::Timestamp now, RttStats &rtt_stats,
                                         BdwStats &bdw_stats, LossStats &loss_stats) {
  //LOG(ERROR) << "ON ACK " << ack.max_seqno << " " << ack.received_mask << " " << ack.received_count;
  auto update = packets_.on_ack(ack);
  if (!update.was_max_updated) {
    return update;
  }

  // update rtt
  ack_delay = td::clamp(ack_delay, 0.0, config_.max_ack_delay);
  auto rtt_sample = now.at() - packets_.max_packet().sent_at.at();
  rtt_stats.on_rtt_sample(rtt_sample, ack_delay, now);

  bdw_stats.on_update(now, update.new_received);
  bdw_stats.on_packet_ack(packets_.max_packet().bdw_packet_info, packets_.max_packet().sent_at, now);

  // drop ready packets
  SenderPackets::Limits limits;
  limits.sent_at = td::Timestamp::at(now.at() - get_loss_delay(rtt_stats));
  limits.seqno = sub_or_zero(packets_.max_packet().seqno, get_loss_seqno_delay());
  update.drop_update = packets_.drop_packets(limits);

  loss_stats.on_update(update.drop_update.new_ack, update.drop_update.new_lost);

  fec_helper_.received_symbols_count = packets_.received_count();
  extra_symbols_ = loss_stats.prob.send_n(fec_helper_.get_left_fec_symbols_count());
  return update;
}

void RldpSender::on_send(td::uint32 seqno, td::Timestamp now, bool is_probe, const RttStats &rtt_stats,
                         const BdwStats &bdw_stats) {
  SenderPackets::Packet packet;
  packet.is_in_flight = true;
  packet.sent_at = now;
  packet.seqno = seqno;
  packet.size = 0;
  packet.bdw_packet_info = bdw_stats.on_packet_send(packets_.first_sent_at(now));
  packets_.send(packet);

  probe_timeout_ = td::Timestamp::at(now.at() + get_probe_delay(rtt_stats));

  if (is_probe) {
    //LOG(ERROR) << get_probe_delay(rtt_stats) << " " << rtt_stats.last_rtt << " " << packets_.in_flight_count() << " "
    //<< packets_.received_count();
    probe_k_ = std::min(probe_k_ * 2, 10u);
  } else {
    probe_k_ = 1;
  }
}

double RldpSender::get_loss_delay(const RttStats &rtt_stats) {
  auto rtt = std::max(rtt_stats.last_rtt, rtt_stats.smoothed_rtt);
  if (rtt < 0) {
    rtt = config_.initial_rtt;
  }
  return rtt * 8 / 7;
}

double RldpSender::get_probe_delay(const RttStats &rtt_stats) {
  if (rtt_stats.last_rtt < 0) {
    return config_.initial_rtt * 2;
  } else {
    return (rtt_stats.smoothed_rtt + rtt_stats.rtt_var * 4 + config_.max_ack_delay) * probe_k_;
  }
}
}  // namespace rldp2
}  // namespace ton
