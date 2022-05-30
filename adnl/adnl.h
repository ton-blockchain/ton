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
#include "auto/tl/ton_api.h"
#include "td/utils/port/IPAddress.h"
#include "adnl-node-id.hpp"
#include "adnl-node.h"
#include "common/errorcode.h"
#include "keyring/keyring.h"

namespace ton {

namespace dht {
class Dht;
}

namespace adnl {

enum class AdnlLocalIdMode : td::uint32 { direct_only = 1, drop_from_net = 2 };

class AdnlNetworkManager;

class AdnlExtServer : public td::actor::Actor {
 public:
  virtual void add_local_id(AdnlNodeIdShort id) = 0;
  virtual void add_tcp_port(td::uint16 port) = 0;
  virtual ~AdnlExtServer() = default;
};

class AdnlSenderInterface : public td::actor::Actor {
 public:
  virtual ~AdnlSenderInterface() = default;

  virtual void send_message(AdnlNodeIdShort src, AdnlNodeIdShort dst, td::BufferSlice data) = 0;

  virtual void send_query(AdnlNodeIdShort src, AdnlNodeIdShort dst, std::string name,
                          td::Promise<td::BufferSlice> promise, td::Timestamp timeout, td::BufferSlice data) = 0;
  virtual void send_query_ex(AdnlNodeIdShort src, AdnlNodeIdShort dst, std::string name,
                             td::Promise<td::BufferSlice> promise, td::Timestamp timeout, td::BufferSlice data,
                             td::uint64 max_answer_size) = 0;
  virtual void get_conn_ip_str(AdnlNodeIdShort l_id, AdnlNodeIdShort p_id, td::Promise<td::string> promise) = 0;
};

class AdnlTunnel : public td::actor::Actor {};

class Adnl : public AdnlSenderInterface {
 public:
  class Callback {
   public:
    virtual void receive_message(AdnlNodeIdShort src, AdnlNodeIdShort dst, td::BufferSlice data) = 0;
    virtual void receive_query(AdnlNodeIdShort src, AdnlNodeIdShort dst, td::BufferSlice data,
                               td::Promise<td::BufferSlice> promise) = 0;
    virtual ~Callback() = default;
  };

  static constexpr td::uint32 get_mtu() {
    return 1024;
  }
  static constexpr td::uint32 huge_packet_max_size() {
    return 1024 * 8;
  }

  struct SendFlags {
    enum Flags : td::uint32 { direct_only = 1 };
  };
  virtual void send_message_ex(AdnlNodeIdShort src, AdnlNodeIdShort dst, td::BufferSlice data, td::uint32 flags) = 0;

  // adds node to peer table
  // used mostly from DHT to avoid loops
  virtual void add_peer(AdnlNodeIdShort local_id, AdnlNodeIdFull id, AdnlAddressList addr_list) = 0;

  // adds address list for nodes from config
  virtual void add_static_nodes_from_config(AdnlNodesList nodes) = 0;

  // adds local id. After that you can send/receive messages from/to this id
  void add_id(AdnlNodeIdFull id, AdnlAddressList addr_list, td::uint8 cat) {
    add_id_ex(std::move(id), std::move(addr_list), cat, 0);
  }
  virtual void add_id_ex(AdnlNodeIdFull id, AdnlAddressList addr_list, td::uint8 cat, td::uint32 mode) = 0;
  virtual void del_id(AdnlNodeIdShort id, td::Promise<td::Unit> promise) = 0;

  // subscribe to (some) messages(+queries) to this local id
  virtual void subscribe(AdnlNodeIdShort dst, std::string prefix, std::unique_ptr<Callback> callback) = 0;
  virtual void unsubscribe(AdnlNodeIdShort dst, std::string prefix) = 0;

  // register (main) dht node
  // it will be used to send queries to DHT from adnl
  // there are two types of queries:
  //   - discover node addr list for unknown node
  //   - update local node information
  virtual void register_dht_node(td::actor::ActorId<dht::Dht> dht_node) = 0;
  virtual void register_network_manager(td::actor::ActorId<AdnlNetworkManager> network_manager) = 0;

  // get local id information
  // for example when you need to sent it further
  virtual void get_addr_list(AdnlNodeIdShort id, td::Promise<AdnlAddressList> promise) = 0;
  virtual void get_self_node(AdnlNodeIdShort id, td::Promise<AdnlNode> promise) = 0;

  virtual void create_ext_server(std::vector<AdnlNodeIdShort> ids, std::vector<td::uint16> ports,
                                 td::Promise<td::actor::ActorOwn<AdnlExtServer>> promise) = 0;
  virtual void create_tunnel(AdnlNodeIdShort dst, td::uint32 size,
                             td::Promise<std::pair<td::actor::ActorOwn<AdnlTunnel>, AdnlAddress>> promise) = 0;

  static td::actor::ActorOwn<Adnl> create(std::string db, td::actor::ActorId<keyring::Keyring> keyring);

  static std::string int_to_bytestring(td::int32 id) {
    return std::string(reinterpret_cast<char *>(&id), 4);
  }

  static td::int32 adnl_start_time();
};

}  // namespace adnl

using Adnl = adnl::Adnl;

}  // namespace ton
