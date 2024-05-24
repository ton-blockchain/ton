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

#include "td/actor/actor.h"
#include "td/utils/BufferedUdp.h"
#include "td/net/UdpServer.h"
#include "td/net/TcpListener.h"

#include "td/actor/PromiseFuture.h"
#include "adnl-network-manager.h"
#include "adnl-received-mask.h"

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
    td::uint16 port;
    td::IPAddress proxy_addr;
    std::shared_ptr<AdnlProxy> proxy;
    size_t socket_idx;
    td::int64 out_seqno{0};
    AdnlCategoryMask cat_mask{0};

    bool is_proxy() const {
      return proxy != nullptr;
    }
    bool operator==(const OutDesc &with) const {
      if (port != with.port) {
        return false;
      }
      if (!is_proxy()) {
        return !with.is_proxy();
      }
      if (!with.is_proxy()) {
        return false;
      }
      return proxy_addr == with.proxy_addr && proxy->id() == with.proxy->id();
    }
  };
  struct InDesc {
    td::uint16 port;
    std::shared_ptr<AdnlProxy> proxy;
    AdnlCategoryMask cat_mask;
    AdnlReceivedMaskVersion received{};
    OutDesc *out_desc = nullptr;
    bool is_proxy() const {
      return proxy != nullptr;
    }
    bool operator==(const InDesc &with) const {
      if (port != with.port) {
        return false;
      }
      if (!is_proxy()) {
        return !with.is_proxy();
      }
      if (!with.is_proxy()) {
        return false;
      }
      return proxy->id() == with.proxy->id();
    }
  };
  struct UdpSocketDesc {
    UdpSocketDesc(td::uint16 port, td::actor::ActorOwn<td::UdpServer> server) : port(port), server(std::move(server)) {
    }
    td::uint16 port;
    td::actor::ActorOwn<td::UdpServer> server;
    size_t in_desc{std::numeric_limits<size_t>::max()};
    bool allow_proxy{false};
  };

  OutDesc *choose_out_iface(td::uint8 cat, td::uint32 priority);

  AdnlNetworkManagerImpl(td::uint16 out_udp_port) : out_udp_port_(out_udp_port) {
  }

  void install_callback(std::unique_ptr<Callback> callback) override {
    callback_ = std::move(callback);
  }

  void alarm() override;
  void start_up() override {
    alarm_timestamp() = td::Timestamp::in(60.0);
  }

  void add_in_addr(InDesc desc, size_t socket_idx) {
    for (size_t idx = 0; idx < in_desc_.size(); idx++) {
      if (in_desc_[idx] == desc) {
        in_desc_[idx].cat_mask |= desc.cat_mask;
        return;
      }
    }
    if (desc.is_proxy()) {
      udp_sockets_[socket_idx].allow_proxy = true;
      proxy_addrs_[desc.proxy->id()] = in_desc_.size();
    } else {
      CHECK(udp_sockets_[socket_idx].in_desc == std::numeric_limits<size_t>::max());
      udp_sockets_[socket_idx].in_desc = in_desc_.size();
    }
    in_desc_.push_back(std::move(desc));
  }

  void add_self_addr(td::IPAddress addr, AdnlCategoryMask cat_mask, td::uint32 priority) override;
  void add_proxy_addr(td::IPAddress addr, td::uint16 local_port, std::shared_ptr<AdnlProxy> proxy,
                      AdnlCategoryMask cat_mask, td::uint32 priority) override;
  void send_udp_packet(AdnlNodeIdShort src_id, AdnlNodeIdShort dst_id, td::IPAddress dst_addr, td::uint32 priority,
                       td::BufferSlice data) override;

  void set_local_id_category(AdnlNodeIdShort id, td::uint8 cat) override {
    if (cat == 255) {
      adnl_id_2_cat_.erase(id);
    } else {
      adnl_id_2_cat_[id] = cat;
    }
  }

  size_t add_listening_udp_port(td::uint16 port);
  void receive_udp_message(td::UdpMessage message, size_t idx);
  void proxy_register(OutDesc &desc);

 private:
  std::unique_ptr<Callback> callback_;

  std::map<td::uint32, std::vector<OutDesc>> out_desc_;
  std::vector<InDesc> in_desc_;
  std::map<td::Bits256, size_t> proxy_addrs_;

  td::uint64 received_messages_ = 0;
  td::uint64 sent_messages_ = 0;

  std::vector<UdpSocketDesc> udp_sockets_;
  std::map<td::uint16, size_t> port_2_socket_;

  std::map<AdnlNodeIdShort, td::uint8> adnl_id_2_cat_;

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
