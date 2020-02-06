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

namespace ton {

namespace adnl {

td::actor::ActorOwn<AdnlNetworkManager> AdnlNetworkManager::create(td::uint16 port) {
  return td::actor::create_actor<AdnlNetworkManagerImpl>("NetworkManager", port);
}

void AdnlNetworkManagerImpl::add_listening_udp_port(td::uint16 port) {
  class Callback : public td::UdpServer::Callback {
   public:
    Callback(td::actor::ActorShared<AdnlNetworkManagerImpl> manager) : manager_(std::move(manager)) {
    }

   private:
    td::actor::ActorShared<AdnlNetworkManagerImpl> manager_;
    void on_udp_message(td::UdpMessage udp_message) override {
      td::actor::send_closure_later(manager_, &AdnlNetworkManagerImpl::receive_udp_message, std::move(udp_message));
    }
  };

  auto X = td::UdpServer::create("udp server", port, std::make_unique<Callback>(actor_shared(this)));
  X.ensure();
  udp_servers_.emplace(port, X.move_as_ok());
}

void AdnlNetworkManagerImpl::receive_udp_message(td::UdpMessage message) {
  if (!callback_) {
    LOG(ERROR) << this << ": dropping IN message [?->?]: peer table unitialized";
    return;
  }
  if (message.error.is_error()) {
    VLOG(ADNL_WARNING) << this << ": dropping ERROR message: " << message.error;
    return;
  }
  if (message.data.size() >= get_mtu()) {
    VLOG(ADNL_NOTICE) << this << ": received huge packet of size " << message.data.size();
  }
  received_messages_++;
  if (received_messages_ % 64 == 0) {
    VLOG(ADNL_DEBUG) << this << ": received " << received_messages_ << " udp messages";
  }

  VLOG(ADNL_EXTRA_DEBUG) << this << ": received message of size " << message.data.size();
  callback_->receive_packet(message.address, std::move(message.data));
}

void AdnlNetworkManagerImpl::send_udp_packet(AdnlNodeIdShort src_id, AdnlNodeIdShort dst_id, td::IPAddress dst_addr,
                                             td::uint32 priority, td::BufferSlice data) {
  auto randseed = 1;  // use DST?
  while (priority > 0) {
    if (out_desc_[priority].size() > 0) {
      break;
    }
    priority--;
  }
  if (out_desc_[priority].size() == 0) {
    VLOG(ADNL_WARNING) << this << ": dropping OUT message [" << src_id << "->" << dst_id << "]: no out desc";
    return;
  }

  auto &dv = out_desc_[priority];
  auto &v = dv[randseed % dv.size()];

  if (!v.is_proxy()) {
    auto it = udp_servers_.find(static_cast<td::uint16>(v.addr.get_port()));
    CHECK(it != udp_servers_.end());

    td::UdpMessage M;
    M.address = dst_addr;
    M.data = std::move(data);

    CHECK(M.data.size() <= get_mtu());

    td::actor::send_closure(it->second, &td::UdpServer::send, std::move(M));
  } else {
    auto it = udp_servers_.find(out_udp_port_);
    CHECK(it != udp_servers_.end());

    auto enc = v.proxy->encrypt(
        AdnlProxy::Packet{dst_addr.get_ipv4(), static_cast<td::uint16>(dst_addr.get_port()), std::move(data)});

    td::UdpMessage M;
    M.address = v.addr;
    M.data = std::move(enc);

    td::actor::send_closure(it->second, &td::UdpServer::send, std::move(M));
  }
}

}  // namespace adnl

}  // namespace ton
