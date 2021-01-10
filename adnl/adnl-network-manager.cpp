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
#include "adnl-network-manager.hpp"
#include "adnl-peer-table.h"

#include "auto/tl/ton_api.hpp"

#include "td/utils/overloaded.h"

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
  add_in_addr(InDesc{port, nullptr, cat_mask}, idx);
  auto d = OutDesc{port, td::IPAddress{}, nullptr, idx};
  for (auto &it : out_desc_[priority]) {
    if (it == d) {
      it.cat_mask |= cat_mask;
      return;
    }
  }

  d.cat_mask = cat_mask;
  out_desc_[priority].push_back(std::move(d));
}

void AdnlNetworkManagerImpl::add_proxy_addr(td::IPAddress addr, td::uint16 local_port, std::shared_ptr<AdnlProxy> proxy,
                                            AdnlCategoryMask cat_mask, td::uint32 priority) {
  size_t idx = add_listening_udp_port(local_port);
  add_in_addr(InDesc{local_port, proxy, cat_mask}, idx);
  auto d = OutDesc{local_port, addr, proxy, idx};
  for (auto &it : out_desc_[priority]) {
    if (it == d) {
      it.cat_mask |= cat_mask;
      return;
    }
  }
  d.cat_mask = cat_mask;
  proxy_register(d);
  out_desc_[priority].push_back(std::move(d));
}

void AdnlNetworkManagerImpl::receive_udp_message(td::UdpMessage message, size_t idx) {
  if (!callback_) {
    LOG(ERROR) << this << ": dropping IN message [?->?]: peer table unitialized";
    return;
  }
  if (message.error.is_error()) {
    VLOG(ADNL_WARNING) << this << ": dropping ERROR message: " << message.error;
    return;
  }
  if (message.data.size() < 32) {
    VLOG(ADNL_WARNING) << this << ": received too small proxy packet of size " << message.data.size();
    return;
  }
  if (message.data.size() >= get_mtu() + 128) {
    VLOG(ADNL_NOTICE) << this << ": received huge packet of size " << message.data.size();
  }
  CHECK(idx < udp_sockets_.size());
  auto &socket = udp_sockets_[idx];
  AdnlCategoryMask cat_mask;
  bool from_proxy = false;
  if (socket.allow_proxy) {
    td::Bits256 x;
    x.as_slice().copy_from(message.data.as_slice().truncate(32));
    auto it = proxy_addrs_.find(x);
    if (it != proxy_addrs_.end()) {
      from_proxy = true;
      CHECK(it->second < in_desc_.size());
      auto &proxy_iface = in_desc_[it->second];
      CHECK(proxy_iface.is_proxy());
      auto R = in_desc_[it->second].proxy->decrypt(std::move(message.data));
      if (R.is_error()) {
        VLOG(ADNL_WARNING) << this << ": failed to decrypt proxy mesage: " << R.move_as_error();
        return;
      }
      auto packet = R.move_as_ok();
      if (packet.flags & 1) {
        message.address.init_host_port(td::IPAddress::ipv4_to_str(packet.ip), packet.port).ensure();
      } else {
        message.address = td::IPAddress{};
      }
      if ((packet.flags & 6) == 6) {
        if (proxy_iface.received.packet_is_delivered(packet.adnl_start_time, packet.seqno)) {
          VLOG(ADNL_WARNING) << this << ": dropping duplicate proxy packet";
          return;
        }
      }
      if (packet.flags & 8) {
        if (packet.date < td::Clocks::system() - 60 || packet.date > td::Clocks::system() + 60) {
          VLOG(ADNL_WARNING) << this << ": dropping proxy packet: bad time " << packet.date;
          return;
        }
      }
      if (!(packet.flags & (1 << 16))) {
        VLOG(ADNL_WARNING) << this << ": dropping proxy packet: packet has outbound flag";
        return;
      }
      if (packet.flags & (1 << 17)) {
        auto F = fetch_tl_object<ton_api::adnl_ProxyControlPacket>(std::move(packet.data), true);
        if (F.is_error()) {
          VLOG(ADNL_WARNING) << this << ": dropping proxy packet: bad control packet";
          return;
        }
        ton_api::downcast_call(*F.move_as_ok().get(),
                               td::overloaded(
                                   [&](const ton_api::adnl_proxyControlPacketPing &f) {
                                     auto &v = *proxy_iface.out_desc;
                                     auto data =
                                         create_serialize_tl_object<ton_api::adnl_proxyControlPacketPong>(f.id_);
                                     AdnlProxy::Packet p;
                                     p.flags = 6 | (1 << 17);
                                     p.ip = 0;
                                     p.port = 0;
                                     p.data = std::move(data);
                                     p.adnl_start_time = Adnl::adnl_start_time();
                                     p.seqno = ++v.out_seqno;

                                     auto enc = v.proxy->encrypt(std::move(p));

                                     td::UdpMessage M;
                                     M.address = v.proxy_addr;
                                     M.data = std::move(enc);

                                     td::actor::send_closure(socket.server, &td::UdpServer::send, std::move(M));
                                   },
                                   [&](const ton_api::adnl_proxyControlPacketPong &f) {},
                                   [&](const ton_api::adnl_proxyControlPacketRegister &f) {}));
        return;
      }
      message.data = std::move(packet.data);
      cat_mask = in_desc_[it->second].cat_mask;
    }
  }
  if (!from_proxy) {
    if (socket.in_desc == std::numeric_limits<size_t>::max()) {
      VLOG(ADNL_WARNING) << this << ": received bad packet to proxy-only listenung port";
      return;
    }
    cat_mask = in_desc_[socket.in_desc].cat_mask;
  }
  if (message.data.size() >= get_mtu()) {
    VLOG(ADNL_NOTICE) << this << ": received huge packet of size " << message.data.size();
  }
  received_messages_++;
  if (received_messages_ % 64 == 0) {
    VLOG(ADNL_DEBUG) << this << ": received " << received_messages_ << " udp messages";
  }

  VLOG(ADNL_EXTRA_DEBUG) << this << ": received message of size " << message.data.size();
  callback_->receive_packet(message.address, cat_mask, std::move(message.data));
}

void AdnlNetworkManagerImpl::send_udp_packet(AdnlNodeIdShort src_id, AdnlNodeIdShort dst_id, td::IPAddress dst_addr,
                                             td::uint32 priority, td::BufferSlice data) {
  auto it = adnl_id_2_cat_.find(src_id);
  if (it == adnl_id_2_cat_.end()) {
    VLOG(ADNL_WARNING) << this << ": dropping OUT message [" << src_id << "->" << dst_id << "]: unknown src";
    return;
  }

  auto out = choose_out_iface(it->second, priority);
  if (!out) {
    VLOG(ADNL_WARNING) << this << ": dropping OUT message [" << src_id << "->" << dst_id << "]: no out rules";
    return;
  }

  auto &v = *out;
  auto &socket = udp_sockets_[v.socket_idx];

  if (!v.is_proxy()) {
    td::UdpMessage M;
    M.address = dst_addr;
    M.data = std::move(data);

    CHECK(M.data.size() <= get_mtu());

    td::actor::send_closure(socket.server, &td::UdpServer::send, std::move(M));
  } else {
    AdnlProxy::Packet p;
    p.flags = 7;
    p.ip = dst_addr.get_ipv4();
    p.port = static_cast<td::uint16>(dst_addr.get_port());
    p.data = std::move(data);
    p.adnl_start_time = Adnl::adnl_start_time();
    p.seqno = ++v.out_seqno;

    auto enc = v.proxy->encrypt(std::move(p));

    td::UdpMessage M;
    M.address = v.proxy_addr;
    M.data = std::move(enc);

    td::actor::send_closure(socket.server, &td::UdpServer::send, std::move(M));
  }
}

void AdnlNetworkManagerImpl::proxy_register(OutDesc &desc) {
  auto data = create_serialize_tl_object<ton_api::adnl_proxyControlPacketRegister>(0, 0);
  AdnlProxy::Packet p;
  p.flags = 6 | (1 << 17);
  p.ip = 0;
  p.port = 0;
  p.data = std::move(data);
  p.adnl_start_time = Adnl::adnl_start_time();
  p.seqno = ++desc.out_seqno;

  auto enc = desc.proxy->encrypt(std::move(p));

  td::UdpMessage M;
  M.address = desc.proxy_addr;
  M.data = std::move(enc);

  auto &socket = udp_sockets_[desc.socket_idx];
  td::actor::send_closure(socket.server, &td::UdpServer::send, std::move(M));
}

void AdnlNetworkManagerImpl::alarm() {
  alarm_timestamp() = td::Timestamp::in(60.0);
  for (auto &vec : out_desc_) {
    for (auto &desc : vec.second) {
      if (desc.is_proxy()) {
        proxy_register(desc);
      }
    }
  }
}

}  // namespace adnl

}  // namespace ton
