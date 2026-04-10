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

#include "BdwStats.h"
#include "rldp.hpp"

namespace ton {
namespace rldp2 {

BdwStats::PacketInfo BdwStats::on_packet_send(td::Timestamp first_sent_at) const {
  PacketInfo packet;
  packet.delivered_now = delivered_now;
  packet.first_sent_at = first_sent_at;
  packet.delivered_count = delivered_count;
  packet.is_paused = static_cast<bool>(paused_at_);
  return packet;
}

void BdwStats::on_packet_ack(const PacketInfo &info, td::Timestamp sent_at, td::Timestamp now) {
  // Bandwidth estimation: rate = delivered / time_interval
  // We measure deliveries that happened AFTER this packet was sent.
  // Time interval is measured on both sender and receiver sides (take max for robustness).
  //
  // sent_passed: sender-side interval
  //   From: first_sent_at (earliest in-flight packet)
  //   To: sent_at (when this packet was sent)
  //
  // ack_passed: receiver-side interval
  //   From: first delivery AFTER this packet was sent
  //   To: now (when we receive this ack)
  //
  // Problem: info.delivered_now is last delivery BEFORE send, not first AFTER.
  // Fix: use max(delivered_now, first_sent_at) as approximation for measurement start.
  auto effective_delivered_at = td::Timestamp::at(td::max(info.delivered_now.at(), info.first_sent_at.at()));
  if (paused_at_.is_in_past(effective_delivered_at)) {
    paused_at_ = {};
  }

  auto sent_passed = sent_at.at() - info.first_sent_at.at();
  auto ack_passed = now.at() - effective_delivered_at.at();
  auto passed = td::max(sent_passed, ack_passed);
  if (passed < 0.01) {
    VLOG(RLDP_INFO) << "Invalid passed " << passed;
  }
  auto delivered = delivered_count - info.delivered_count;
  on_rate_sample((double)delivered / passed, now, info.is_paused);
}

void BdwStats::on_update(td::Timestamp now, td::uint64 delivered_count_diff) {
  this->delivered_now = now;
  this->delivered_count += delivered_count_diff;
}

void BdwStats::on_pause(td::Timestamp now) {
  paused_at_ = now;
}

void BdwStats::on_rate_sample(double rate, td::Timestamp now, bool is_paused) {
  // ignore decrease of rate if is_paused == true
  if (is_paused && rate < windowed_max_bdw) {
    return;
  }
  windowed_max_bdw_stat.add_event(rate, now.at());
  auto windowed_max_bdw_sample = windowed_max_bdw_stat.get_stat(now.at()).get_stat();
  if (windowed_max_bdw_sample) {
    windowed_max_bdw = windowed_max_bdw_sample.value();
  }
}
}  // namespace rldp2
}  // namespace ton
