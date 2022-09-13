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

#include <map>
#include <set>

#include "adnl-peer-table.h"
#include "adnl-peer.h"
#include "keys/encryptor.h"
//#include "adnl-decryptor.h"
#include "adnl-local-id.h"
#include "adnl-query.h"
#include "utils.hpp"
#include "adnl-static-nodes.h"
#include "adnl-ext-server.h"
#include "adnl-address-list.h"

namespace ton {

namespace adnl {

class AdnlPeerTableImpl : public AdnlPeerTable {
 public:
  AdnlPeerTableImpl(std::string db_root, td::actor::ActorId<keyring::Keyring> keyring);

  void add_peer(AdnlNodeIdShort local_id, AdnlNodeIdFull id, AdnlAddressList addr_list) override;
  void add_static_nodes_from_config(AdnlNodesList nodes) override;

  void receive_packet(td::IPAddress addr, AdnlCategoryMask cat_mask, td::BufferSlice data) override;
  void receive_decrypted_packet(AdnlNodeIdShort dst, AdnlPacket data) override;
  void send_message_in(AdnlNodeIdShort src, AdnlNodeIdShort dst, AdnlMessage message, td::uint32 flags) override;
  void send_message(AdnlNodeIdShort src, AdnlNodeIdShort dst, td::BufferSlice data) override {
    send_message_ex(src, dst, std::move(data), 0);
  }
  void send_message_ex(AdnlNodeIdShort src, AdnlNodeIdShort dst, td::BufferSlice data, td::uint32 flags) override {
    if (data.size() > huge_packet_max_size()) {
      VLOG(ADNL_WARNING) << "dropping too big packet [" << src << "->" << dst << "]: size=" << data.size();
      VLOG(ADNL_WARNING) << "DUMP: " << td::buffer_to_hex(data.as_slice().truncate(128));
      return;
    }
    send_message_in(src, dst, AdnlMessage{adnlmessage::AdnlMessageCustom{std::move(data)}}, flags);
  }
  void answer_query(AdnlNodeIdShort src, AdnlNodeIdShort dst, AdnlQueryId query_id, td::BufferSlice data) override;
  void send_query(AdnlNodeIdShort src, AdnlNodeIdShort dst, std::string name, td::Promise<td::BufferSlice> promise,
                  td::Timestamp timeout, td::BufferSlice data) override;
  void send_query_ex(AdnlNodeIdShort src, AdnlNodeIdShort dst, std::string name, td::Promise<td::BufferSlice> promise,
                     td::Timestamp timeout, td::BufferSlice data, td::uint64 max_answer_size) override {
    send_query(src, dst, name, std::move(promise), timeout, std::move(data));
  }
  void add_id_ex(AdnlNodeIdFull id, AdnlAddressList addr_list, td::uint8 cat, td::uint32 mode) override;
  void del_id(AdnlNodeIdShort id, td::Promise<td::Unit> promise) override;
  void subscribe(AdnlNodeIdShort dst, std::string prefix, std::unique_ptr<Callback> callback) override;
  void unsubscribe(AdnlNodeIdShort dst, std::string prefix) override;
  void register_dht_node(td::actor::ActorId<dht::Dht> dht_node) override;
  void register_network_manager(td::actor::ActorId<AdnlNetworkManager> network_manager) override;
  void get_addr_list(AdnlNodeIdShort id, td::Promise<AdnlAddressList> promise) override;
  void get_self_node(AdnlNodeIdShort id, td::Promise<AdnlNode> promise) override;
  void start_up() override;
  void register_channel(AdnlChannelIdShort id, AdnlNodeIdShort local_id,
                        td::actor::ActorId<AdnlChannel> channel) override;
  void unregister_channel(AdnlChannelIdShort id) override;

  void write_new_addr_list_to_db(AdnlNodeIdShort local_id, AdnlNodeIdShort peer_id, AdnlDbItem node,
                                 td::Promise<td::Unit> promise) override;
  void get_addr_list_from_db(AdnlNodeIdShort local_id, AdnlNodeIdShort peer_id,
                             td::Promise<AdnlDbItem> promise) override;

  void add_static_node(AdnlNode node) override {
    CHECK(!static_nodes_manager_.empty());
    td::actor::send_closure(static_nodes_manager_, &AdnlStaticNodesManager::add_node, std::move(node));
  }
  void del_static_node(AdnlNodeIdShort id) override {
    td::actor::send_closure(static_nodes_manager_, &AdnlStaticNodesManager::del_node, id);
  }
  void get_static_node(AdnlNodeIdShort id, td::Promise<AdnlNode> promise) override {
    td::actor::send_closure(static_nodes_manager_, &AdnlStaticNodesManager::get_node, id, std::move(promise));
  }
  void deliver(AdnlNodeIdShort src, AdnlNodeIdShort dst, td::BufferSlice data) override;
  void deliver_query(AdnlNodeIdShort src, AdnlNodeIdShort dst, td::BufferSlice data,
                     td::Promise<td::BufferSlice> promise) override;
  void decrypt_message(AdnlNodeIdShort dst, td::BufferSlice data, td::Promise<td::BufferSlice> promise) override;

  void create_ext_server(std::vector<AdnlNodeIdShort> ids, std::vector<td::uint16> ports,
                         td::Promise<td::actor::ActorOwn<AdnlExtServer>> promise) override;

  void create_tunnel(AdnlNodeIdShort dst, td::uint32 size,
                     td::Promise<std::pair<td::actor::ActorOwn<AdnlTunnel>, AdnlAddress>> promise) override;
  void get_conn_ip_str(AdnlNodeIdShort l_id, AdnlNodeIdShort p_id, td::Promise<td::string> promise) override;

  struct PrintId {};
  PrintId print_id() const {
    return PrintId{};
  }

 private:
  struct LocalIdInfo {
    td::actor::ActorOwn<AdnlLocalId> local_id;
    td::uint8 cat;
    td::uint32 mode;
  };
  td::actor::ActorId<keyring::Keyring> keyring_;

  td::actor::ActorId<AdnlNetworkManager> network_manager_;
  td::actor::ActorId<dht::Dht> dht_node_;
  td::actor::ActorOwn<AdnlStaticNodesManager> static_nodes_manager_;

  void deliver_one_message(AdnlNodeIdShort src, AdnlNodeIdShort dst, AdnlMessage message);

  std::map<AdnlNodeIdShort, td::actor::ActorOwn<AdnlPeer>> peers_;
  std::map<AdnlNodeIdShort, LocalIdInfo> local_ids_;
  std::map<AdnlChannelIdShort, std::pair<td::actor::ActorId<AdnlChannel>, td::uint8>> channels_;

  td::actor::ActorOwn<AdnlDb> db_;

  td::actor::ActorOwn<AdnlExtServer> ext_server_;

  AdnlNodeIdShort proxy_addr_;
  //std::map<td::uint64, td::actor::ActorId<AdnlQuery>> out_queries_;
  //td::uint64 last_query_id_ = 1;
};

inline td::StringBuilder &operator<<(td::StringBuilder &sb, const AdnlPeerTableImpl::PrintId &id) {
  sb << "[peertable]";
  return sb;
}

inline td::StringBuilder &operator<<(td::StringBuilder &sb, const AdnlPeerTableImpl &manager) {
  sb << manager.print_id();
  return sb;
}

}  // namespace adnl

}  // namespace ton
