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

#include "Bbr.h"
#include "InboundTransfer.h"
#include "LossStats.h"
#include "OutboundTransfer.h"
#include "Pacer.h"
#include "RttStats.h"

#include "common/bitstring.h"

#include "td/utils/buffer.h"
#include "td/utils/Heap.h"
#include "td/utils/VectorQueue.h"

#include <set>

namespace ton {
namespace rldp2 {
using TransferId = td::Bits256;
class ConnectionCallback {
 public:
  virtual ~ConnectionCallback() {
  }
  virtual void send_raw(td::BufferSlice small_datagram) = 0;
  virtual void receive(TransferId transfer_id, td::Result<td::BufferSlice> r_data) = 0;
  virtual void on_sent(TransferId transfer_id, td::Result<td::Unit> state) = 0;
};

class RldpConnection {
 public:
  RldpConnection();
  RldpConnection(RldpConnection &&other) = delete;
  RldpConnection &operator=(RldpConnection &&other) = delete;
  void send(TransferId tranfer_id, td::BufferSlice data, td::Timestamp timeout = td::Timestamp::never());
  void set_receive_limits(TransferId transfer_id, td::Timestamp timeout, td::uint64 max_size);

  void receive_raw(td::BufferSlice packet);

  td::Timestamp run(ConnectionCallback &callback);

  void set_default_mtu(td::uint64 mtu) {
    default_mtu_ = mtu;
  }
  td::uint64 default_mtu() const {
    return default_mtu_;
  }

 private:
  td::uint64 default_mtu_ = 7680;

  std::map<TransferId, OutboundTransfer> outbound_transfers_;
  td::uint32 in_flight_count_{0};
  std::map<TransferId, InboundTransfer> inbound_transfers_;

  struct Limit : public td::HeapNode {
    TransferId transfer_id;
    td::uint64 max_size;
    bool is_inbound;
    bool operator<(const Limit &other) const {
      return transfer_id < other.transfer_id;
    }
  };
  td::KHeap<double> limits_heap_;
  std::set<Limit> limits_set_;

  struct CompletedId {
    TransferId transfer_id;
    td::Timestamp timeout;
  };
  td::VectorQueue<CompletedId> completed_queue_;
  std::set<TransferId> completed_set_;

  void add_limit(td::Timestamp timeout, Limit limit);
  td::Timestamp next_limit_expires_at();
  void drop_limits(TransferId id);
  void on_inbound_completed(TransferId transfer_id, td::Timestamp now);
  td::Timestamp loop_limits(td::Timestamp now);

  void loop_bbr(td::Timestamp now);

  RttStats rtt_stats_;
  BdwStats bdw_stats_;
  LossStats loss_stats_;
  Bbr bbr_;
  Pacer pacer_;
  td::uint32 congestion_window_{0};

  std::vector<td::BufferSlice> to_send_raw_;
  std::vector<std::pair<TransferId, td::Result<td::BufferSlice>>> to_receive_;
  std::vector<std::pair<TransferId, td::Result<td::Unit>>> to_on_sent_;

  void send_packet(td::BufferSlice packet) {
    to_send_raw_.push_back(std::move(packet));
  };

  td::Timestamp run(const TransferId &transfer_id, InboundTransfer &inbound);
  struct Guard {
    td::uint32 &in_flight_count;
    const RldpSender &sender;
    td::uint32 before_in_flight{sender.get_inflight_symbols_count()};

    Guard(td::uint32 &in_flight_count, const RldpSender &sender) : in_flight_count(in_flight_count), sender(sender){};
    ~Guard() {
      in_flight_count -= before_in_flight;
      in_flight_count += sender.get_inflight_symbols_count();
    }
  };

  td::optional<td::Timestamp> step(const TransferId &transfer_id, OutboundTransfer &outbound, td::Timestamp now);

  void receive_raw_obj(ton::ton_api::rldp2_messagePart &part);

  void receive_raw_obj(ton::ton_api::rldp2_complete &part);

  void receive_raw_obj(ton::ton_api::rldp2_confirm &part);
};
}  // namespace rldp2
}  // namespace ton
