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
#include "auto/tl/ton_api.h"
#include "td/utils/Random.h"
#include "common/delay.h"

#include "adnl/utils.hpp"
#include "dht/dht.h"

#include "overlay.hpp"
#include "auto/tl/ton_api.hpp"

#include "keys/encryptor.h"
#include "td/utils/Status.h"
#include "td/utils/StringBuilder.h"
#include "td/utils/port/signals.h"
#include <limits>

namespace ton {

namespace overlay {

const OverlayMemberCertificate OverlayNode::empty_certificate_{};

static std::string overlay_actor_name(const OverlayIdFull &overlay_id) {
  return PSTRING() << "overlay." << overlay_id.compute_short_id().bits256_value().to_hex().substr(0, 4);
}

td::actor::ActorOwn<Overlay> Overlay::create_public(td::actor::ActorId<keyring::Keyring> keyring,
                                                    td::actor::ActorId<adnl::Adnl> adnl,
                                                    td::actor::ActorId<OverlayManager> manager,
                                                    td::actor::ActorId<dht::Dht> dht_node,
                                                    adnl::AdnlNodeIdShort local_id, OverlayIdFull overlay_id,
                                                    std::unique_ptr<Overlays::Callback> callback,
                                                    OverlayPrivacyRules rules, td::string scope, OverlayOptions opts) {
  return td::actor::create_actor<OverlayImpl>(
      overlay_actor_name(overlay_id), keyring, adnl, manager, dht_node, local_id, std::move(overlay_id),
      OverlayType::Public, std::vector<adnl::AdnlNodeIdShort>(), std::vector<PublicKeyHash>(),
      OverlayMemberCertificate{}, std::move(callback), std::move(rules), std::move(scope), std::move(opts));
}

td::actor::ActorOwn<Overlay> Overlay::create_private(
    td::actor::ActorId<keyring::Keyring> keyring, td::actor::ActorId<adnl::Adnl> adnl,
    td::actor::ActorId<OverlayManager> manager, td::actor::ActorId<dht::Dht> dht_node, adnl::AdnlNodeIdShort local_id,
    OverlayIdFull overlay_id, std::vector<adnl::AdnlNodeIdShort> nodes, std::unique_ptr<Overlays::Callback> callback,
    OverlayPrivacyRules rules, std::string scope, OverlayOptions opts) {
  return td::actor::create_actor<OverlayImpl>(
      overlay_actor_name(overlay_id), keyring, adnl, manager, dht_node, local_id, std::move(overlay_id),
      OverlayType::FixedMemberList, std::move(nodes), std::vector<PublicKeyHash>(), OverlayMemberCertificate{},
      std::move(callback), std::move(rules), std::move(scope), std::move(opts));
}

td::actor::ActorOwn<Overlay> Overlay::create_semiprivate(
    td::actor::ActorId<keyring::Keyring> keyring, td::actor::ActorId<adnl::Adnl> adnl,
    td::actor::ActorId<OverlayManager> manager, td::actor::ActorId<dht::Dht> dht_node, adnl::AdnlNodeIdShort local_id,
    OverlayIdFull overlay_id, std::vector<adnl::AdnlNodeIdShort> nodes, std::vector<PublicKeyHash> root_public_keys,
    OverlayMemberCertificate cert, std::unique_ptr<Overlays::Callback> callback, OverlayPrivacyRules rules,
    std::string scope, OverlayOptions opts) {
  return td::actor::create_actor<OverlayImpl>(overlay_actor_name(overlay_id), keyring, adnl, manager, dht_node,
                                              local_id, std::move(overlay_id), OverlayType::CertificatedMembers,
                                              std::move(nodes), std::move(root_public_keys), std::move(cert),
                                              std::move(callback), std::move(rules), std::move(scope), std::move(opts));
}

OverlayImpl::OverlayImpl(td::actor::ActorId<keyring::Keyring> keyring, td::actor::ActorId<adnl::Adnl> adnl,
                         td::actor::ActorId<OverlayManager> manager, td::actor::ActorId<dht::Dht> dht_node,
                         adnl::AdnlNodeIdShort local_id, OverlayIdFull overlay_id, OverlayType overlay_type,
                         std::vector<adnl::AdnlNodeIdShort> nodes, std::vector<PublicKeyHash> root_public_keys,
                         OverlayMemberCertificate cert, std::unique_ptr<Overlays::Callback> callback,
                         OverlayPrivacyRules rules, td::string scope, OverlayOptions opts)
    : keyring_(keyring)
    , adnl_(adnl)
    , manager_(manager)
    , dht_node_(dht_node)
    , local_id_(local_id)
    , id_full_(std::move(overlay_id))
    , callback_(std::move(callback))
    , overlay_type_(overlay_type)
    , rules_(std::move(rules))
    , scope_(scope)
    , announce_self_(opts.announce_self_)
    , opts_(std::move(opts)) {
  overlay_id_ = id_full_.compute_short_id();
  frequent_dht_lookup_ = opts_.frequent_dht_lookup_;
  peer_list_.local_member_flags_ = opts_.local_overlay_member_flags_;
  opts_.broadcast_speed_multiplier_ = std::max(opts_.broadcast_speed_multiplier_, 1e-9);

  VLOG(OVERLAY_INFO) << this << ": creating";

  auto nodes_size = static_cast<td::uint32>(nodes.size());
  OverlayImpl::update_root_member_list(std::move(nodes), std::move(root_public_keys), std::move(cert));
  update_neighbours(nodes_size);
}

void OverlayImpl::process_query(adnl::AdnlNodeIdShort src, ton_api::overlay_getRandomPeers &query,
                                td::Promise<td::BufferSlice> promise) {
  if (overlay_type_ != OverlayType::FixedMemberList) {
    VLOG(OVERLAY_DEBUG) << this << ": received " << query.peers_->nodes_.size() << " nodes from " << src
                        << " in getRandomPeers query";
    add_peers(query.peers_);
    send_random_peers(src, std::move(promise));
  } else {
    VLOG(OVERLAY_WARNING) << this << ": DROPPING getRandomPeers query from " << src << " in private overlay";
    promise.set_error(td::Status::Error(ErrorCode::protoviolation, "overlay is private"));
  }
}

void OverlayImpl::process_query(adnl::AdnlNodeIdShort src, ton_api::overlay_getRandomPeersV2 &query,
                                td::Promise<td::BufferSlice> promise) {
  if (overlay_type_ != OverlayType::FixedMemberList) {
    VLOG(OVERLAY_DEBUG) << this << ": received " << query.peers_->nodes_.size() << " nodes from " << src
                        << " in getRandomPeers query";
    add_peers(query.peers_);
    send_random_peers_v2(src, std::move(promise));
  } else {
    VLOG(OVERLAY_WARNING) << this << ": DROPPING getRandomPeers query from " << src << " in private overlay";
    promise.set_error(td::Status::Error(ErrorCode::protoviolation, "overlay is private"));
  }
}

void OverlayImpl::process_query(adnl::AdnlNodeIdShort src, ton_api::overlay_ping &query,
                                td::Promise<td::BufferSlice> promise) {
  promise.set_value(create_serialize_tl_object<ton_api::overlay_pong>());
}

void OverlayImpl::process_query(adnl::AdnlNodeIdShort src, ton_api::overlay_getBroadcast &query,
                                td::Promise<td::BufferSlice> promise) {
  auto it = broadcasts_.find(query.hash_);
  if (it == broadcasts_.end()) {
    VLOG(OVERLAY_NOTICE) << this << ": received getBroadcastQuery(" << query.hash_ << ") from " << src
                         << " but broadcast is unknown";
    promise.set_value(create_serialize_tl_object<ton_api::overlay_broadcastNotFound>());
    return;
  }
  if (delivered_broadcasts_.find(query.hash_) != delivered_broadcasts_.end()) {
    VLOG(OVERLAY_DEBUG) << this << ": received getBroadcastQuery(" << query.hash_ << ") from " << src
                        << " but broadcast already deleted";
    promise.set_value(create_serialize_tl_object<ton_api::overlay_broadcastNotFound>());
    return;
  }

  VLOG(OVERLAY_DEBUG) << this << ": received getBroadcastQuery(" << query.hash_ << ") from " << src
                      << " sending broadcast";
  promise.set_value(it->second->serialize());
}

void OverlayImpl::process_query(adnl::AdnlNodeIdShort src, ton_api::overlay_getBroadcastList &query,
                                td::Promise<td::BufferSlice> promise) {
  VLOG(OVERLAY_WARNING) << this << ": DROPPING getBroadcastList query";
  promise.set_error(td::Status::Error(ErrorCode::protoviolation, "dropping get broadcast list query"));
}

/*void OverlayImpl::process_query(adnl::AdnlNodeIdShort src, adnl::AdnlQueryId query_id, ton_api::overlay_customQuery &query) {
  callback_->receive_query(src, query_id, id_, std::move(query.data_));
}
*/

void OverlayImpl::receive_query(adnl::AdnlNodeIdShort src, tl_object_ptr<ton_api::overlay_messageExtra> extra,
                                td::BufferSlice data, td::Promise<td::BufferSlice> promise) {
  if (!is_valid_peer(src, extra ? extra->certificate_.get() : nullptr)) {
    VLOG(OVERLAY_WARNING) << this << ": received query in private overlay from unknown source " << src;
    promise.set_error(td::Status::Error(ErrorCode::protoviolation, "overlay is not public"));
    return;
  }

  auto R = fetch_tl_object<ton_api::Function>(data.clone(), true);

  if (R.is_error()) {
    // allow custom query to be here
    callback_->receive_query(src, overlay_id_, std::move(data), std::move(promise));
    return;
  }

  auto Q = R.move_as_ok();

  VLOG(OVERLAY_EXTRA_DEBUG) << this << "query from " << src << ": " << ton_api::to_string(Q);

  ton_api::downcast_call(*Q.get(), [&](auto &object) { this->process_query(src, object, std::move(promise)); });
}

td::Status OverlayImpl::process_broadcast(adnl::AdnlNodeIdShort message_from,
                                          tl_object_ptr<ton_api::overlay_broadcast> bcast) {
  if (peer_list_.local_member_flags_ & OverlayMemberFlags::DoNotReceiveBroadcasts) {
    return td::Status::OK();
  }
  return BroadcastSimple::create(this, message_from, std::move(bcast));
}

td::Status OverlayImpl::process_broadcast(adnl::AdnlNodeIdShort message_from,
                                          tl_object_ptr<ton_api::overlay_broadcastFec> b) {
  if (peer_list_.local_member_flags_ & OverlayMemberFlags::DoNotReceiveBroadcasts) {
    return td::Status::OK();
  }
  return OverlayFecBroadcastPart::create(this, message_from, std::move(b));
}

td::Status OverlayImpl::process_broadcast(adnl::AdnlNodeIdShort message_from,
                                          tl_object_ptr<ton_api::overlay_broadcastFecShort> b) {
  if (peer_list_.local_member_flags_ & OverlayMemberFlags::DoNotReceiveBroadcasts) {
    return td::Status::OK();
  }
  return OverlayFecBroadcastPart::create(this, message_from, std::move(b));
}

td::Status OverlayImpl::process_broadcast(adnl::AdnlNodeIdShort message_from,
                                          tl_object_ptr<ton_api::overlay_broadcastNotFound> bcast) {
  return td::Status::Error(ErrorCode::protoviolation,
                           PSTRING() << "received strange message broadcastNotFound from " << message_from);
}

td::Status OverlayImpl::process_broadcast(adnl::AdnlNodeIdShort message_from,
                                          tl_object_ptr<ton_api::overlay_fec_received> msg) {
  return td::Status::OK();  // disable this logic for now
  auto it = fec_broadcasts_.find(msg->hash_);
  if (it != fec_broadcasts_.end()) {
    VLOG(OVERLAY_DEBUG) << this << ": received fec opt-out message from " << message_from << " for broadcast "
                        << msg->hash_;
    it->second->add_received(message_from);
  } else {
    VLOG(OVERLAY_DEBUG) << this << ": received fec opt-out message from " << message_from << " for unknown broadcast "
                        << msg->hash_;
  }
  return td::Status::OK();
}

td::Status OverlayImpl::process_broadcast(adnl::AdnlNodeIdShort message_from,
                                          tl_object_ptr<ton_api::overlay_fec_completed> msg) {
  return td::Status::OK();  // disable this logic for now
  auto it = fec_broadcasts_.find(msg->hash_);
  if (it != fec_broadcasts_.end()) {
    VLOG(OVERLAY_DEBUG) << this << ": received fec completed message from " << message_from << " for broadcast "
                        << msg->hash_;
    it->second->add_completed(message_from);
  } else {
    VLOG(OVERLAY_DEBUG) << this << ": received fec completed message from " << message_from << " for unknown broadcast "
                        << msg->hash_;
  }
  return td::Status::OK();
}

td::Status OverlayImpl::process_broadcast(adnl::AdnlNodeIdShort message_from,
                                          tl_object_ptr<ton_api::overlay_unicast> msg) {
  VLOG(OVERLAY_DEBUG) << this << ": received unicast from " << message_from;
  callback_->receive_message(message_from, overlay_id_, std::move(msg->data_));
  return td::Status::OK();
}

void OverlayImpl::receive_message(adnl::AdnlNodeIdShort src, tl_object_ptr<ton_api::overlay_messageExtra> extra,
                                  td::BufferSlice data) {
  if (!is_valid_peer(src, extra ? extra->certificate_.get() : nullptr)) {
    VLOG(OVERLAY_WARNING) << this << ": received message in private overlay from unknown source " << src;
    return;
  }

  auto X = fetch_tl_object<ton_api::overlay_Broadcast>(data.clone(), true);
  if (X.is_error()) {
    VLOG(OVERLAY_DEBUG) << this << ": received custom message";
    callback_->receive_message(src, overlay_id_, std::move(data));
    return;
  }
  auto Q = X.move_as_ok();
  ton_api::downcast_call(*Q.get(), [Self = this, &Q, &src](auto &object) {
    Self->process_broadcast(src, move_tl_object_as<std::remove_reference_t<decltype(object)>>(Q));
  });
}

void OverlayImpl::alarm() {
  bcast_gc();

  if (update_throughput_at_.is_in_past()) {
    double t_elapsed = td::Time::now() - last_throughput_update_.at();

    auto SelfId = actor_id(this);
    iterate_all_peers([&](const adnl::AdnlNodeIdShort &key, OverlayPeer &peer) {
      peer.traffic = peer.traffic_ctr;
      peer.traffic.normalize(t_elapsed);
      peer.traffic_ctr = {};
      peer.traffic_responses = peer.traffic_responses_ctr;
      peer.traffic_responses.normalize(t_elapsed);
      peer.traffic_responses_ctr = {};

      auto P = td::PromiseCreator::lambda([SelfId, peer_id = key](td::Result<td::string> result) {
        result.ensure();
        td::actor::send_closure(SelfId, &Overlay::update_peer_ip_str, peer_id, result.move_as_ok());
      });

      td::actor::send_closure(adnl_, &adnl::AdnlSenderInterface::get_conn_ip_str, local_id_, key, std::move(P));
    });
    total_traffic = total_traffic_ctr;
    total_traffic.normalize(t_elapsed);
    total_traffic_ctr = {};
    total_traffic_responses = total_traffic_responses_ctr;
    total_traffic_responses.normalize(t_elapsed);
    total_traffic_responses_ctr = {};

    update_throughput_at_ = td::Timestamp::in(50.0);
    last_throughput_update_ = td::Timestamp::now();
  }

  if (overlay_type_ != OverlayType::FixedMemberList) {
    if (has_valid_membership_certificate()) {
      auto P = get_random_peer();
      if (P) {
        if (overlay_type_ == OverlayType::Public) {
          send_random_peers(P->get_id(), {});
        } else {
          send_random_peers_v2(P->get_id(), {});
        }
      }
    } else {
      VLOG(OVERLAY_WARNING) << "member certificate ist invalid, valid_until="
                            << peer_list_.local_cert_is_valid_until_.at_unix();
    }
    if (next_dht_query_ && next_dht_query_.is_in_past() && overlay_type_ == OverlayType::Public) {
      next_dht_query_ = td::Timestamp::never();
      std::function<void(dht::DhtValue)> callback = [SelfId = actor_id(this)](dht::DhtValue value) {
        td::actor::send_closure(SelfId, &OverlayImpl::receive_dht_nodes, std::move(value));
      };
      td::Promise<td::Unit> on_finish = [SelfId = actor_id(this)](td::Result<td::Unit> R) {
        td::actor::send_closure(SelfId, &OverlayImpl::dht_lookup_finished, R.move_as_status());
      };
      td::actor::send_closure(dht_node_, &dht::Dht::get_value_many, dht::DhtKey{overlay_id_.pubkey_hash(), "nodes", 0},
                              std::move(callback), std::move(on_finish));
    }
    if (update_db_at_.is_in_past() && overlay_type_ == OverlayType::Public) {
      std::vector<OverlayNode> vec;
      for (td::uint32 i = 0; i < 20; i++) {
        auto P = get_random_peer();
        if (!P) {
          break;
        }
        vec.push_back(P->get_node()->clone());
      }
      if (vec.size() > 0) {
        td::actor::send_closure(manager_, &OverlayManager::save_to_db, local_id_, overlay_id_, std::move(vec));
      }
      update_db_at_ = td::Timestamp::in(60.0);
    }

    if (update_neighbours_at_.is_in_past()) {
      update_neighbours(2);
      update_neighbours_at_ = td::Timestamp::in(td::Random::fast(30.0, 120.0));
    } else {
      update_neighbours(0);
    }
    alarm_timestamp() = td::Timestamp::in(1.0);
  } else {
    if (update_neighbours_at_.is_in_past()) {
      update_neighbours(0);
      update_neighbours_at_ = td::Timestamp::in(60.0 + td::Random::fast(0, 100) * 0.6);
    }
    if (opts_.private_ping_peers_) {
      if (private_ping_peers_at_.is_in_past()) {
        ping_random_peers();
        private_ping_peers_at_ = td::Timestamp::in(td::Random::fast(30.0, 50.0));
      }
      alarm_timestamp().relax(private_ping_peers_at_);
    }
    alarm_timestamp().relax(update_neighbours_at_);
    alarm_timestamp().relax(update_throughput_at_);
  }
}

void OverlayImpl::receive_dht_nodes(dht::DhtValue v) {
  CHECK(overlay_type_ == OverlayType::Public);
  auto R = fetch_tl_object<ton_api::overlay_nodes>(v.value().clone(), true);
  if (R.is_ok()) {
    auto r = R.move_as_ok();
    VLOG(OVERLAY_INFO) << this << ": received " << r->nodes_.size() << " nodes from overlay";
    VLOG(OVERLAY_EXTRA_DEBUG) << this << ": nodes: " << ton_api::to_string(r);
    std::vector<OverlayNode> nodes;
    for (auto &n : r->nodes_) {
      auto N = OverlayNode::create(n);
      if (N.is_ok()) {
        nodes.emplace_back(N.move_as_ok());
      }
    }
    add_peers(std::move(nodes));
  } else {
    VLOG(OVERLAY_WARNING) << this << ": incorrect value in DHT for overlay nodes: " << R.move_as_error();
  }
}

void OverlayImpl::dht_lookup_finished(td::Status S) {
  if (S.is_error()) {
    VLOG(OVERLAY_NOTICE) << this << ": can not get value from DHT: " << S;
  }
  if (!(next_dht_store_query_ && next_dht_store_query_.is_in_past())) {
    finish_dht_query();
    return;
  }
  next_dht_store_query_ = td::Timestamp::never();
  if (!announce_self_) {
    finish_dht_query();
    return;
  }

  VLOG(OVERLAY_INFO) << this << ": adding self node to DHT overlay's nodes";
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), oid = print_id()](td::Result<OverlayNode> R) {
    if (R.is_error()) {
      LOG(ERROR) << oid << "cannot get self node";
      td::actor::send_closure(SelfId, &OverlayImpl::finish_dht_query);
      return;
    }
    td::actor::send_closure(SelfId, &OverlayImpl::update_dht_nodes, R.move_as_ok());
  });
  get_self_node(std::move(P));
}

void OverlayImpl::update_dht_nodes(OverlayNode node) {
  if (overlay_type_ != OverlayType::Public) {
    return;
  }

  auto nodes = create_tl_object<ton_api::overlay_nodes>(std::vector<tl_object_ptr<ton_api::overlay_node>>());
  nodes->nodes_.emplace_back(node.tl());

  dht::DhtKey dht_key{overlay_id_.pubkey_hash(), "nodes", 0};
  auto update_rule = dht::DhtUpdateRuleOverlayNodes::create();
  dht::DhtKeyDescription dht_key_descr(std::move(dht_key), id_full_.pubkey(), update_rule.move_as_ok(),
                                       td::BufferSlice());
  dht_key_descr.check().ensure();
  dht::DhtValue value{std::move(dht_key_descr), serialize_tl_object(nodes, true),
                      static_cast<td::uint32>(td::Clocks::system() + 3600), td::BufferSlice()};
  value.check().ensure();

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), oid = print_id()](td::Result<td::Unit> res) {
    if (res.is_error()) {
      VLOG(OVERLAY_NOTICE) << oid << ": error storing to DHT: " << res.move_as_error();
    }
    td::actor::send_closure(SelfId, &OverlayImpl::finish_dht_query);
  });

  td::actor::send_closure(dht_node_, &dht::Dht::set_value, std::move(value), std::move(P));
}

void OverlayImpl::bcast_gc() {
  while (broadcasts_.size() > max_data_bcasts()) {
    auto bcast = BroadcastSimple::from_list_node(bcast_data_lru_.get());
    CHECK(bcast);
    auto hash = bcast->get_hash();
    broadcasts_.erase(hash);
    if (delivered_broadcasts_.insert(hash).second) {
      bcast_lru_.push(hash);
    }
  }
  while (fec_broadcasts_.size() > 0) {
    auto bcast = BroadcastFec::from_list_node(bcast_fec_lru_.prev);
    CHECK(bcast);
    if (bcast->get_date() > td::Clocks::system() - 60) {
      break;
    }
    auto hash = bcast->get_hash();
    CHECK(fec_broadcasts_.count(hash) == 1);
    fec_broadcasts_.erase(hash);
    if (delivered_broadcasts_.insert(hash).second) {
      bcast_lru_.push(hash);
    }
  }
  while (bcast_lru_.size() > max_bcasts()) {
    auto Id = bcast_lru_.front();
    bcast_lru_.pop();
    CHECK(delivered_broadcasts_.erase(Id));
  }
  CHECK(delivered_broadcasts_.size() == bcast_lru_.size());
}

void OverlayImpl::send_broadcast(PublicKeyHash send_as, td::uint32 flags, td::BufferSlice data) {
  if (!has_valid_membership_certificate()) {
    VLOG(OVERLAY_WARNING) << "member certificate is invalid, valid_until="
                          << peer_list_.local_cert_is_valid_until_.at_unix();
    return;
  }
  if (!has_valid_broadcast_certificate(send_as, data.size(), false)) {
    VLOG(OVERLAY_WARNING) << "broadcast source certificate is invalid";
    return;
  }
  auto S = BroadcastSimple::create_new(actor_id(this), keyring_, send_as, std::move(data), flags);
  if (S.is_error()) {
    LOG(WARNING) << "failed to send broadcast: " << S;
  }
}

void OverlayImpl::send_broadcast_fec(PublicKeyHash send_as, td::uint32 flags, td::BufferSlice data) {
  if (!has_valid_membership_certificate()) {
    VLOG(OVERLAY_WARNING) << "member certificate is invalid, valid_until="
                          << peer_list_.local_cert_is_valid_until_.at_unix();
    return;
  }
  if (!has_valid_broadcast_certificate(send_as, data.size(), true)) {
    VLOG(OVERLAY_WARNING) << "broadcast source certificate is invalid";
    return;
  }
  OverlayOutboundFecBroadcast::create(std::move(data), flags, actor_id(this), send_as,
                                      opts_.broadcast_speed_multiplier_);
}

void OverlayImpl::print(td::StringBuilder &sb) {
  sb << this;
}

td::Status OverlayImpl::check_date(td::uint32 date) {
  auto n = td::Clocks::system();
  if (date < n - 20) {
    return td::Status::Error(ErrorCode::notready, "too old broadcast");
  }
  if (date > n + 20) {
    return td::Status::Error(ErrorCode::notready, "too new broadcast");
  }
  return td::Status::OK();
}

BroadcastCheckResult OverlayImpl::check_source_eligible(const PublicKeyHash& source, const Certificate* cert,
                                                        td::uint32 size, bool is_fec) {
  if (size == 0) {
    return BroadcastCheckResult::Forbidden;
  }
  auto r = rules_.check_rules(source, size, is_fec);
  if (!cert || r == BroadcastCheckResult::Allowed) {
    return r;
  }
  td::Bits256 cert_hash = get_tl_object_sha_bits256(cert->tl());
  auto cached_cert = checked_certificates_cache_.find(source);
  bool cached = cached_cert != checked_certificates_cache_.end() && cached_cert->second->cert_hash == cert_hash;

  auto r2 = cert->check(source, overlay_id_, static_cast<td::int32>(td::Clocks::system()), size, is_fec,
                        /* skip_check_signature = */ cached);
  if (r2 != BroadcastCheckResult::Forbidden) {
    if (cached_cert == checked_certificates_cache_.end()) {
      cached_cert = checked_certificates_cache_.emplace(
          source, std::make_unique<CachedCertificate>(source, cert_hash)).first;
    } else {
      cached_cert->second->cert_hash = cert_hash;
      cached_cert->second->remove();
    }
    checked_certificates_cache_lru_.put(cached_cert->second.get());
    while (checked_certificates_cache_.size() > max_checked_certificates_cache_size_) {
      auto to_remove = (CachedCertificate*)checked_certificates_cache_lru_.get();
      CHECK(to_remove);
      to_remove->remove();
      checked_certificates_cache_.erase(to_remove->source);
    }
  }
  r2 = broadcast_check_result_min(r2, rules_.check_rules(cert->issuer_hash(), size, is_fec));
  return broadcast_check_result_max(r, r2);
}

BroadcastCheckResult OverlayImpl::check_source_eligible(PublicKey source, const Certificate* cert, td::uint32 size,
                                                        bool is_fec) {
  return check_source_eligible(source.compute_short_id(), cert, size, is_fec);
}

td::Status OverlayImpl::check_delivered(BroadcastHash hash) {
  if (delivered_broadcasts_.count(hash) == 1 || broadcasts_.count(hash) == 1) {
    return td::Status::Error(ErrorCode::notready, "duplicate broadcast");
  } else {
    return td::Status::OK();
  }
}

BroadcastFec *OverlayImpl::get_fec_broadcast(BroadcastHash hash) {
  auto it = fec_broadcasts_.find(hash);
  if (it == fec_broadcasts_.end()) {
    return nullptr;
  } else {
    return it->second.get();
  }
}

void OverlayImpl::register_fec_broadcast(std::unique_ptr<BroadcastFec> bcast) {
  auto hash = bcast->get_hash();
  bcast_fec_lru_.put(bcast.get());
  fec_broadcasts_.emplace(hash, std::move(bcast));
  bcast_gc();
}

void OverlayImpl::get_self_node(td::Promise<OverlayNode> promise) {
  OverlayNode s{local_id_, overlay_id_, peer_list_.local_member_flags_};
  auto to_sign = s.to_sign();
  auto P = td::PromiseCreator::lambda(
      [oid = print_id(), s = std::move(s), cert = peer_list_.cert_,
       promise = std::move(promise)](td::Result<std::pair<td::BufferSlice, PublicKey>> R) mutable {
        if (R.is_error()) {
          auto S = R.move_as_error();
          LOG(ERROR) << oid << ": failed to get self node: " << S;
          promise.set_error(std::move(S));
          return;
        }
        auto V = R.move_as_ok();
        s.update_signature(std::move(V.first));
        s.update_adnl_id(adnl::AdnlNodeIdFull{V.second});
        s.update_certificate(std::move(cert));
        promise.set_value(std::move(s));
      });

  td::actor::send_closure(keyring_, &keyring::Keyring::sign_add_get_public_key, local_id_.pubkey_hash(),
                          std::move(to_sign), std::move(P));
}

void OverlayImpl::send_new_fec_broadcast_part(PublicKeyHash local_id, Overlay::BroadcastDataHash data_hash,
                                              td::uint32 size, td::uint32 flags, td::BufferSlice part, td::uint32 seqno,
                                              fec::FecType fec_type, td::uint32 date) {
  auto S = OverlayFecBroadcastPart::create_new(this, actor_id(this), local_id, data_hash, size, flags, std::move(part),
                                               seqno, std::move(fec_type), date);
  if (S.is_error() && S.code() != ErrorCode::notready) {
    LOG(WARNING) << "failed to send broadcast part: " << S;
  }
}

void OverlayImpl::deliver_broadcast(PublicKeyHash source, td::BufferSlice data) {
  callback_->receive_broadcast(source, overlay_id_, std::move(data));
}

void OverlayImpl::failed_to_create_fec_broadcast(td::Status reason) {
  if (reason.code() == ErrorCode::notready) {
    LOG(DEBUG) << "failed to receive fec broadcast: " << reason;
  } else {
    LOG(WARNING) << "failed to receive fec broadcast: " << reason;
  }
}

void OverlayImpl::created_fec_broadcast(PublicKeyHash local_id, std::unique_ptr<OverlayFecBroadcastPart> bcast) {
  bcast->update_overlay(this);
  auto S = bcast->run();
  if (S.is_error() && S.code() != ErrorCode::notready) {
    LOG(WARNING) << "failed to send fec broadcast: " << S;
  }
}

void OverlayImpl::failed_to_create_simple_broadcast(td::Status reason) {
  if (reason.code() == ErrorCode::notready) {
    LOG(DEBUG) << "failed to send simple broadcast: " << reason;
  } else {
    LOG(WARNING) << "failed to send simple broadcast: " << reason;
  }
}

void OverlayImpl::created_simple_broadcast(std::unique_ptr<BroadcastSimple> bcast) {
  bcast->update_overlay(this);
  auto S = bcast->run();
  register_simple_broadcast(std::move(bcast));
  if (S.is_error() && S.code() != ErrorCode::notready) {
    LOG(WARNING) << "failed to receive fec broadcast: " << S;
  }
}

void OverlayImpl::register_simple_broadcast(std::unique_ptr<BroadcastSimple> bcast) {
  auto hash = bcast->get_hash();
  bcast_data_lru_.put(bcast.get());
  broadcasts_.emplace(hash, std::move(bcast));
  bcast_gc();
}

td::Result<Encryptor *> OverlayImpl::get_encryptor(PublicKey source) {
  auto short_id = source.compute_short_id();
  auto it = encryptor_map_.find(short_id);
  if (it != encryptor_map_.end()) {
    return it->second->get();
  }
  TRY_RESULT(e, source.create_encryptor());
  auto res = e.get();
  auto cache = std::make_unique<CachedEncryptor>(short_id, std::move(e));
  encryptor_lru_.put(cache.get());
  encryptor_map_.emplace(short_id, std::move(cache));
  while (encryptor_map_.size() > max_encryptors()) {
    auto x = CachedEncryptor::from_list_node(encryptor_lru_.get());
    auto id = x->id();
    encryptor_map_.erase(id);
  }
  return res;
}

std::shared_ptr<Certificate> OverlayImpl::get_certificate(PublicKeyHash source) {
  auto it = certs_.find(source);
  return (it == certs_.end()) ? nullptr : it->second;
}

void OverlayImpl::set_privacy_rules(OverlayPrivacyRules rules) {
  rules_ = std::move(rules);
}

void OverlayImpl::check_broadcast(PublicKeyHash src, td::BufferSlice data, td::Promise<td::Unit> promise) {
  callback_->check_broadcast(src, overlay_id_, std::move(data), std::move(promise));
}

void OverlayImpl::broadcast_checked(Overlay::BroadcastHash hash, td::Result<td::Unit> R) {
  {
    auto it = broadcasts_.find(hash);
    if (it != broadcasts_.end()) {
      it->second->broadcast_checked(std::move(R));
    }
  }
  {
    auto it = fec_broadcasts_.find(hash);
    if (it != fec_broadcasts_.end()) {
      it->second->broadcast_checked(std::move(R));
    }
  }
}

void OverlayImpl::get_stats(td::Promise<tl_object_ptr<ton_api::engine_validator_overlayStats>> promise) {
  auto res = create_tl_object<ton_api::engine_validator_overlayStats>();
  res->adnl_id_ = local_id_.bits256_value();
  res->overlay_id_ = overlay_id_.bits256_value();
  res->overlay_id_full_ = id_full_.pubkey().tl();
  res->scope_ = scope_;
  iterate_all_peers([&](const adnl::AdnlNodeIdShort &key, const OverlayPeer &peer) {
    auto node_obj = create_tl_object<ton_api::engine_validator_overlayStatsNode>();
    node_obj->adnl_id_ = key.bits256_value();
    node_obj->traffic_ = peer.traffic.tl();
    node_obj->traffic_responses_ = peer.traffic_responses.tl();
    node_obj->ip_addr_ = peer.ip_addr_str;

    node_obj->last_in_query_ = static_cast<td::uint32>(peer.last_in_query_at.at_unix());
    node_obj->last_out_query_ = static_cast<td::uint32>(peer.last_out_query_at.at_unix());

    node_obj->bdcst_errors_ = peer.broadcast_errors;
    node_obj->fec_bdcst_errors_ = peer.fec_broadcast_errors;

    node_obj->is_neighbour_ = peer.is_neighbour();
    node_obj->is_alive_ = peer.is_alive();
    node_obj->node_flags_ = peer.get_node()->flags();

    node_obj->last_ping_at_ = (peer.last_ping_at ? peer.last_ping_at.at_unix() : -1.0);
    node_obj->last_ping_time_ = peer.last_ping_time;

    res->nodes_.push_back(std::move(node_obj));
  });

  res->total_traffic_ = total_traffic.tl();
  res->total_traffic_responses_ = total_traffic_responses.tl();
  res->stats_.push_back(
      create_tl_object<ton_api::engine_validator_oneStat>("neighbours_cnt", PSTRING() << neighbours_cnt()));

  callback_->get_stats_extra([promise = std::move(promise), res = std::move(res)](td::Result<std::string> R) mutable {
    if (R.is_ok()) {
      res->extra_ = R.move_as_ok();
    }
    promise.set_value(std::move(res));
  });
}

bool OverlayImpl::has_valid_broadcast_certificate(const PublicKeyHash &source, size_t size, bool is_fec) {
  if (size > std::numeric_limits<td::uint32>::max()) {
    return false;
  }
  auto it = certs_.find(source);
  return check_source_eligible(source, it == certs_.end() ? nullptr : it->second.get(), (td::uint32)size, is_fec) !=
         BroadcastCheckResult::Forbidden;
}

void TrafficStats::add_packet(td::uint64 size, bool in) {
  if (in) {
    ++in_packets;
    in_bytes += size;
  } else {
    ++out_packets;
    out_bytes += size;
  }
}

void TrafficStats::normalize(double elapsed) {
  out_bytes = static_cast<td::uint64>(out_bytes / elapsed);
  in_bytes = static_cast<td::uint64>(in_bytes / elapsed);
  out_packets = static_cast<td::uint32>(out_packets / elapsed);
  in_packets = static_cast<td::uint32>(in_packets / elapsed);
}

tl_object_ptr<ton_api::engine_validator_overlayStatsTraffic> TrafficStats::tl() const {
  return create_tl_object<ton_api::engine_validator_overlayStatsTraffic>(out_bytes, in_bytes, out_packets, in_packets);
}

}  // namespace overlay

}  // namespace ton
