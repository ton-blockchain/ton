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

#include "dht/dht.h"
#include "adnl-peer-table.h"
#include "utils.hpp"
#include "auto/tl/ton_api.h"

namespace ton {

namespace adnl {

class AdnlPeerTable;
class AdnlNetworkManager;
class AdnlLocalId;
class AdnlNetworkConnection;

class AdnlPeer;

class AdnlPeerPair : public td::actor::Actor {
 public:
  virtual void receive_packet_from_channel(AdnlChannelIdShort id, AdnlPacket packet) = 0;
  virtual void receive_packet_checked(AdnlPacket packet) = 0;
  virtual void receive_packet(AdnlPacket packet) = 0;

  virtual void send_messages(std::vector<OutboundAdnlMessage> message) = 0;
  inline void send_message(OutboundAdnlMessage message) {
    std::vector<OutboundAdnlMessage> vec;
    vec.push_back(std::move(message));
    send_messages(std::move(vec));
  }
  static constexpr td::uint32 get_mtu() {
    return Adnl::get_mtu() + 128;
  }
  virtual void send_query(std::string name, td::Promise<td::BufferSlice> promise, td::Timestamp timeout,
                          td::BufferSlice data, td::uint32 flags) = 0;
  virtual void alarm_query(AdnlQueryId query_id) = 0;
  virtual void update_dht_node(td::actor::ActorId<dht::Dht> dht_node) = 0;
  virtual void update_peer_id(AdnlNodeIdFull id) = 0;
  virtual void update_addr_list(AdnlAddressList addr_list) = 0;
  virtual void get_conn_ip_str(td::Promise<td::string> promise) = 0;

  static td::actor::ActorOwn<AdnlPeerPair> create(td::actor::ActorId<AdnlNetworkManager> network_manager,
                                                  td::actor::ActorId<AdnlPeerTable> peer_table, td::uint32 local_mode,
                                                  td::actor::ActorId<AdnlLocalId> local_actor,
                                                  td::actor::ActorId<AdnlPeer> peer_actor,
                                                  td::actor::ActorId<dht::Dht> dht_node, AdnlNodeIdShort local_id,
                                                  AdnlNodeIdShort peer_id);
};

class AdnlPeer : public td::actor::Actor {
 public:
  virtual void receive_packet(AdnlNodeIdShort dst, td::uint32 dst_mode, td::actor::ActorId<AdnlLocalId> dst_actor,
                              AdnlPacket message) = 0;
  virtual void send_messages(AdnlNodeIdShort src, td::uint32 src_mode, td::actor::ActorId<AdnlLocalId> src_actor,
                             std::vector<OutboundAdnlMessage> messages) = 0;
  virtual void send_query(AdnlNodeIdShort src, td::uint32 src_mode, td::actor::ActorId<AdnlLocalId> src_actor,
                          std::string name, td::Promise<td::BufferSlice> promise, td::Timestamp timeout,
                          td::BufferSlice data, td::uint32 flags) = 0;
  void send_one_message(AdnlNodeIdShort src, td::uint32 src_mode, td::actor::ActorId<AdnlLocalId> src_actor,
                        OutboundAdnlMessage message) {
    std::vector<OutboundAdnlMessage> vec;
    vec.push_back(std::move(message));
    send_messages(src, src_mode, src_actor, std::move(vec));
  }

  void send_message(AdnlNodeIdShort src, td::uint32 src_mode, td::actor::ActorId<AdnlLocalId> src_actor,
                    td::BufferSlice data, td::uint32 flags) {
    auto M = OutboundAdnlMessage{adnlmessage::AdnlMessageCustom{std::move(data)}, flags};
    send_one_message(src, src_mode, src_actor, std::move(M));
  }

  static td::actor::ActorOwn<AdnlPeer> create(td::actor::ActorId<AdnlNetworkManager> network_manager,
                                              td::actor::ActorId<AdnlPeerTable> peer_table,
                                              td::actor::ActorId<dht::Dht> dht_node, AdnlNodeIdShort peer_id);

  virtual void del_local_id(AdnlNodeIdShort local_id) = 0;
  virtual void update_id(AdnlNodeIdFull id) = 0;
  virtual void update_addr_list(AdnlNodeIdShort local_id, td::uint32 local_mode,
                                td::actor::ActorId<AdnlLocalId> local_actor, AdnlAddressList addr_list) = 0;
  virtual void update_dht_node(td::actor::ActorId<dht::Dht> dht_node) = 0;
  virtual void get_conn_ip_str(AdnlNodeIdShort l_id, td::Promise<td::string> promise) = 0;
};

}  // namespace adnl

}  // namespace ton
