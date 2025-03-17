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
#include "adnl/adnl-node-id.hpp"
#include "adnl/adnl-node.h"
#include "auto/tl/ton_api.h"
#include "overlay.hpp"
#include "td/utils/Status.h"
#include "td/utils/Time.h"
#include "td/utils/port/signals.h"
#include <algorithm>
#include <vector>

namespace ton {

namespace overlay {

void OverlayImpl::del_peer(const adnl::AdnlNodeIdShort &id) {
  auto P = peer_list_.peers_.get(id);
  if (P == nullptr) {
    return;
  }
  if (P->is_permanent_member()) {
    VLOG(OVERLAY_DEBUG) << this << ": not deleting peer " << id << ": a permanent member";
    return;
  }

  VLOG(OVERLAY_DEBUG) << this << ": deleting peer " << id;

  if (P->is_neighbour()) {
    del_from_neighbour_list(P);
  }
  peer_list_.peers_.remove(id);
  peer_list_.bad_peers_.erase(id);
}

void OverlayImpl::del_from_neighbour_list(OverlayPeer *P) {
  CHECK(P);
  if (!P->is_neighbour()) {
    return;
  }
  auto id = P->get_id();
  bool deleted = false;
  auto &neighbours = peer_list_.neighbours_;
  for (auto &n : neighbours) {
    if (n == id) {
      n = neighbours[neighbours.size() - 1];
      neighbours.resize(neighbours.size() - 1);
      deleted = true;
      break;
    }
  }
  CHECK(deleted);
  P->set_neighbour(false);
}

void OverlayImpl::del_from_neighbour_list(const adnl::AdnlNodeIdShort &id) {
  auto P = peer_list_.peers_.get(id);
  CHECK(P != nullptr);
  return del_from_neighbour_list(P);
}

void OverlayImpl::del_some_peers() {
  if (overlay_type_ == OverlayType::FixedMemberList) {
    return;
  }
  const size_t max_iterations = 10;
  size_t iteration_seqno = 0;
  while (peer_list_.peers_.size() > max_peers() && iteration_seqno++ < max_iterations) {
    OverlayPeer *P;
    if (peer_list_.bad_peers_.empty()) {
      P = get_random_peer();
    } else {
      auto it = peer_list_.bad_peers_.upper_bound(peer_list_.next_bad_peer_);
      if (it == peer_list_.bad_peers_.end()) {
        it = peer_list_.bad_peers_.begin();
      }
      P = peer_list_.peers_.get(peer_list_.next_bad_peer_ = *it);
    }
    if (P && !P->is_permanent_member()) {
      auto id = P->get_id();
      del_peer(id);
    }
  }
  update_neighbours(0);
}

td::Status OverlayImpl::validate_peer_certificate(const adnl::AdnlNodeIdShort &node,
                                                  const OverlayMemberCertificate &cert) {
  if (cert.empty()) {
    if (is_persistent_node(node) || overlay_type_ == OverlayType::Public) {
      return td::Status::OK();
    }
    return td::Status::Error(ErrorCode::protoviolation, "no member certificate found");
  }
  if (cert.is_expired()) {
    return td::Status::Error(ErrorCode::timeout, "member certificate is expired");
  }
  if (cert.slot() < 0 || cert.slot() >= opts_.max_slaves_in_semiprivate_overlay_) {
    return td::Status::Error(ErrorCode::timeout, "member certificate has invalid slot");
  }
  const auto &issued_by = cert.issued_by();
  auto it = peer_list_.root_public_keys_.find(issued_by.compute_short_id());
  if (it == peer_list_.root_public_keys_.end()) {
    return td::Status::Error(ErrorCode::protoviolation, "member certificate is signed by unknown public key");
  }
  if (it->second.size() > (size_t)cert.slot()) {
    auto &el = it->second[cert.slot()];
    if (cert.expire_at() < el.expire_at) {
      return td::Status::Error(ErrorCode::protoviolation,
                               "member certificate rejected, because we know of newer certificate at the same slot");
    } else if (cert.expire_at() == el.expire_at) {
      if (node < el.node) {
        return td::Status::Error(ErrorCode::protoviolation,
                                 "member certificate rejected, because we know of newer certificate at the same slot");
      } else if (el.node == node) {
        // we could return OK here, but we must make sure, that the unchecked signature will not be used for updating PeerNode.
      }
    }
  }
  auto R = get_encryptor(issued_by);
  if (R.is_error()) {
    return R.move_as_error_prefix("failed to check member certificate: failed to create encryptor: ");
  }
  auto enc = R.move_as_ok();
  auto S = enc->check_signature(cert.to_sign_data(node).as_slice(), cert.signature());
  if (S.is_error()) {
    return S.move_as_error_prefix("failed to check member certificate: bad signature: ");
  }
  if (it->second.size() <= (size_t)cert.slot()) {
    it->second.resize((size_t)cert.slot() + 1);
  }
  it->second[cert.slot()].expire_at = cert.expire_at();
  it->second[cert.slot()].node = node;
  return td::Status::OK();
}

td::Status OverlayImpl::validate_peer_certificate(const adnl::AdnlNodeIdShort &node,
                                                  ton_api::overlay_MemberCertificate *cert) {
  OverlayMemberCertificate ncert(cert);
  return validate_peer_certificate(node, ncert);
}

td::Status OverlayImpl::validate_peer_certificate(const adnl::AdnlNodeIdShort &node,
                                                  const OverlayMemberCertificate *cert) {
  if (!cert) {
    if (is_persistent_node(node) || overlay_type_ == OverlayType::Public) {
      return td::Status::OK();
    }
    return td::Status::Error(ErrorCode::protoviolation, "no member certificate found");
  }
  return validate_peer_certificate(node, *cert);
}

void OverlayImpl::add_peer(OverlayNode node) {
  CHECK(overlay_type_ != OverlayType::FixedMemberList);
  if (node.overlay_id() != overlay_id_) {
    VLOG(OVERLAY_WARNING) << this << ": received node with bad overlay";
    return;
  }
  auto t = td::Clocks::system();
  if (node.version() + Overlays::overlay_peer_ttl() < t || node.version() > t + 60) {
    VLOG(OVERLAY_INFO) << this << ": ignoring node of too old version " << node.version();
    return;
  }

  auto pub_id = node.adnl_id_full();
  if (pub_id.compute_short_id() == local_id_) {
    VLOG(OVERLAY_DEBUG) << this << ": ignoring self node";
    return;
  }

  auto S = node.check_signature();
  if (S.is_error()) {
    VLOG(OVERLAY_WARNING) << this << ": bad signature: " << S;
    return;
  }

  if (overlay_type_ == OverlayType::CertificatedMembers) {
    auto R = validate_peer_certificate(node.adnl_id_short(), *node.certificate());
    if (R.is_error()) {
      VLOG(OVERLAY_WARNING) << this << ": bad peer certificate node=" << node.adnl_id_short() << ": "
                            << R.move_as_error();
      UNREACHABLE();
      return;
    }
  }

  auto id = node.adnl_id_short();

  auto V = peer_list_.peers_.get(id);
  if (V) {
    VLOG(OVERLAY_DEBUG) << this << ": updating peer " << id << " up to version " << node.version();
    V->update(std::move(node));
  } else {
    VLOG(OVERLAY_DEBUG) << this << ": adding peer " << id << " of version " << node.version();
    CHECK(overlay_type_ != OverlayType::CertificatedMembers || (node.certificate() && !node.certificate()->empty()));
    peer_list_.peers_.insert(id, OverlayPeer(std::move(node)));
    del_some_peers();
    auto X = peer_list_.peers_.get(id);
    if (X != nullptr && !X->is_neighbour() && peer_list_.neighbours_.size() < max_neighbours() &&
        !(X->get_node()->flags() & OverlayMemberFlags::DoNotReceiveBroadcasts) && X->get_id() != local_id_) {
      peer_list_.neighbours_.push_back(X->get_id());
      X->set_neighbour(true);
    }

    update_neighbours(0);
  }
}

void OverlayImpl::add_peers(std::vector<OverlayNode> peers) {
  for (auto &node : peers) {
    add_peer(std::move(node));
  }
}

void OverlayImpl::add_peers(const tl_object_ptr<ton_api::overlay_nodes> &nodes) {
  for (auto &n : nodes->nodes_) {
    auto N = OverlayNode::create(n);
    if (N.is_ok()) {
      add_peer(N.move_as_ok());
    }
  }
}

void OverlayImpl::add_peers(const tl_object_ptr<ton_api::overlay_nodesV2> &nodes) {
  for (auto &n : nodes->nodes_) {
    auto N = OverlayNode::create(n);
    if (N.is_ok()) {
      add_peer(N.move_as_ok());
    }
  }
}

void OverlayImpl::on_ping_result(adnl::AdnlNodeIdShort peer, bool success, double store_ping_time) {
  if (overlay_type_ == OverlayType::FixedMemberList && (!success || store_ping_time < 0.0)) {
    return;
  }
  if (OverlayPeer *p = peer_list_.peers_.get(peer)) {
    p->on_ping_result(success);
    if (store_ping_time >= 0.0 && success) {
      p->last_ping_at = td::Timestamp::now();
      p->last_ping_time = store_ping_time;
    }
    if (p->is_alive()) {
      peer_list_.bad_peers_.erase(peer);
    } else {
      peer_list_.bad_peers_.insert(peer);
    }
  }
}

void OverlayImpl::receive_random_peers(adnl::AdnlNodeIdShort src, td::Result<td::BufferSlice> R, double elapsed) {
  CHECK(overlay_type_ != OverlayType::FixedMemberList);
  on_ping_result(src, R.is_ok(), elapsed);
  if (R.is_error()) {
    VLOG(OVERLAY_NOTICE) << this << ": failed getRandomPeers query: " << R.move_as_error();
    return;
  }
  auto R2 = fetch_tl_object<ton_api::overlay_nodes>(R.move_as_ok(), true);
  if (R2.is_error()) {
    VLOG(OVERLAY_WARNING) << this << ": dropping incorrect answer to overlay.getRandomPeers query from " << src << ": "
                          << R2.move_as_error();
    return;
  }

  add_peers(R2.move_as_ok());
}

void OverlayImpl::receive_random_peers_v2(adnl::AdnlNodeIdShort src, td::Result<td::BufferSlice> R, double elapsed) {
  CHECK(overlay_type_ != OverlayType::FixedMemberList);
  on_ping_result(src, R.is_ok(), elapsed);
  if (R.is_error()) {
    VLOG(OVERLAY_NOTICE) << this << ": failed getRandomPeersV2 query: " << R.move_as_error();
    return;
  }
  auto R2 = fetch_tl_object<ton_api::overlay_nodesV2>(R.move_as_ok(), true);
  if (R2.is_error()) {
    VLOG(OVERLAY_WARNING) << this << ": dropping incorrect answer to overlay.getRandomPeers query from " << src << ": "
                          << R2.move_as_error();
    return;
  }

  add_peers(R2.move_as_ok());
}

void OverlayImpl::send_random_peers_cont(adnl::AdnlNodeIdShort src, OverlayNode node,
                                         td::Promise<td::BufferSlice> promise) {
  std::vector<tl_object_ptr<ton_api::overlay_node>> vec;
  if (announce_self_) {
    vec.emplace_back(node.tl());
  }

  td::uint32 max_iterations = nodes_to_send() + 16;
  for (td::uint32 i = 0; i < max_iterations && vec.size() < nodes_to_send(); i++) {
    auto P = get_random_peer(true);
    if (P) {
      if (P->has_full_id()) {
        vec.emplace_back(P->get_node()->tl());
      }
    } else {
      break;
    }
  }

  if (promise) {
    auto Q = create_tl_object<ton_api::overlay_nodes>(std::move(vec));
    promise.set_value(serialize_tl_object(Q, true));
  } else {
    auto P = td::PromiseCreator::lambda(
        [SelfId = actor_id(this), src, timer = td::Timer()](td::Result<td::BufferSlice> res) {
          td::actor::send_closure(SelfId, &OverlayImpl::receive_random_peers, src, std::move(res), timer.elapsed());
        });
    auto Q =
        create_tl_object<ton_api::overlay_getRandomPeers>(create_tl_object<ton_api::overlay_nodes>(std::move(vec)));
    td::actor::send_closure(manager_, &OverlayManager::send_query, src, local_id_, overlay_id_,
                            "overlay getRandomPeers", std::move(P),
                            td::Timestamp::in(5.0 + td::Random::fast(0, 50) * 0.1), serialize_tl_object(Q, true));
  }
}

void OverlayImpl::send_random_peers(adnl::AdnlNodeIdShort src, td::Promise<td::BufferSlice> promise) {
  auto P = td::PromiseCreator::lambda([src, promise = std::move(promise),
                                       SelfId = actor_id(this)](td::Result<OverlayNode> res) mutable {
    if (res.is_error()) {
      promise.set_error(td::Status::Error(ErrorCode::error, "cannot get self node"));
      return;
    }
    td::actor::send_closure(SelfId, &OverlayImpl::send_random_peers_cont, src, res.move_as_ok(), std::move(promise));
  });

  get_self_node(std::move(P));
}

void OverlayImpl::send_random_peers_v2_cont(adnl::AdnlNodeIdShort src, OverlayNode node,
                                            td::Promise<td::BufferSlice> promise) {
  std::vector<tl_object_ptr<ton_api::overlay_nodeV2>> vec;
  if (announce_self_) {
    CHECK(is_persistent_node(node.adnl_id_short()) || !node.certificate()->empty());
    vec.emplace_back(node.tl_v2());
  }

  td::uint32 max_iterations = nodes_to_send() + 16;
  for (td::uint32 i = 0; i < max_iterations && vec.size() < nodes_to_send(); i++) {
    auto P = get_random_peer(true);
    if (P) {
      if (P->has_full_id() && !P->is_permanent_member()) {
        vec.emplace_back(P->get_node()->tl_v2());
      }
    } else {
      break;
    }
  }

  if (promise) {
    auto Q = create_tl_object<ton_api::overlay_nodesV2>(std::move(vec));
    promise.set_value(serialize_tl_object(Q, true));
  } else {
    auto P = td::PromiseCreator::lambda(
        [SelfId = actor_id(this), src, timer = td::Timer()](td::Result<td::BufferSlice> res) {
          td::actor::send_closure(SelfId, &OverlayImpl::receive_random_peers_v2, src, std::move(res), timer.elapsed());
        });
    auto Q =
        create_tl_object<ton_api::overlay_getRandomPeersV2>(create_tl_object<ton_api::overlay_nodesV2>(std::move(vec)));
    td::actor::send_closure(manager_, &OverlayManager::send_query, src, local_id_, overlay_id_,
                            "overlay getRandomPeers", std::move(P),
                            td::Timestamp::in(5.0 + td::Random::fast(0, 50) * 0.1), serialize_tl_object(Q, true));
  }
}

void OverlayImpl::send_random_peers_v2(adnl::AdnlNodeIdShort src, td::Promise<td::BufferSlice> promise) {
  auto P = td::PromiseCreator::lambda([src, promise = std::move(promise),
                                       SelfId = actor_id(this)](td::Result<OverlayNode> res) mutable {
    if (res.is_error()) {
      promise.set_error(td::Status::Error(ErrorCode::error, "cannot get self node"));
      return;
    }
    td::actor::send_closure(SelfId, &OverlayImpl::send_random_peers_v2_cont, src, res.move_as_ok(), std::move(promise));
  });

  get_self_node(std::move(P));
}

void OverlayImpl::ping_random_peers() {
  auto peers = get_neighbours(5);
  for (const adnl::AdnlNodeIdShort &peer : peers) {
    auto P =
        td::PromiseCreator::lambda([SelfId = actor_id(this), peer, timer = td::Timer(), oid = print_id()](td::Result<td::BufferSlice> R) {
          if (R.is_error()) {
            VLOG(OVERLAY_INFO) << oid << " ping to " << peer << " failed : " << R.move_as_error();
            return;
          }
          td::actor::send_closure(SelfId, &OverlayImpl::receive_pong, peer, timer.elapsed());
        });
    td::actor::send_closure(manager_, &OverlayManager::send_query, peer, local_id_, overlay_id_, "overlay ping",
                            std::move(P), td::Timestamp::in(5.0), create_serialize_tl_object<ton_api::overlay_ping>());
  }
}

void OverlayImpl::receive_pong(adnl::AdnlNodeIdShort peer, double elapsed) {
  on_ping_result(peer, true, elapsed);
}

void OverlayImpl::update_neighbours(td::uint32 nodes_to_change) {
  if (peer_list_.peers_.size() == 0) {
    return;
  }
  td::uint32 iter = 0;
  while (iter++ < 10 && (nodes_to_change > 0 || peer_list_.neighbours_.size() < max_neighbours())) {
    auto X = peer_list_.peers_.get_random();
    if (!X) {
      break;
    }

    if (X->get_id() == local_id_) {
      continue;
    }

    if (overlay_type_ != OverlayType::FixedMemberList && X->get_version() <= td::Clocks::system() -
        Overlays::overlay_peer_ttl()) {
      if (X->is_permanent_member()) {
        del_from_neighbour_list(X);
      } else {
        auto id = X->get_id();
        del_peer(id);
      }
      continue;
    }

    if (overlay_type_ == OverlayType::CertificatedMembers && !X->is_permanent_member() &&
        X->certificate()->is_expired()) {
      auto id = X->get_id();
      del_peer(id);
      continue;
    }

    if (X->get_node()->flags() & OverlayMemberFlags::DoNotReceiveBroadcasts) {
      if (X->is_neighbour()) {
        del_from_neighbour_list(X);
      }
      continue;
    }

    if (X->is_neighbour()) {
      continue;
    }

    if (peer_list_.neighbours_.size() < max_neighbours()) {
      VLOG(OVERLAY_INFO) << this << ": adding new neighbour " << X->get_id();
      peer_list_.neighbours_.push_back(X->get_id());
      X->set_neighbour(true);
    } else if (X->is_alive()) {
      CHECK(nodes_to_change > 0);
      auto i = td::Random::fast(0, static_cast<td::uint32>(peer_list_.neighbours_.size()) - 1);
      auto Y = peer_list_.peers_.get(peer_list_.neighbours_[i]);
      CHECK(Y != nullptr);
      CHECK(Y->is_neighbour());
      Y->set_neighbour(false);
      peer_list_.neighbours_[i] = X->get_id();
      X->set_neighbour(true);
      nodes_to_change--;
      VLOG(OVERLAY_INFO) << this << ": changing neighbour " << Y->get_id() << " -> " << X->get_id();
    }
  }
}

OverlayPeer *OverlayImpl::get_random_peer(bool only_alive) {
  size_t skip_bad = 3;
  OverlayPeer *res = nullptr;
  while (!res && peer_list_.peers_.size() > (only_alive ? peer_list_.bad_peers_.size() : 0)) {
    auto P = peer_list_.peers_.get_random();
    if (!P->is_permanent_member() &&
        (P->get_version() + 3600 < td::Clocks::system() || P->certificate()->is_expired())) {
      VLOG(OVERLAY_INFO) << this << ": deleting outdated peer " << P->get_id();
      del_peer(P->get_id());
      continue;
    }
    if (!P->is_alive()) {
      if (only_alive) {
        continue;
      }
      if (skip_bad > 0) {
        --skip_bad;
        continue;
      }
    }
    res = P;
  }
  update_neighbours(0);
  return res;
}

void OverlayImpl::get_overlay_random_peers(td::uint32 max_peers,
                                           td::Promise<std::vector<adnl::AdnlNodeIdShort>> promise) {
  std::vector<adnl::AdnlNodeIdShort> v;
  auto t = td::Clocks::system();
  while (v.size() < max_peers && v.size() < peer_list_.peers_.size() - peer_list_.bad_peers_.size()) {
    auto P = peer_list_.peers_.get_random();
    if (!P->is_permanent_member() && (P->get_version() + 3600 < t || P->certificate()->is_expired(t))) {
      VLOG(OVERLAY_INFO) << this << ": deleting outdated peer " << P->get_id();
      del_peer(P->get_id());
    } else if (P->is_alive()) {
      bool dup = false;
      for (auto &n : v) {
        if (n == P->get_id()) {
          dup = true;
          break;
        }
      }
      if (!dup) {
        v.push_back(P->get_id());
      }
    }
  }
  update_neighbours(0);
  promise.set_result(std::move(v));
}

void OverlayImpl::receive_nodes_from_db(tl_object_ptr<ton_api::overlay_nodes> tl_nodes) {
  if (overlay_type_ != OverlayType::FixedMemberList) {
    add_peers(tl_nodes);
  }
}

void OverlayImpl::receive_nodes_from_db_v2(tl_object_ptr<ton_api::overlay_nodesV2> tl_nodes) {
  if (overlay_type_ != OverlayType::FixedMemberList) {
    add_peers(tl_nodes);
  }
}

bool OverlayImpl::is_persistent_node(const adnl::AdnlNodeIdShort &id) {
  auto P = peer_list_.peers_.get(id);
  if (!P) {
    return false;
  }
  return P->is_permanent_member();
}

bool OverlayImpl::is_valid_peer(const adnl::AdnlNodeIdShort &src,
                                const ton_api::overlay_MemberCertificate *certificate) {
  if (overlay_type_ == OverlayType::Public) {
    on_ping_result(src, true);
    return true;
  } else if (overlay_type_ == OverlayType::FixedMemberList) {
    return peer_list_.peers_.get(src);
  } else {
    OverlayMemberCertificate cert(certificate);
    if (cert.empty()) {
      auto P = peer_list_.peers_.get(src);
      if (P && !P->is_permanent_member()) {
        auto C = P->certificate();
        if (C) {
          cert = *C;
        }
      }
    }

    auto S = validate_peer_certificate(src, cert);
    if (S.is_error()) {
      VLOG(OVERLAY_WARNING) << "adnl=" << src << ": certificate is invalid: " << S;
      return false;
    }
    auto P = peer_list_.peers_.get(src);
    if (P) {
      CHECK(P->is_permanent_member() || !cert.empty());
      P->update_certificate(std::move(cert));
    }
    return true;
  }
}

void OverlayImpl::iterate_all_peers(std::function<void(const adnl::AdnlNodeIdShort &key, OverlayPeer &peer)> cb) {
  peer_list_.peers_.iterate([&](const adnl::AdnlNodeIdShort &key, OverlayPeer &peer) { cb(key, peer); });
}

void OverlayImpl::update_peer_err_ctr(adnl::AdnlNodeIdShort peer_id, bool is_fec) {
  auto src_peer = peer_list_.peers_.get(peer_id);
  if (src_peer) {
    if (is_fec) {
      src_peer->fec_broadcast_errors++;
    } else {
      src_peer->broadcast_errors++;
    }
  }
}

void OverlayImpl::update_throughput_out_ctr(adnl::AdnlNodeIdShort peer_id, td::uint64 msg_size, bool is_query,
                                            bool is_response) {
  auto out_peer = peer_list_.peers_.get(peer_id);
  if (out_peer) {
    out_peer->traffic_ctr.add_packet(msg_size, false);
    if (is_response) {
      out_peer->traffic_responses_ctr.add_packet(msg_size, false);
    }
    if (is_query) {
      out_peer->last_out_query_at = td::Timestamp::now();
    }
  }
  total_traffic_ctr.add_packet(msg_size, false);
  if (is_response) {
    total_traffic_responses_ctr.add_packet(msg_size, false);
  }
}

void OverlayImpl::update_throughput_in_ctr(adnl::AdnlNodeIdShort peer_id, td::uint64 msg_size, bool is_query,
                                           bool is_response) {
  auto in_peer = peer_list_.peers_.get(peer_id);
  if (in_peer) {
    in_peer->traffic_ctr.add_packet(msg_size, true);
    if (is_response) {
      in_peer->traffic_responses_ctr.add_packet(msg_size, true);
    }
    if (is_query) {
      in_peer->last_in_query_at = td::Timestamp::now();
    }
  }
  total_traffic_ctr.add_packet(msg_size, true);
  if (is_response) {
    total_traffic_responses_ctr.add_packet(msg_size, true);
  }
}

void OverlayImpl::update_peer_ip_str(adnl::AdnlNodeIdShort peer_id, td::string ip_str) {
  auto fpeer = peer_list_.peers_.get(peer_id);
  if (fpeer) {
    fpeer->ip_addr_str = ip_str;
  }
}

bool OverlayImpl::has_good_peers() const {
  return peer_list_.peers_.size() > peer_list_.bad_peers_.size();
}

bool OverlayImpl::is_root_public_key(const PublicKeyHash &key) const {
  return peer_list_.root_public_keys_.count(key) > 0;
}

std::vector<adnl::AdnlNodeIdShort> OverlayImpl::get_neighbours(td::uint32 max_size) const {
  if (max_size == 0 || max_size >= peer_list_.neighbours_.size()) {
    return peer_list_.neighbours_;
  } else {
    std::vector<adnl::AdnlNodeIdShort> vec;
    std::vector<td::uint32> ul;
    for (td::uint32 i = 0; i < max_size; i++) {
      td::uint32 t = td::Random::fast(0, static_cast<td::int32>(peer_list_.neighbours_.size()) - 1 - i);
      td::uint32 j;
      for (j = 0; j < i && ul[j] <= t; j++) {
        t++;
      }
      ul.emplace(ul.begin() + j, t);
      vec.push_back(peer_list_.neighbours_[t]);
    }
    return vec;
  }
}

void OverlayImpl::send_message_to_neighbours(td::BufferSlice data) {
  for (auto &n : peer_list_.neighbours_) {
    td::actor::send_closure(manager_, &OverlayManager::send_message, n, local_id_, overlay_id_, data.clone());
  }
}

size_t OverlayImpl::neighbours_cnt() const {
  return peer_list_.neighbours_.size();
}

void OverlayImpl::update_root_member_list(std::vector<adnl::AdnlNodeIdShort> ids,
                                          std::vector<PublicKeyHash> root_public_keys, OverlayMemberCertificate cert) {
  auto expected_size =
      (td::uint32)(ids.size() + root_public_keys.size() * opts_.max_slaves_in_semiprivate_overlay_);
  opts_.max_peers_ = std::max(opts_.max_peers_, expected_size);
  std::sort(ids.begin(), ids.end());
  auto old_root_public_keys = std::move(peer_list_.root_public_keys_);
  for (const auto &pub_key : root_public_keys) {
    auto it = old_root_public_keys.find(pub_key);
    if (it != old_root_public_keys.end()) {
      peer_list_.root_public_keys_.emplace(it->first, std::move(it->second));
    } else {
      peer_list_.root_public_keys_.emplace(pub_key, PeerList::SlaveKeys{});
    }
  }
  std::vector<adnl::AdnlNodeIdShort> to_del;
  peer_list_.peers_.iterate([&](const adnl::AdnlNodeIdShort &key, OverlayPeer &peer) {
    peer.set_permanent(std::binary_search(ids.begin(), ids.end(), key));
    if (peer.is_permanent_member()) {
      peer.clear_certificate();
    } else {
      auto S = validate_peer_certificate(peer.get_id(), peer.certificate());
      if (S.is_error()) {
        to_del.push_back(peer.get_id());
      }
    }
  });
  for (const auto &id : to_del) {
    del_peer(id);
  }
  for (const auto &id : ids) {
    if (!peer_list_.peers_.exists(id)) {
      OverlayNode node(id, overlay_id_, opts_.default_permanent_members_flags_);
      OverlayPeer peer(std::move(node));
      peer.set_permanent(true);
      CHECK(peer.is_permanent_member());
      peer_list_.peers_.insert(std::move(id), std::move(peer));
    }
  }

  update_member_certificate(std::move(cert));
  update_neighbours(0);
}

void OverlayImpl::update_member_certificate(OverlayMemberCertificate cert) {
  peer_list_.cert_ = std::move(cert);

  if (is_persistent_node(local_id_)) {
    peer_list_.local_cert_is_valid_until_ = td::Timestamp::in(86400.0 * 365 * 100); /* 100 years */
  } else {
    auto R = validate_peer_certificate(local_id_, &peer_list_.cert_);
    if (R.is_ok()) {
      peer_list_.local_cert_is_valid_until_ = td::Timestamp::at_unix(cert.expire_at());
    } else {
      peer_list_.local_cert_is_valid_until_ = td::Timestamp::never();
    }
  }
}

bool OverlayImpl::has_valid_membership_certificate() {
  if (overlay_type_ != OverlayType::CertificatedMembers) {
    return true;
  }

  if (!peer_list_.local_cert_is_valid_until_) {
    return false;
  }

  return !peer_list_.local_cert_is_valid_until_.is_in_past();
}

}  // namespace overlay

}  // namespace ton
