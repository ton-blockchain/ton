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
#include "td/db/RocksDb.h"
#include "td/utils/Random.h"
#include "td/utils/crypto.h"
#include "td/utils/tl_parsers.h"
#include "td/utils/tl_storers.h"

#include "adnl-channel.h"
#include "adnl-ext-client.h"
#include "adnl-peer-table.hpp"
#include "adnl-peer.h"
#include "adnl-query.h"
#include "adnl-tunnel.h"
#include "utils.hpp"

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

void AdnlPeerTableImpl::update_id(AdnlPeerTableImpl::PeerInfo &peer_info, AdnlNodeIdFull &&peer_id) {
  if (peer_info.peer_id.empty()) {
    peer_info.peer_id = std::move(peer_id);
    for (auto &e : peer_info.peers) {
      td::actor::send_closure(e.second, &AdnlPeerPair::update_peer_id, peer_info.peer_id);
    }
  }
}

td::actor::ActorOwn<AdnlPeerPair> &AdnlPeerTableImpl::get_peer_pair(AdnlNodeIdShort peer_id,
                                                                    AdnlPeerTableImpl::PeerInfo &peer_info,
                                                                    AdnlNodeIdShort local_id,
                                                                    AdnlPeerTableImpl::LocalIdInfo &local_id_info) {
  auto it = peer_info.peers.find(local_id);
  if (it == peer_info.peers.end()) {
    it = peer_info.peers
             .emplace(local_id, AdnlPeerPair::create(network_manager_, actor_id(this), local_id_info.mode,
                                                     local_id_info.local_id.get(), dht_node_, local_id, peer_id))
             .first;
    if (!peer_info.peer_id.empty()) {
      td::actor::send_closure(it->second, &AdnlPeerPair::update_peer_id, peer_info.peer_id);
    }
  }
  return it->second;
}

void AdnlPeerTableImpl::receive_decrypted_packet(AdnlNodeIdShort dst, AdnlPacket packet, td::uint64 serialized_size) {
  packet.run_basic_checks().ensure();

  if (!packet.inited_from_short()) {
    VLOG(ADNL_INFO) << this << ": dropping IN message [?->" << dst << "]: destination not set";
    return;
  }
  AdnlNodeIdShort src = packet.from_short();

  auto it = peers_.find(src);
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

    it = peers_.try_emplace(src).first;
  }

  auto it2 = local_ids_.find(dst);
  if (it2 == local_ids_.end()) {
    VLOG(ADNL_ERROR) << this << ": dropping IN message [" << packet.from_short() << "->" << dst
                     << "]: unknown dst (but how did we decrypt message?)";
    return;
  }

  if (packet.inited_from()) {
    update_id(it->second, packet.from());
  }

  td::actor::send_closure(get_peer_pair(src, it->second, dst, it2->second), &AdnlPeerPair::receive_packet,
                          std::move(packet), serialized_size);
}

void AdnlPeerTableImpl::add_peer(AdnlNodeIdShort local_id, AdnlNodeIdFull id, AdnlAddressList addr_list) {
  auto id_short = id.compute_short_id();
  VLOG(ADNL_DEBUG) << this << ": adding peer " << id_short << " for local id " << local_id;

  auto it2 = local_ids_.find(local_id);
  CHECK(it2 != local_ids_.end());

  auto &peer_info = peers_[id_short];
  update_id(peer_info, std::move(id));
  if (!addr_list.empty()) {
    td::actor::send_closure(get_peer_pair(id_short, peer_info, local_id, it2->second), &AdnlPeerPair::update_addr_list,
                            std::move(addr_list));
  }
}

void AdnlPeerTableImpl::add_static_nodes_from_config(AdnlNodesList nodes) {
  for (auto &node : nodes.nodes()) {
    auto id_short = node.compute_short_id();
    VLOG(ADNL_INFO) << "[staticnodes] adding static node " << id_short;
    static_nodes_.emplace(id_short, std::move(node));
  }
}

void AdnlPeerTableImpl::send_message_in(AdnlNodeIdShort src, AdnlNodeIdShort dst, AdnlMessage message,
                                        td::uint32 flags) {
  auto &peer_info = peers_[dst];

  auto it2 = local_ids_.find(src);
  if (it2 == local_ids_.end()) {
    LOG(ERROR) << this << ": dropping OUT message [" << src << "->" << dst << "]: unknown src";
    return;
  }

  std::vector<OutboundAdnlMessage> messages;
  messages.push_back(OutboundAdnlMessage{std::move(message), flags});
  td::actor::send_closure(get_peer_pair(dst, peer_info, src, it2->second), &AdnlPeerPair::send_messages,
                          std::move(messages));
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
  auto &peer_info = peers_[dst];

  auto it2 = local_ids_.find(src);
  if (it2 == local_ids_.end()) {
    LOG(ERROR) << this << ": dropping OUT message [" << src << "->" << dst << "]: unknown src";
    return;
  }

  td::actor::send_closure(get_peer_pair(dst, peer_info, src, it2->second), &AdnlPeerPair::send_query, name,
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

  for (auto &e : peers_) {
    for (auto &e2 : e.second.peers) {
      td::actor::send_closure(e2.second, &AdnlPeerPair::update_dht_node, dht_node_);
    }
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
  auto it2 = it->second.peers.find(l_id);
  if (it2 == it->second.peers.end()) {
    promise.set_value("undefined");
    return;
  }
  td::actor::send_closure(it2->second, &AdnlPeerPair::get_conn_ip_str, std::move(promise));
}

void AdnlPeerTableImpl::get_stats_peer(AdnlNodeIdShort peer_id, AdnlPeerTableImpl::PeerInfo &peer_info, bool all,
                                       td::Promise<std::vector<tl_object_ptr<ton_api::adnl_stats_peerPair>>> promise) {
  class Cb : public td::actor::Actor {
   public:
    explicit Cb(td::Promise<std::vector<tl_object_ptr<ton_api::adnl_stats_peerPair>>> promise)
        : promise_(std::move(promise)) {
    }

    void got_peer_pair_stats(tl_object_ptr<ton_api::adnl_stats_peerPair> peer_pair) {
      if (peer_pair) {
        result_.push_back(std::move(peer_pair));
      }
      dec_pending();
    }

    void inc_pending() {
      ++pending_;
    }

    void dec_pending() {
      CHECK(pending_ > 0);
      --pending_;
      if (pending_ == 0) {
        promise_.set_result(std::move(result_));
        stop();
      }
    }

   private:
    td::Promise<std::vector<tl_object_ptr<ton_api::adnl_stats_peerPair>>> promise_;
    size_t pending_ = 1;
    std::vector<tl_object_ptr<ton_api::adnl_stats_peerPair>> result_;
  };
  auto callback = td::actor::create_actor<Cb>("adnlpeerstats", std::move(promise)).release();

  for (auto &[local_id, peer_pair] : peer_info.peers) {
    td::actor::send_closure(callback, &Cb::inc_pending);
    td::actor::send_closure(
        peer_pair, &AdnlPeerPair::get_stats, all,
        [local_id = local_id, peer_id = peer_id, callback](td::Result<tl_object_ptr<ton_api::adnl_stats_peerPair>> R) {
          if (R.is_error()) {
            VLOG(ADNL_NOTICE) << "failed to get stats for peer pair " << peer_id << "->" << local_id << " : "
                              << R.move_as_error();
            td::actor::send_closure(callback, &Cb::dec_pending);
          } else {
            td::actor::send_closure(callback, &Cb::got_peer_pair_stats, R.move_as_ok());
          }
        });
  }
  td::actor::send_closure(callback, &Cb::dec_pending);
}

void AdnlPeerTableImpl::get_stats(bool all, td::Promise<tl_object_ptr<ton_api::adnl_stats>> promise) {
  class Cb : public td::actor::Actor {
   public:
    explicit Cb(td::Promise<tl_object_ptr<ton_api::adnl_stats>> promise) : promise_(std::move(promise)) {
    }

    void got_local_id_stats(tl_object_ptr<ton_api::adnl_stats_localId> local_id) {
      auto &local_id_stats = local_id_stats_[local_id->short_id_];
      if (local_id_stats) {
        local_id->peers_ = std::move(local_id_stats->peers_);
      }
      local_id_stats = std::move(local_id);
      dec_pending();
    }

    void got_peer_stats(std::vector<tl_object_ptr<ton_api::adnl_stats_peerPair>> peer_pairs) {
      for (auto &peer_pair : peer_pairs) {
        auto &local_id_stats = local_id_stats_[peer_pair->local_id_];
        if (local_id_stats == nullptr) {
          local_id_stats = create_tl_object<ton_api::adnl_stats_localId>();
          local_id_stats->short_id_ = peer_pair->local_id_;
        }
        local_id_stats->peers_.push_back(std::move(peer_pair));
      }
      dec_pending();
    }

    void inc_pending() {
      ++pending_;
    }

    void dec_pending() {
      CHECK(pending_ > 0);
      --pending_;
      if (pending_ == 0) {
        auto stats = create_tl_object<ton_api::adnl_stats>();
        stats->timestamp_ = td::Clocks::system();
        for (auto &[id, local_id_stats] : local_id_stats_) {
          stats->local_ids_.push_back(std::move(local_id_stats));
        }
        promise_.set_result(std::move(stats));
        stop();
      }
    }

   private:
    td::Promise<tl_object_ptr<ton_api::adnl_stats>> promise_;
    size_t pending_ = 1;

    std::map<td::Bits256, tl_object_ptr<ton_api::adnl_stats_localId>> local_id_stats_;
  };
  auto callback = td::actor::create_actor<Cb>("adnlstats", std::move(promise)).release();

  for (auto &[id, local_id] : local_ids_) {
    td::actor::send_closure(callback, &Cb::inc_pending);
    td::actor::send_closure(local_id.local_id, &AdnlLocalId::get_stats, all,
                            [id = id, callback](td::Result<tl_object_ptr<ton_api::adnl_stats_localId>> R) {
                              if (R.is_error()) {
                                VLOG(ADNL_NOTICE)
                                    << "failed to get stats for local id " << id << " : " << R.move_as_error();
                                td::actor::send_closure(callback, &Cb::dec_pending);
                              } else {
                                td::actor::send_closure(callback, &Cb::got_local_id_stats, R.move_as_ok());
                              }
                            });
  }
  for (auto &[id, peer] : peers_) {
    td::actor::send_closure(callback, &Cb::inc_pending);
    get_stats_peer(id, peer, all,
                   [id = id, callback](td::Result<std::vector<tl_object_ptr<ton_api::adnl_stats_peerPair>>> R) {
                     if (R.is_error()) {
                       VLOG(ADNL_NOTICE) << "failed to get stats for peer " << id << " : " << R.move_as_error();
                       td::actor::send_closure(callback, &Cb::dec_pending);
                     } else {
                       td::actor::send_closure(callback, &Cb::got_peer_stats, R.move_as_ok());
                     }
                   });
  }
  td::actor::send_closure(callback, &Cb::dec_pending);
}

}  // namespace adnl

}  // namespace ton
