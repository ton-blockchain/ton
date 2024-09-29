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
#include "adnl-peer-table.hpp"
#include "adnl-peer.h"
#include "adnl-channel.h"
#include "utils.hpp"

#include "td/utils/tl_storers.h"
#include "td/utils/crypto.h"
#include "td/utils/tl_parsers.h"
#include "td/utils/Random.h"
#include "td/db/RocksDb.h"

#include "utils.hpp"
#include "adnl-query.h"
#include "adnl-ext-client.h"
#include "adnl-tunnel.h"

namespace ton {

namespace adnl {

td::int32 Adnl::adnl_start_time() {
  static td::int32 start_time = [] {
    auto init_start_time = static_cast<td::int32>(td::Clocks::system());
    CHECK(init_start_time > 0);
    return init_start_time;
  }();
  return start_time;
}

td::actor::ActorOwn<Adnl> Adnl::create(std::string db, td::actor::ActorId<keyring::Keyring> keyring) {
  adnl_start_time();
  return td::actor::ActorOwn<Adnl>(td::actor::create_actor<AdnlPeerTableImpl>("PeerTable", db, keyring));
}

void AdnlPeerTableImpl::receive_packet(td::IPAddress addr, AdnlCategoryMask cat_mask, td::BufferSlice data) {
  if (data.size() < 32) {
    VLOG(ADNL_WARNING) << this << ": dropping IN message [?->?]: message too short: len=" << data.size();
    return;
  }

  AdnlNodeIdShort dst{data.as_slice().truncate(32)};
  data.confirm_read(32);

  auto it = local_ids_.find(dst);
  if (it != local_ids_.end()) {
    if (!cat_mask.test(it->second.cat)) {
      VLOG(ADNL_WARNING) << this << ": dropping IN message [?->" << dst << "]: category mismatch";
      return;
    }
    td::actor::send_closure(it->second.local_id, &AdnlLocalId::receive, addr, std::move(data));
    return;
  }

  AdnlChannelIdShort dst_chan_id{dst.pubkey_hash()};
  auto it2 = channels_.find(dst_chan_id);
  if (it2 != channels_.end()) {
    if (!cat_mask.test(it2->second.second)) {
      VLOG(ADNL_WARNING) << this << ": dropping IN message to channel [?->" << dst << "]: category mismatch";
      return;
    }
    td::actor::send_closure(it2->second.first, &AdnlChannel::receive, addr, std::move(data));
    return;
  }

  VLOG(ADNL_DEBUG) << this << ": dropping IN message [?->" << dst << "]: unknown dst " << dst
                   << " (len=" << (data.size() + 32) << ")";
}

void AdnlPeerTableImpl::receive_decrypted_packet(AdnlNodeIdShort dst, AdnlPacket packet) {
  packet.run_basic_checks().ensure();

  if (!packet.inited_from_short()) {
    VLOG(ADNL_INFO) << this << ": dropping IN message [?->" << dst << "]: destination not set";
    return;
  }

  auto it = peers_.find(packet.from_short());
  if (it == peers_.end()) {
    if (!packet.inited_from()) {
      VLOG(ADNL_NOTICE) << this << ": dropping IN message [" << packet.from_short() << "->" << dst
                        << "]: unknown peer and no full src in packet";
      return;
    }
    if (network_manager_.empty()) {
      VLOG(ADNL_NOTICE) << this << ": dropping IN message [" << packet.from_short() << "->" << dst
                        << "]: unknown peer and network manager uninitialized";
      return;
    }

    it = peers_
             .emplace(packet.from_short(),
                      AdnlPeer::create(network_manager_, actor_id(this), dht_node_, packet.from_short()))
             .first;
    CHECK(it != peers_.end());
  }

  auto it2 = local_ids_.find(dst);
  if (it2 == local_ids_.end()) {
    VLOG(ADNL_ERROR) << this << ": dropping IN message [" << packet.from_short() << "->" << dst
                     << "]: unknown dst (but how did we decrypt message?)";
    return;
  }
  td::actor::send_closure(it->second, &AdnlPeer::receive_packet, dst, it2->second.mode, it2->second.local_id.get(),
                          std::move(packet));
}

void AdnlPeerTableImpl::add_peer(AdnlNodeIdShort local_id, AdnlNodeIdFull id, AdnlAddressList addr_list) {
  auto id_short = id.compute_short_id();
  VLOG(ADNL_DEBUG) << this << ": adding peer " << id_short << " for local id " << local_id;

  auto it2 = local_ids_.find(local_id);
  CHECK(it2 != local_ids_.end());

  auto it = peers_.find(id_short);
  if (it == peers_.end()) {
    it = peers_.emplace(id_short, AdnlPeer::create(network_manager_, actor_id(this), dht_node_, id_short)).first;
    CHECK(it != peers_.end());
  }
  td::actor::send_closure(it->second, &AdnlPeer::update_id, std::move(id));
  if (!addr_list.empty()) {
    td::actor::send_closure(it->second, &AdnlPeer::update_addr_list, local_id, it2->second.mode,
                            it2->second.local_id.get(), std::move(addr_list));
  }
}

void AdnlPeerTableImpl::add_static_nodes_from_config(AdnlNodesList nodes) {
  for (auto &it : nodes.nodes()) {
    add_static_node(it);
  }
}

void AdnlPeerTableImpl::send_message_in(AdnlNodeIdShort src, AdnlNodeIdShort dst, AdnlMessage message,
                                        td::uint32 flags) {
  auto it = peers_.find(dst);

  if (it == peers_.end()) {
    it = peers_.emplace(dst, AdnlPeer::create(network_manager_, actor_id(this), dht_node_, dst)).first;
  }

  auto it2 = local_ids_.find(src);
  if (it2 == local_ids_.end()) {
    LOG(ERROR) << this << ": dropping OUT message [" << src << "->" << dst << "]: unknown src";
    return;
  }

  td::actor::send_closure(it->second, &AdnlPeer::send_one_message, src, it2->second.mode, it2->second.local_id.get(),
                          OutboundAdnlMessage{std::move(message), flags});
}

void AdnlPeerTableImpl::answer_query(AdnlNodeIdShort src, AdnlNodeIdShort dst, AdnlQueryId query_id,
                                     td::BufferSlice data) {
  if (data.size() > get_mtu()) {
    LOG(ERROR) << this << ": dropping OUT message [" << src << "->" << dst
               << "]: message too big: size=" << data.size();
    return;
  }
  send_message_in(src, dst, adnlmessage::AdnlMessageAnswer{query_id, std::move(data)}, 0);
}

void AdnlPeerTableImpl::send_query(AdnlNodeIdShort src, AdnlNodeIdShort dst, std::string name,
                                   td::Promise<td::BufferSlice> promise, td::Timestamp timeout, td::BufferSlice data) {
  if (data.size() > huge_packet_max_size()) {
    VLOG(ADNL_WARNING) << "dropping too big packet [" << src << "->" << dst << "]: size=" << data.size();
    VLOG(ADNL_WARNING) << "DUMP: " << td::buffer_to_hex(data.as_slice().truncate(128));
    return;
  }
  auto it = peers_.find(dst);

  if (it == peers_.end()) {
    it = peers_.emplace(dst, AdnlPeer::create(network_manager_, actor_id(this), dht_node_, dst)).first;
  }

  auto it2 = local_ids_.find(src);
  if (it2 == local_ids_.end()) {
    LOG(ERROR) << this << ": dropping OUT message [" << src << "->" << dst << "]: unknown src";
    return;
  }

  td::actor::send_closure(it->second, &AdnlPeer::send_query, src, it2->second.mode, it2->second.local_id.get(), name,
                          std::move(promise), timeout, std::move(data), 0);
}

void AdnlPeerTableImpl::add_id_ex(AdnlNodeIdFull id, AdnlAddressList addr_list, td::uint8 cat, td::uint32 mode) {
  auto a = id.compute_short_id();
  VLOG(ADNL_INFO) << "adnl: adding local id " << a;

  auto it = local_ids_.find(a);

  if (it != local_ids_.end()) {
    if (it->second.cat != cat) {
      it->second.cat = cat;
      if (!network_manager_.empty()) {
        td::actor::send_closure(network_manager_, &AdnlNetworkManager::set_local_id_category, a, cat);
      }
    }
    td::actor::send_closure(it->second.local_id, &AdnlLocalId::update_address_list, std::move(addr_list));
  } else {
    local_ids_.emplace(
        a, LocalIdInfo{td::actor::create_actor<AdnlLocalId>("localid", std::move(id), std::move(addr_list), mode,
                                                            actor_id(this), keyring_, dht_node_),
                       cat, mode});
    if (!network_manager_.empty()) {
      td::actor::send_closure(network_manager_, &AdnlNetworkManager::set_local_id_category, a, cat);
    }
  }
}

void AdnlPeerTableImpl::del_id(AdnlNodeIdShort id, td::Promise<td::Unit> promise) {
  VLOG(ADNL_INFO) << "adnl: deleting local id " << id;
  local_ids_.erase(id);
  promise.set_value(td::Unit());
}

void AdnlPeerTableImpl::subscribe(AdnlNodeIdShort dst, std::string prefix, std::unique_ptr<Callback> callback) {
  auto it = local_ids_.find(dst);
  LOG_CHECK(it != local_ids_.end()) << "dst=" << dst;

  td::actor::send_closure(it->second.local_id, &AdnlLocalId::subscribe, prefix, std::move(callback));
}

void AdnlPeerTableImpl::unsubscribe(AdnlNodeIdShort dst, std::string prefix) {
  auto it = local_ids_.find(dst);

  if (it != local_ids_.end()) {
    td::actor::send_closure(it->second.local_id, &AdnlLocalId::unsubscribe, prefix);
  }
}

void AdnlPeerTableImpl::register_dht_node(td::actor::ActorId<dht::Dht> dht_node) {
  dht_node_ = dht_node;

  for (auto &peer : peers_) {
    td::actor::send_closure(peer.second, &AdnlPeer::update_dht_node, dht_node_);
  }
  for (auto &local_id : local_ids_) {
    td::actor::send_closure(local_id.second.local_id, &AdnlLocalId::update_dht_node, dht_node_);
  }
}

void AdnlPeerTableImpl::register_network_manager(td::actor::ActorId<AdnlNetworkManager> network_manager) {
  network_manager_ = std::move(network_manager);

  class Cb : public AdnlNetworkManager::Callback {
   public:
    void receive_packet(td::IPAddress addr, AdnlCategoryMask cat_mask, td::BufferSlice data) override {
      td::actor::send_closure(id_, &AdnlPeerTableImpl::receive_packet, addr, std::move(cat_mask), std::move(data));
    }
    Cb(td::actor::ActorId<AdnlPeerTableImpl> id) : id_(id) {
    }

   private:
    td::actor::ActorId<AdnlPeerTableImpl> id_;
  };

  auto cb = std::make_unique<Cb>(actor_id(this));
  td::actor::send_closure(network_manager_, &AdnlNetworkManager::install_callback, std::move(cb));

  for (auto &id : local_ids_) {
    td::actor::send_closure(network_manager_, &AdnlNetworkManager::set_local_id_category, id.first, id.second.cat);
  }
}

void AdnlPeerTableImpl::get_addr_list(AdnlNodeIdShort id, td::Promise<AdnlAddressList> promise) {
  auto it = local_ids_.find(id);
  if (it == local_ids_.end()) {
    promise.set_error(td::Status::Error(ErrorCode::notready));
    return;
  }
  td::actor::send_closure(it->second.local_id, &AdnlLocalId::get_addr_list_async, std::move(promise));
}

void AdnlPeerTableImpl::get_self_node(AdnlNodeIdShort id, td::Promise<AdnlNode> promise) {
  auto it = local_ids_.find(id);
  if (it == local_ids_.end()) {
    promise.set_error(td::Status::Error(ErrorCode::notready));
    return;
  }
  td::actor::send_closure(it->second.local_id, &AdnlLocalId::get_self_node, std::move(promise));
}

void AdnlPeerTableImpl::register_channel(AdnlChannelIdShort id, AdnlNodeIdShort local_id,
                                         td::actor::ActorId<AdnlChannel> channel) {
  auto it = local_ids_.find(local_id);
  auto cat = (it != local_ids_.end()) ? it->second.cat : 255;
  auto success = channels_.emplace(id, std::make_pair(channel, cat)).second;
  CHECK(success);
}

void AdnlPeerTableImpl::unregister_channel(AdnlChannelIdShort id) {
  auto erased = channels_.erase(id);
  CHECK(erased == 1);
}

void AdnlPeerTableImpl::start_up() {
}

void AdnlPeerTableImpl::write_new_addr_list_to_db(AdnlNodeIdShort local_id, AdnlNodeIdShort peer_id, AdnlDbItem node,
                                                  td::Promise<td::Unit> promise) {
  if (db_.empty()) {
    promise.set_value(td::Unit());
    return;
  }
  td::actor::send_closure(db_, &AdnlDb::update, local_id, peer_id, std::move(node), std::move(promise));
}

void AdnlPeerTableImpl::get_addr_list_from_db(AdnlNodeIdShort local_id, AdnlNodeIdShort peer_id,
                                              td::Promise<AdnlDbItem> promise) {
  if (db_.empty()) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "db not inited"));
    return;
  }
  td::actor::send_closure(db_, &AdnlDb::get, local_id, peer_id, std::move(promise));
}

AdnlPeerTableImpl::AdnlPeerTableImpl(std::string db_root, td::actor::ActorId<keyring::Keyring> keyring) {
  keyring_ = keyring;
  static_nodes_manager_ = AdnlStaticNodesManager::create();

  if (!db_root.empty()) {
    db_ = AdnlDb::create(db_root + "/adnl");
  }
}

void AdnlPeerTableImpl::deliver(AdnlNodeIdShort src, AdnlNodeIdShort dst, td::BufferSlice data) {
  auto it = local_ids_.find(dst);
  if (it != local_ids_.end()) {
    td::actor::send_closure(it->second.local_id, &AdnlLocalId::deliver, src, std::move(data));
  }
}
void AdnlPeerTableImpl::deliver_query(AdnlNodeIdShort src, AdnlNodeIdShort dst, td::BufferSlice data,
                                      td::Promise<td::BufferSlice> promise) {
  auto it = local_ids_.find(dst);
  if (it != local_ids_.end()) {
    td::actor::send_closure(it->second.local_id, &AdnlLocalId::deliver_query, src, std::move(data), std::move(promise));
  } else {
    LOG(WARNING) << "deliver query: unknown dst " << dst;
    promise.set_error(td::Status::Error(ErrorCode::notready, "cannot deliver: unknown DST"));
  }
}

void AdnlPeerTableImpl::decrypt_message(AdnlNodeIdShort dst, td::BufferSlice data,
                                        td::Promise<td::BufferSlice> promise) {
  auto it = local_ids_.find(dst);
  if (it != local_ids_.end()) {
    td::actor::send_closure(it->second.local_id, &AdnlLocalId::decrypt_message, std::move(data), std::move(promise));
  } else {
    LOG(WARNING) << "decrypt message: unknown dst " << dst;
    promise.set_error(td::Status::Error(ErrorCode::notready, "cannot decrypt: unknown DST"));
  }
}

void AdnlPeerTableImpl::create_ext_server(std::vector<AdnlNodeIdShort> ids, std::vector<td::uint16> ports,
                                          td::Promise<td::actor::ActorOwn<AdnlExtServer>> promise) {
  promise.set_value(AdnlExtServerCreator::create(actor_id(this), std::move(ids), std::move(ports)));
}

void AdnlPeerTableImpl::create_tunnel(AdnlNodeIdShort dst, td::uint32 size,
                                      td::Promise<std::pair<td::actor::ActorOwn<AdnlTunnel>, AdnlAddress>> promise) {
}

void AdnlPeerTableImpl::get_conn_ip_str(AdnlNodeIdShort l_id, AdnlNodeIdShort p_id, td::Promise<td::string> promise) {
  auto it = peers_.find(p_id);
  if (it == peers_.end()) {
    promise.set_value("undefined");
    return;
  }
  td::actor::send_closure(it->second, &AdnlPeer::get_conn_ip_str, l_id, std::move(promise));
}

}  // namespace adnl

}  // namespace ton
