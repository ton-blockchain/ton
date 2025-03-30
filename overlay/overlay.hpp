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

#include <any>
#include <memory>
#include <vector>
#include <map>
#include <set>
#include <unordered_set>
#include <queue>

#include "adnl/adnl-node-id.hpp"
#include "overlay.h"
#include "overlay-manager.h"
#include "overlay-fec.hpp"
#include "overlay-broadcast.hpp"
#include "overlay-fec-broadcast.hpp"
#include "overlay-id.hpp"

#include "td/utils/DecTree.h"
#include "td/utils/List.h"
#include "td/utils/Status.h"
#include "td/utils/Time.h"
#include "td/utils/buffer.h"
#include "td/utils/overloaded.h"
#include "fec/fec.h"

#include "adnl/utils.hpp"
#include "keys/encryptor.h"

#include "auto/tl/ton_api.h"
#include "auto/tl/ton_api.hpp"
#include "td/utils/port/signals.h"
#include "tl-utils/common-utils.hpp"

namespace ton {

namespace overlay {

//using OverlayNode = tl_object_ptr<ton_api::overlay_node>;
//using OverlayNodesList = tl_object_ptr<ton_api::overlay_nodes>;

class OverlayImpl;

struct TrafficStats {
  td::uint64 out_bytes = 0;
  td::uint64 in_bytes = 0;

  td::uint32 out_packets = 0;
  td::uint32 in_packets = 0;

  void add_packet(td::uint64 size, bool in);
  void normalize(double elapsed);
  tl_object_ptr<ton_api::engine_validator_overlayStatsTraffic> tl() const;
};

class OverlayPeer {
 public:
  adnl::AdnlNodeIdShort get_id() const {
    return id_;
  }
  adnl::AdnlNodeIdFull get_full_id() const {
    return node_.adnl_id_full();
  }
  const OverlayNode *get_node() const {
    return &node_;
  }
  void update(OverlayNode node) {
    CHECK(get_id() == node.adnl_id_short());
    node_.update(std::move(node));
  }
  void update_certificate(OverlayMemberCertificate cert) {
    node_.update_certificate(std::move(cert));
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
  void on_ping_result(bool success) {
    if (success) {
      missed_pings_ = 0;
      last_receive_at_ = td::Timestamp::now();
      is_alive_ = true;
    } else {
      ++missed_pings_;
      if (missed_pings_ >= 3 && last_receive_at_.is_in_past(td::Timestamp::in(-15.0))) {
        is_alive_ = false;
      }
    }
  }
  bool is_alive() const {
    return is_alive_;
  }

  bool is_permanent_member() const {
    return is_permanent_member_;
  }

  void set_permanent(bool value) {
    is_permanent_member_ = value;
  }

  void clear_certificate() {
    node_.clear_certificate();
  }

  auto certificate() const {
    return node_.certificate();
  }

  bool has_full_id() const {
    return node_.has_full_id();
  }

  TrafficStats traffic, traffic_ctr;
  TrafficStats traffic_responses, traffic_responses_ctr;

  td::uint32 broadcast_errors = 0;
  td::uint32 fec_broadcast_errors = 0;

  td::Timestamp last_in_query_at = td::Timestamp::now();
  td::Timestamp last_out_query_at = td::Timestamp::now();

  td::string ip_addr_str = "undefined";

  td::Timestamp last_ping_at = td::Timestamp::never();
  double last_ping_time = -1.0;

 private:
  OverlayNode node_;
  adnl::AdnlNodeIdShort id_;

  bool is_neighbour_ = false;
  size_t missed_pings_ = 0;
  bool is_alive_ = true;
  bool is_permanent_member_ = false;
  td::Timestamp last_receive_at_ = td::Timestamp::now();
};

class OverlayImpl : public Overlay {
 public:
  OverlayImpl(td::actor::ActorId<keyring::Keyring> keyring, td::actor::ActorId<adnl::Adnl> adnl,
              td::actor::ActorId<OverlayManager> manager, td::actor::ActorId<dht::Dht> dht_node,
              adnl::AdnlNodeIdShort local_id, OverlayIdFull overlay_id, OverlayType overlay_type,
              std::vector<adnl::AdnlNodeIdShort> nodes, std::vector<PublicKeyHash> root_public_keys,
              OverlayMemberCertificate cert, std::unique_ptr<Overlays::Callback> callback, OverlayPrivacyRules rules,
              td::string scope = "{ \"type\": \"undefined\" }", OverlayOptions opts = {});
  void update_dht_node(td::actor::ActorId<dht::Dht> dht) override {
    dht_node_ = dht;
  }

  void receive_message(adnl::AdnlNodeIdShort src, tl_object_ptr<ton_api::overlay_messageExtra> extra,
                       td::BufferSlice data) override;
  void receive_query(adnl::AdnlNodeIdShort src, tl_object_ptr<ton_api::overlay_messageExtra> extra,
                     td::BufferSlice data, td::Promise<td::BufferSlice> promise) override;
  void send_message_to_neighbours(td::BufferSlice data) override;
  void send_broadcast(PublicKeyHash send_as, td::uint32 flags, td::BufferSlice data) override;
  void send_broadcast_fec(PublicKeyHash send_as, td::uint32 flags, td::BufferSlice data) override;
  void receive_nodes_from_db(tl_object_ptr<ton_api::overlay_nodes> nodes) override;
  void receive_nodes_from_db_v2(tl_object_ptr<ton_api::overlay_nodesV2> nodes) override;

  void get_self_node(td::Promise<OverlayNode> promise);

  void alarm() override;
  void start_up() override {
    update_throughput_at_ = td::Timestamp::in(50.0);
    last_throughput_update_ = td::Timestamp::now();

    if (overlay_type_ == OverlayType::Public) {
      update_db_at_ = td::Timestamp::in(60.0);
    }
    alarm_timestamp() = td::Timestamp::in(1);
  }

  void on_ping_result(adnl::AdnlNodeIdShort peer, bool success, double store_ping_time = -1.0);
  void receive_random_peers(adnl::AdnlNodeIdShort src, td::Result<td::BufferSlice> R, double elapsed);
  void receive_random_peers_v2(adnl::AdnlNodeIdShort src, td::Result<td::BufferSlice> R, double elapsed);
  void send_random_peers(adnl::AdnlNodeIdShort dst, td::Promise<td::BufferSlice> promise);
  void send_random_peers_v2(adnl::AdnlNodeIdShort dst, td::Promise<td::BufferSlice> promise);
  void send_random_peers_cont(adnl::AdnlNodeIdShort dst, OverlayNode node, td::Promise<td::BufferSlice> promise);
  void send_random_peers_v2_cont(adnl::AdnlNodeIdShort dst, OverlayNode node, td::Promise<td::BufferSlice> promise);
  void ping_random_peers();
  void receive_pong(adnl::AdnlNodeIdShort peer, double elapsed);
  void get_overlay_random_peers(td::uint32 max_peers, td::Promise<std::vector<adnl::AdnlNodeIdShort>> promise) override;
  void set_privacy_rules(OverlayPrivacyRules rules) override;
  void add_certificate(PublicKeyHash key, std::shared_ptr<Certificate> cert) override {
    certs_[key] = std::move(cert);
  }
  void update_member_certificate(OverlayMemberCertificate cert) override;

  void receive_dht_nodes(dht::DhtValue v);
  void dht_lookup_finished(td::Status S);
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
  BroadcastCheckResult check_source_eligible(const PublicKeyHash &source, const Certificate *cert, td::uint32 size,
                                             bool is_fec);
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
  std::vector<adnl::AdnlNodeIdShort> get_neighbours(td::uint32 max_size = 0) const;
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

  void update_throughput_out_ctr(adnl::AdnlNodeIdShort peer_id, td::uint64 msg_size, bool is_query,
                                 bool is_response) override;

  void update_throughput_in_ctr(adnl::AdnlNodeIdShort peer_id, td::uint64 msg_size, bool is_query,
                                bool is_response) override;

  void update_peer_ip_str(adnl::AdnlNodeIdShort peer_id, td::string ip_str) override;

  void update_root_member_list(std::vector<adnl::AdnlNodeIdShort> ids, std::vector<PublicKeyHash> root_public_keys,
                               OverlayMemberCertificate cert) override;

  bool is_valid_peer(const adnl::AdnlNodeIdShort &id, const ton_api::overlay_MemberCertificate *certificate);
  bool is_persistent_node(const adnl::AdnlNodeIdShort &id);

  td::uint32 max_data_bcasts() const {
    return 100;
  }
  td::uint32 max_bcasts() const {
    return 1000;
  }
  td::uint32 max_fec_bcasts() const {
    return 20;
  }
  td::uint32 max_sources() const {
    return 10;
  }
  td::uint32 max_encryptors() const {
    return 16;
  }

  td::uint32 max_neighbours() const {
    return opts_.max_neighbours_;
  }

  td::uint32 max_peers() const {
    return opts_.max_peers_;
  }

  td::uint32 nodes_to_send() const {
    return opts_.nodes_to_send_;
  }

  td::uint32 propagate_broadcast_to() const {
    return opts_.propagate_broadcast_to_;
  }

  bool has_valid_membership_certificate();
  bool has_valid_broadcast_certificate(const PublicKeyHash &source, size_t size, bool is_fec);

  void forget_peer(adnl::AdnlNodeIdShort peer_id) override {
    del_peer(peer_id);
  }

 private:
  template <class T>
  void process_query(adnl::AdnlNodeIdShort src, T &query, td::Promise<td::BufferSlice> promise) {
    callback_->receive_query(src, overlay_id_, serialize_tl_object(&query, true), std::move(promise));
  }

  void process_query(adnl::AdnlNodeIdShort src, ton_api::overlay_getRandomPeers &query,
                     td::Promise<td::BufferSlice> promise);
  void process_query(adnl::AdnlNodeIdShort src, ton_api::overlay_getRandomPeersV2 &query,
                     td::Promise<td::BufferSlice> promise);
  void process_query(adnl::AdnlNodeIdShort src, ton_api::overlay_ping &query, td::Promise<td::BufferSlice> promise);
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

  td::Status validate_peer_certificate(const adnl::AdnlNodeIdShort &node, const OverlayMemberCertificate &cert);
  td::Status validate_peer_certificate(const adnl::AdnlNodeIdShort &node, const OverlayMemberCertificate *cert);
  td::Status validate_peer_certificate(const adnl::AdnlNodeIdShort &node, ton_api::overlay_MemberCertificate *cert);
  void add_peer(OverlayNode node);
  void add_peers(std::vector<OverlayNode> nodes);
  void add_peers(const tl_object_ptr<ton_api::overlay_nodes> &nodes);
  void add_peers(const tl_object_ptr<ton_api::overlay_nodesV2> &nodes);
  void del_some_peers();
  void del_peer(const adnl::AdnlNodeIdShort &id);
  void del_from_neighbour_list(OverlayPeer *P);
  void del_from_neighbour_list(const adnl::AdnlNodeIdShort &id);
  void iterate_all_peers(std::function<void(const adnl::AdnlNodeIdShort &key, OverlayPeer &peer)> cb);
  OverlayPeer *get_random_peer(bool only_alive = false);
  bool is_root_public_key(const PublicKeyHash &key) const;
  bool has_good_peers() const;
  size_t neighbours_cnt() const;

  void finish_dht_query() {
    if (!next_dht_store_query_) {
      next_dht_store_query_ = td::Timestamp::in(td::Random::fast(60.0, 100.0));
    }
    if (frequent_dht_lookup_ && !has_good_peers()) {
      next_dht_query_ = td::Timestamp::in(td::Random::fast(6.0, 10.0));
    } else {
      next_dht_query_ = next_dht_store_query_;
    }
  }

  td::actor::ActorId<keyring::Keyring> keyring_;
  td::actor::ActorId<adnl::Adnl> adnl_;
  td::actor::ActorId<OverlayManager> manager_;
  td::actor::ActorId<dht::Dht> dht_node_;
  adnl::AdnlNodeIdShort local_id_;
  OverlayIdFull id_full_;
  OverlayIdShort overlay_id_;

  td::Timestamp next_dht_query_ = td::Timestamp::in(1.0);
  td::Timestamp next_dht_store_query_ = td::Timestamp::in(1.0);
  td::Timestamp update_db_at_;
  td::Timestamp update_throughput_at_ = td::Timestamp::now();
  td::Timestamp update_neighbours_at_ = td::Timestamp::now();
  td::Timestamp last_throughput_update_;
  td::Timestamp private_ping_peers_at_ = td::Timestamp::now();

  std::unique_ptr<Overlays::Callback> callback_;

  std::map<BroadcastHash, std::unique_ptr<BroadcastSimple>> broadcasts_;
  std::map<BroadcastHash, std::unique_ptr<BroadcastFec>> fec_broadcasts_;
  std::set<BroadcastHash> delivered_broadcasts_;

  td::ListNode bcast_data_lru_;
  td::ListNode bcast_fec_lru_;
  std::queue<BroadcastHash> bcast_lru_;

  std::map<BroadcastHash, td::actor::ActorOwn<OverlayOutboundFecBroadcast>> out_fec_bcasts_;

  void bcast_gc();

  static BroadcastHash get_broadcast_hash(adnl::AdnlNodeIdShort &src, td::Bits256 &data_hash) {
    td::uint8 buf[64];
    td::MutableSlice m{buf, 64};
    m.copy_from(src.as_slice());
    m.remove_prefix(32);
    m.copy_from(data_hash.as_slice());
    return td::sha256_bits256(td::Slice(buf, 64));
  }

  OverlayType overlay_type_;
  OverlayPrivacyRules rules_;
  td::string scope_;
  bool announce_self_ = true;
  bool frequent_dht_lookup_ = false;
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

  struct PeerList {
    struct SlaveKey {
      td::int32 expire_at{0};
      adnl::AdnlNodeIdShort node{};
    };
    using SlaveKeys = std::vector<SlaveKey>;
    std::map<PublicKeyHash, SlaveKeys> root_public_keys_;
    OverlayMemberCertificate cert_;
    std::set<adnl::AdnlNodeIdShort> bad_peers_;
    adnl::AdnlNodeIdShort next_bad_peer_ = adnl::AdnlNodeIdShort::zero();
    td::DecTree<adnl::AdnlNodeIdShort, OverlayPeer> peers_;
    std::vector<adnl::AdnlNodeIdShort> neighbours_;

    td::Timestamp local_cert_is_valid_until_;
    td::uint32 local_member_flags_{0};
  } peer_list_;
  TrafficStats total_traffic, total_traffic_ctr;
  TrafficStats total_traffic_responses, total_traffic_responses_ctr;

  OverlayOptions opts_;

  struct CachedCertificate : td::ListNode {
    CachedCertificate(PublicKeyHash source, td::Bits256 cert_hash)
      : source(source)
      , cert_hash(cert_hash) {
    }

    PublicKeyHash source;
    td::Bits256 cert_hash;
  };
  std::map<PublicKeyHash, std::unique_ptr<CachedCertificate>> checked_certificates_cache_;
  td::ListNode checked_certificates_cache_lru_;
  size_t max_checked_certificates_cache_size_ = 1000;
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
