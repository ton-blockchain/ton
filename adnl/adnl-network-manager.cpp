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
#include "td/actor/coro_utils.h"

#include "adnl-network-manager.hpp"
#include "adnl-peer-table.h"

namespace ton {

namespace adnl {

td::actor::ActorOwn<AdnlNetworkManager> AdnlNetworkManager::create(td::uint16 port) {
  return td::actor::create_actor<AdnlNetworkManagerImpl>("NetworkManager", port);
}

AdnlNetworkManagerImpl::OutDesc *AdnlNetworkManagerImpl::choose_out_iface(td::uint8 cat, td::uint32 priority) {
  auto it = out_desc_.upper_bound(priority);
  while (true) {
    if (it == out_desc_.begin()) {
      return nullptr;
    }
    it--;

    auto &v = it->second;
    for (auto &x : v) {
      if (x.cat_mask.test(cat)) {
        return &x;
      }
    }
  }
}

size_t AdnlNetworkManagerImpl::add_listening_udp_port(td::uint16 port) {
  auto it = port_2_socket_.find(port);
  if (it != port_2_socket_.end()) {
    return it->second;
  }
  class Callback : public td::UdpServer::Callback {
   public:
    Callback(td::actor::ActorShared<AdnlNetworkManagerImpl> manager, size_t idx)
        : manager_(std::move(manager)), idx_(idx) {
    }

   private:
    td::actor::ActorShared<AdnlNetworkManagerImpl> manager_;
    size_t idx_;
    void on_udp_message(td::UdpMessage udp_message) override {
      td::actor::send_closure_later(manager_, &AdnlNetworkManagerImpl::receive_udp_message, std::move(udp_message),
                                    idx_);
    }
  };

  auto idx = udp_sockets_.size();
  auto X = td::UdpServer::create("udp server", port, std::make_unique<Callback>(actor_shared(this), idx));
  X.ensure();
  port_2_socket_[port] = idx;
  udp_sockets_.push_back(UdpSocketDesc{port, X.move_as_ok()});
  return idx;
}

void AdnlNetworkManagerImpl::add_self_addr(td::IPAddress addr, AdnlCategoryMask cat_mask, td::uint32 priority) {
  auto port = td::narrow_cast<td::uint16>(addr.get_port());
  size_t idx = add_listening_udp_port(port);
  add_in_addr(InDesc{port, cat_mask}, idx);
  auto d = OutDesc{port, idx};
  for (auto &it : out_desc_[priority]) {
    if (it == d) {
      it.cat_mask |= cat_mask;
      return;
    }
  }

  d.cat_mask = cat_mask;
  out_desc_[priority].push_back(std::move(d));
}

void AdnlNetworkManagerImpl::receive_udp_message(td::UdpMessage message, size_t idx) {
  if (!callback_) {
    metrics_.dir.at(metrics::Direction::in).dropped.inc();
    LOG(ERROR) << this << ": dropping IN message [?->?]: peer table unitialized";
    return;
  }
  if (message.error.is_error()) {
    metrics_.dir.at(metrics::Direction::in).dropped.inc();
    VLOG(adnl, WARNING) << this << ": dropping ERROR message: " << message.error;
    return;
  }
  metrics_.dir.at(metrics::Direction::in).data.record(message.data.size());
  if (message.data.size() < 32) {
    metrics_.dir.at(metrics::Direction::in).dropped.inc();
    VLOG(adnl, WARNING) << this << ": received too small packet of size " << message.data.size();
    return;
  }
  if (message.data.size() >= get_mtu() + 128) {
    VLOG(adnl, INFO) << this << ": received huge packet of size " << message.data.size();
  }
  CHECK(idx < udp_sockets_.size());
  auto &socket = udp_sockets_[idx];
  if (socket.in_desc == std::numeric_limits<size_t>::max()) {
    metrics_.dir.at(metrics::Direction::in).dropped.inc();
    VLOG(adnl, WARNING) << this << ": received packet to port without InDesc";
    return;
  }
  AdnlCategoryMask cat_mask = in_desc_[socket.in_desc].cat_mask;
  if (message.data.size() >= get_mtu()) {
    VLOG(adnl, INFO) << this << ": received huge packet of size " << message.data.size();
  }
  received_messages_++;
  if (received_messages_ % 64 == 0) {
    VLOG(adnl, DEBUG) << this << ": received " << received_messages_ << " udp messages";
  }

  VLOG(adnl, DEBUG) << this << ": received message of size " << message.data.size();
  callback_->receive_packet(message.address, cat_mask, std::move(message.data));
}

void AdnlNetworkManagerImpl::send_udp_packet(AdnlNodeIdShort src_id, AdnlNodeIdShort dst_id, td::IPAddress dst_addr,
                                             td::uint32 priority, td::BufferSlice data) {
  auto it = adnl_id_2_cat_.find(src_id);
  if (it == adnl_id_2_cat_.end()) {
    metrics_.dir.at(metrics::Direction::out).dropped.inc();
    VLOG(adnl, WARNING) << this << ": dropping OUT message [" << src_id << "->" << dst_id << "]: unknown src";
    return;
  }

  auto out = choose_out_iface(it->second, priority);
  if (!out) {
    metrics_.dir.at(metrics::Direction::out).dropped.inc();
    VLOG(adnl, WARNING) << this << ": dropping OUT message [" << src_id << "->" << dst_id << "]: no out rules";
    return;
  }

  auto &v = *out;
  auto &socket = udp_sockets_[v.socket_idx];

  td::UdpMessage M;
  M.address = dst_addr;
  M.data = std::move(data);

  CHECK(M.data.size() <= get_mtu());

  metrics_.dir.at(metrics::Direction::out).data.record(M.data.size());
  td::actor::send_closure(socket.server, &td::UdpServer::send, std::move(M));
}

td::actor::Task<> AdnlNetworkManagerImpl::collect(metrics::Context ctx) {
  // Fold the socket-level counters (syscalls, kernel receive-queue drops) into the wire tier as
  // deltas against the last scrape. Sockets are only ever appended, so indexing by position is
  // stable across the suspension.
  std::vector<td::actor::StartedTask<td::UdpWireStats>> asks;
  for (auto &socket : udp_sockets_) {
    asks.push_back(td::actor::ask(socket.server.get(), &td::UdpServer::collect));
  }
  auto stats = co_await td::actor::all_wrap(std::move(asks));
  for (size_t i = 0; i < stats.size() && i < udp_sockets_.size(); i++) {
    if (stats[i].is_error()) {
      continue;
    }
    const auto &cur = stats[i].ok();
    auto &prev = udp_sockets_[i].reflected;
    metrics_.dir.at(metrics::Direction::in).syscalls.inc(cur.in.counters.syscalls - prev.in.counters.syscalls);
    metrics_.dir.at(metrics::Direction::out).syscalls.inc(cur.out.counters.syscalls - prev.out.counters.syscalls);
    metrics_.dir.at(metrics::Direction::in).dropped.inc(cur.in.dropped - prev.in.dropped);
    prev = cur;
  }

  metrics_.listening_sockets.set(udp_sockets_.size());
  ctx.with_name("adnl").with_name("wire").collect(metrics_);
  co_return {};
}

}  // namespace adnl

}  // namespace ton
