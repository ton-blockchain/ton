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

    Copyright 2017-2019 Telegram Systems LLP
*/
#pragma once

#include "td/actor/actor.h"
#include "td/utils/BufferedUdp.h"
#include "td/net/UdpServer.h"
#include "td/net/TcpListener.h"

#include "td/actor/PromiseFuture.h"
#include "adnl-network-manager.h"

#include <map>

namespace td {
class UdpServer;
}

namespace ton {

namespace adnl {

class AdnlPeerTable;

class AdnlNetworkManagerImpl : public AdnlNetworkManager {
 public:
  struct OutDesc {
    td::IPAddress addr;
    std::shared_ptr<AdnlProxy> proxy;

    bool is_proxy() const {
      return proxy != nullptr;
    }
    bool operator==(const OutDesc &with) const {
      return addr == with.addr && is_proxy() == with.is_proxy();
    }
  };

  AdnlNetworkManagerImpl(td::uint16 out_udp_port) : out_udp_port_(out_udp_port) {
  }

  void install_callback(std::unique_ptr<Callback> callback) override {
    callback_ = std::move(callback);
  }

  void add_self_addr(td::IPAddress addr, td::uint32 priority) override {
    auto x = OutDesc{addr, nullptr};
    auto &v = out_desc_[priority];
    for (auto &y : v) {
      if (x == y) {
        return;
      }
    }
    out_desc_[priority].push_back(std::move(x));
    add_listening_udp_port(static_cast<td::uint16>(addr.get_port()));
  }
  void add_proxy_addr(td::IPAddress addr, std::shared_ptr<AdnlProxy> proxy, td::uint32 priority) override {
    auto x = OutDesc{addr, std::move(proxy)};
    auto &v = out_desc_[priority];
    for (auto &y : v) {
      if (x == y) {
        return;
      }
    }
    out_desc_[priority].push_back(std::move(x));
    if (!udp_servers_.count(out_udp_port_)) {
      add_listening_udp_port(out_udp_port_);
    }
  }
  void send_udp_packet(AdnlNodeIdShort src_id, AdnlNodeIdShort dst_id, td::IPAddress dst_addr, td::uint32 priority,
                       td::BufferSlice data) override;

  void add_listening_udp_port(td::uint16 port);
  void receive_udp_message(td::UdpMessage message);

 private:
  std::unique_ptr<Callback> callback_;

  std::map<td::uint32, std::vector<OutDesc>> out_desc_;

  td::uint64 received_messages_ = 0;
  td::uint64 sent_messages_ = 0;

  std::map<td::uint16, td::actor::ActorOwn<td::UdpServer>> udp_servers_;

  td::uint16 out_udp_port_;
};

}  // namespace adnl

}  // namespace ton

namespace td {

inline td::StringBuilder &operator<<(td::StringBuilder &sb, const ton::adnl::AdnlNetworkManagerImpl &manager) {
  sb << manager.print_id();
  return sb;
}

inline td::StringBuilder &operator<<(td::StringBuilder &sb, const ton::adnl::AdnlNetworkManagerImpl *manager) {
  sb << manager->print_id();
  return sb;
}

}  // namespace td
