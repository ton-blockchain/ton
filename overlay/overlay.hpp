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

#include <vector>
#include <map>
#include <set>
#include <queue>

#include "overlay.h"
#include "overlay-manager.h"
#include "overlay-fec.hpp"
#include "overlay-broadcast.hpp"
#include "overlay-fec-broadcast.hpp"
#include "overlay-id.hpp"

#include "td/utils/DecTree.h"
#include "td/utils/List.h"
#include "td/utils/overloaded.h"
#include "fec/fec.h"

#include "adnl/utils.hpp"
#include "keys/encryptor.h"

#include "auto/tl/ton_api.h"
#include "auto/tl/ton_api.hpp"

namespace ton {

namespace overlay {

//using OverlayNode = tl_object_ptr<ton_api::overlay_node>;
//using OverlayNodesList = tl_object_ptr<ton_api::overlay_nodes>;

class OverlayImpl;

class OverlayPeer {
 public:
  adnl::AdnlNodeIdShort get_id() const {
    return id_;
  }
  adnl::AdnlNodeIdFull get_full_id() const {
    return node_.adnl_id_full();
  }
  OverlayNode get() const {
    return node_.clone();
  }
  void update(OverlayNode node) {
    CHECK(get_id() == node.adnl_id_short());
    if (node.version() > node_.version()) {
      node_ = std::move(node);
    }
  }
  OverlayPeer(OverlayNode node) : node_(std::move(node)) {
    id_ = node_.adnl_id_short();
  }
  bool is_neighbour() const {
    return is_neighbour_;
  }
  void set_neighbour(bool value) {
    is_neighbour_ = value;
  }
  td::int32 get_version() const {
    return node_.version();
  }
  
  td::uint32 throughput_out_bytes = 0;
  td::uint32 throughput_in_bytes = 0;
  
  td::uint32 throughput_out_packets = 0;
  td::uint32 throughput_in_packets = 0;
  
  td::uint32 throughput_out_bytes_ctr = 0;
  td::uint32 throughput_in_bytes_ctr = 0;
  
  td::uint32 throughput_out_packets_ctr = 0;
  td::uint32 throughput_in_packets_ctr = 0;
  
  td::uint32 broadcast_errors = 0;
  td::uint32 fec_broadcast_errors = 0;
 
  td::Timestamp last_in_query_at = td::Timestamp::now();
  td::Timestamp last_out_query_at = td::Timestamp::now();
  
  td::string ip_addr_str = "undefined";

 private:
  OverlayNode node_;
  adnl::AdnlNodeIdShort id_;

  bool is_neighbour_ = false;
};

class OverlayImpl : public Overlay {
 public:
  OverlayImpl(td::actor::ActorId<keyring::Keyring> keyring, td::actor::ActorId<adnl::Adnl> adnl,
              td::actor::ActorId<OverlayManager> manager, td::actor::ActorId<dht::Dht> dht_node,
              adnl::AdnlNodeIdShort local_id, OverlayIdFull overlay_id, bool pub,
              std::vector<adnl::AdnlNodeIdShort> nodes, std::unique_ptr<Overlays::Callback> callback,
              OverlayPrivacyRules rules, td::string scope = "{ \"type\": \"undefined\" }");
  void update_dht_node(td::actor::ActorId<dht::Dht> dht) override {
    dht_node_ = dht;
  }

  void receive_message(adnl::AdnlNodeIdShort src, td::BufferSlice data) override;
  void receive_query(adnl::AdnlNodeIdShort src, td::BufferSlice data, td::Promise<td::BufferSlice> promise) override;
  void send_message_to_neighbours(td::BufferSlice data) override;
  void send_broadcast(PublicKeyHash send_as, td::uint32 flags, td::BufferSlice data) override;
  void send_broadcast_fec(PublicKeyHash send_as, td::uint32 flags, td::BufferSlice data) override;
  void receive_nodes_from_db(tl_object_ptr<ton_api::overlay_nodes> nodes) override;

  void get_self_node(td::Promise<OverlayNode> promise);

  void alarm() override;
  void start_up() override {
    update_throughput_at_ = td::Timestamp::in(50.0);
    last_throughput_update_ = td::Timestamp::now();
    
    if (public_) {
      update_db_at_ = td::Timestamp::in(60.0);
    }
    alarm_timestamp() = td::Timestamp::in(1);
  }

  void receive_random_peers(adnl::AdnlNodeIdShort src, td::BufferSlice data);
  void send_random_peers(adnl::AdnlNodeIdShort dst, td::Promise<td::BufferSlice> promise);
  void send_random_peers_cont(adnl::AdnlNodeIdShort dst, OverlayNode node, td::Promise<td::BufferSlice> promise);
  void get_overlay_random_peers(td::uint32 max_peers, td::Promise<std::vector<adnl::AdnlNodeIdShort>> promise) override;
  void set_privacy_rules(OverlayPrivacyRules rules) override;
  void add_certificate(PublicKeyHash key, std::shared_ptr<Certificate> cert) override {
    certs_[key] = std::move(cert);
  }

  void receive_dht_nodes(td::Result<dht::DhtValue> res, bool dummy);
  void update_dht_nodes(OverlayNode node);

  void update_neighbours(td::uint32 nodes_to_change);

  void finish_fec_bcast(BroadcastHash id) {
    out_fec_bcasts_.erase(id);
  }
  struct PrintId {
    OverlayIdShort overlay_id;
    adnl::AdnlNodeIdShort local_id;
  };

  PrintId print_id() const {
    return PrintId{overlay_id_, local_id_};
  }

  void print(td::StringBuilder &sb) override;

  td::Status check_date(td::uint32 date);
  BroadcastCheckResult check_source_eligible(PublicKey source, const Certificate *cert, td::uint32 size, bool is_fec);
  td::Status check_delivered(BroadcastHash hash);

  void broadcast_checked(Overlay::BroadcastHash hash, td::Result<td::Unit> R);
  void check_broadcast(PublicKeyHash src, td::BufferSlice data, td::Promise<td::Unit> promise);

  void update_peer_err_ctr(adnl::AdnlNodeIdShort peer_id, bool is_fec);

  BroadcastFec *get_fec_broadcast(BroadcastHash hash);
  void register_fec_broadcast(std::unique_ptr<BroadcastFec> bcast);
  void register_simple_broadcast(std::unique_ptr<BroadcastSimple> bcast);
  void created_simple_broadcast(std::unique_ptr<BroadcastSimple> bcast);
  void failed_to_create_simple_broadcast(td::Status reason);
  void created_fec_broadcast(PublicKeyHash local_id, std::unique_ptr<OverlayFecBroadcastPart> bcast);
  void failed_to_create_fec_broadcast(td::Status reason);
  void deliver_broadcast(PublicKeyHash source, td::BufferSlice data);
  void send_new_fec_broadcast_part(PublicKeyHash local_id, Overlay::BroadcastDataHash data_hash, td::uint32 size,
                                   td::uint32 flags, td::BufferSlice part, td::uint32 seqno, fec::FecType fec_type,
                                   td::uint32 date);
  std::vector<adnl::AdnlNodeIdShort> get_neighbours(td::uint32 max_size = 0) const {
    if (max_size == 0 || max_size >= neighbours_.size()) {
      return neighbours_;
    } else {
      std::vector<adnl::AdnlNodeIdShort> vec;
      for (td::uint32 i = 0; i < max_size; i++) {
        vec.push_back(neighbours_[td::Random::fast(0, static_cast<td::int32>(neighbours_.size()) - 1)]);
      }
      return vec;
    }
  }
  td::actor::ActorId<OverlayManager> overlay_manager() const {
    return manager_;
  }
  td::actor::ActorId<adnl::Adnl> adnl() const {
    return adnl_;
  }
  td::actor::ActorId<keyring::Keyring> keyring() const {
    return keyring_;
  }
  adnl::AdnlNodeIdShort local_id() const {
    return local_id_;
  }
  OverlayIdShort overlay_id() const {
    return overlay_id_;
  }
  std::shared_ptr<Certificate> get_certificate(PublicKeyHash local_id);
  td::Result<Encryptor *> get_encryptor(PublicKey source);

  void get_stats(td::Promise<tl_object_ptr<ton_api::engine_validator_overlayStats>> promise) override;
  
  void update_throughput_out_ctr(adnl::AdnlNodeIdShort peer_id, td::uint32 msg_size, bool is_query) override {
    auto out_peer = peers_.get(peer_id);
    if(out_peer) {
      out_peer->throughput_out_bytes_ctr += msg_size;
      out_peer->throughput_out_packets_ctr++;
      
      if(is_query)
      {
        out_peer->last_out_query_at = td::Timestamp::now();
      }
    }
  }
  
  void update_throughput_in_ctr(adnl::AdnlNodeIdShort peer_id, td::uint32 msg_size, bool is_query) override {
    auto in_peer = peers_.get(peer_id);
    if(in_peer) {
      in_peer->throughput_in_bytes_ctr += msg_size;
      in_peer->throughput_in_packets_ctr++;
      
      if(is_query)
      {
        in_peer->last_in_query_at = td::Timestamp::now();
      }
    }
  }
  
  void update_peer_ip_str(adnl::AdnlNodeIdShort peer_id, td::string ip_str) override {
    auto fpeer = peers_.get(peer_id);
    if(fpeer) {
      fpeer->ip_addr_str = ip_str;
    }
  }

 private:
  template <class T>
  void process_query(adnl::AdnlNodeIdShort src, T &query, td::Promise<td::BufferSlice> promise) {
    callback_->receive_query(src, overlay_id_, serialize_tl_object(&query, true), std::move(promise));
  }

  void process_query(adnl::AdnlNodeIdShort src, ton_api::overlay_getRandomPeers &query,
                     td::Promise<td::BufferSlice> promise);
  void process_query(adnl::AdnlNodeIdShort src, ton_api::overlay_getBroadcast &query,
                     td::Promise<td::BufferSlice> promise);
  void process_query(adnl::AdnlNodeIdShort src, ton_api::overlay_getBroadcastList &query,
                     td::Promise<td::BufferSlice> promise);
  //void process_query(adnl::AdnlNodeIdShort src, adnl::AdnlQueryId query_id, ton_api::overlay_customQuery &query);

  td::Status process_broadcast(adnl::AdnlNodeIdShort message_from, tl_object_ptr<ton_api::overlay_broadcast> bcast);
  td::Status process_broadcast(adnl::AdnlNodeIdShort message_from, tl_object_ptr<ton_api::overlay_broadcastFec> bcast);
  td::Status process_broadcast(adnl::AdnlNodeIdShort message_from,
                               tl_object_ptr<ton_api::overlay_broadcastFecShort> bcast);
  td::Status process_broadcast(adnl::AdnlNodeIdShort message_from,
                               tl_object_ptr<ton_api::overlay_broadcastNotFound> bcast);
  td::Status process_broadcast(adnl::AdnlNodeIdShort message_from, tl_object_ptr<ton_api::overlay_fec_received> msg);
  td::Status process_broadcast(adnl::AdnlNodeIdShort message_from, tl_object_ptr<ton_api::overlay_fec_completed> msg);
  td::Status process_broadcast(adnl::AdnlNodeIdShort message_from, tl_object_ptr<ton_api::overlay_unicast> msg);

  void do_add_peer(OverlayNode node);
  void add_peer_in_cont(OverlayNode node);
  void add_peer_in(OverlayNode node);
  void add_peer(OverlayNode node);
  void add_peers(std::vector<OverlayNode> nodes);
  void del_some_peers();
  void del_peer(adnl::AdnlNodeIdShort id);
  OverlayPeer *get_random_peer();

  td::actor::ActorId<keyring::Keyring> keyring_;
  td::actor::ActorId<adnl::Adnl> adnl_;
  td::actor::ActorId<OverlayManager> manager_;
  td::actor::ActorId<dht::Dht> dht_node_;
  adnl::AdnlNodeIdShort local_id_;
  OverlayIdFull id_full_;
  OverlayIdShort overlay_id_;

  td::DecTree<adnl::AdnlNodeIdShort, OverlayPeer> peers_;
  td::Timestamp next_dht_query_ = td::Timestamp::in(1.0);
  td::Timestamp update_db_at_;
  td::Timestamp update_throughput_at_;
  td::Timestamp last_throughput_update_;

  std::unique_ptr<Overlays::Callback> callback_;

  std::map<BroadcastHash, std::unique_ptr<BroadcastSimple>> broadcasts_;
  std::map<BroadcastHash, std::unique_ptr<BroadcastFec>> fec_broadcasts_;
  std::set<BroadcastHash> delivered_broadcasts_;

  std::vector<adnl::AdnlNodeIdShort> neighbours_;
  td::ListNode bcast_data_lru_;
  td::ListNode bcast_fec_lru_;
  std::queue<BroadcastHash> bcast_lru_;

  std::map<BroadcastHash, td::actor::ActorOwn<OverlayOutboundFecBroadcast>> out_fec_bcasts_;

  void bcast_gc();

  static td::uint32 max_data_bcasts() {
    return 100;
  }
  static td::uint32 max_bcasts() {
    return 1000;
  }
  static td::uint32 max_fec_bcasts() {
    return 20;
  }
  static td::uint32 max_sources() {
    return 10;
  }
  static td::uint32 max_neighbours() {
    return 5;
  }
  static td::uint32 max_encryptors() {
    return 16;
  }

  static td::uint32 max_peers() {
    return 20;
  }

  static td::uint32 nodes_to_send() {
    return 4;
  }

  static BroadcastHash get_broadcast_hash(adnl::AdnlNodeIdShort &src, td::Bits256 &data_hash) {
    td::uint8 buf[64];
    td::MutableSlice m{buf, 64};
    m.copy_from(src.as_slice());
    m.remove_prefix(32);
    m.copy_from(data_hash.as_slice());
    return td::sha256_bits256(td::Slice(buf, 64));
  }

  bool public_;
  bool semi_public_ = false;
  OverlayPrivacyRules rules_;
  td::string scope_;
  std::map<PublicKeyHash, std::shared_ptr<Certificate>> certs_;

  class CachedEncryptor : public td::ListNode {
   public:
    Encryptor *get() {
      return encryptor_.get();
    }
    auto id() const {
      return id_;
    }
    CachedEncryptor(PublicKeyHash id, std::unique_ptr<Encryptor> encryptor)
        : id_(id), encryptor_(std::move(encryptor)) {
    }
    static CachedEncryptor *from_list_node(td::ListNode *node) {
      return static_cast<CachedEncryptor *>(node);
    }

   private:
    PublicKeyHash id_;
    std::unique_ptr<Encryptor> encryptor_;
  };

  td::ListNode encryptor_lru_;
  std::map<PublicKeyHash, std::unique_ptr<CachedEncryptor>> encryptor_map_;
};

}  // namespace overlay

}  // namespace ton

namespace td {

inline td::StringBuilder &operator<<(td::StringBuilder &sb, const ton::overlay::OverlayImpl::PrintId &id) {
  sb << "[overlay " << id.overlay_id << "@" << id.local_id << "]";
  return sb;
}

inline td::StringBuilder &operator<<(td::StringBuilder &sb, const ton::overlay::OverlayImpl &overlay) {
  sb << overlay.print_id();
  return sb;
}

inline td::StringBuilder &operator<<(td::StringBuilder &sb, const ton::overlay::OverlayImpl *overlay) {
  sb << overlay->print_id();
  return sb;
}

}  // namespace td
