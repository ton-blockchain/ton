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
#include <algorithm>
#include <vector>

#include "adnl/adnl-node-id.hpp"
#include "adnl/adnl-node.h"
#include "auto/tl/ton_api.h"
#include "td/utils/Status.h"
#include "td/utils/Time.h"
#include "td/utils/port/signals.h"

#include "overlay.hpp"

namespace ton {

namespace overlay {

void OverlayImpl::del_peer(const adnl::AdnlNodeIdShort &id) {
  auto P = peer_list_.peers_.get(id);
  if (P == nullptr) {
    return;
  }
  if (P->is_permanent_member()) {
    VLOG(overlay, DEBUG) << this << ": not deleting peer " << id << ": a permanent member";
    return;
  }

  VLOG(overlay, DEBUG) << this << ": deleting peer " << id;

  broadcasts_plumtree_.remove_peer(this, id);
  peer_list_.peers_.remove(id);
}

void OverlayImpl::del_some_peers() {
  if (overlay_type_ == OverlayType::FixedMemberList) {
    return;
  }
  for (size_t i = 0; i < 10 && peer_list_.peers_.size() > max_peers(); ++i) {
    auto *peer = peer_list_.peers_.get_random();
    if (peer && !peer->is_permanent_member() && !broadcasts_plumtree_.is_eager_peer(peer->get_id())) {
      auto peer_id = peer->get_id();
      del_peer(peer_id);
    }
  }
}

td::Status OverlayImpl::validate_peer_certificate(const adnl::AdnlNodeIdShort &node,
                                                  const OverlayMemberCertificate &cert, bool received_from_node) {
  if (opts_.max_slaves_in_semiprivate_overlay_ == 0) {
    return td::Status::Error(ErrorCode::protoviolation, "member certificates are disabled");
  }
  if (cert.empty()) {
    if (is_persistent_node(node) || overlay_type_ == OverlayType::Public ||
        authorized_semiprivate_nodes_.contains(node)) {
      return td::Status::OK();
    }
    return td::Status::Error(ErrorCode::protoviolation, "no member certificate found");
  }
  if (cert.is_expired()) {
    return td::Status::Error(ErrorCode::timeout, "member certificate is expired");
  }
  if (cert.slot() < 0 || cert.slot() >= opts_.max_slaves_in_semiprivate_overlay_) {
    return td::Status::Error(ErrorCode::protoviolation, "member certificate has invalid slot");
  }
  const auto &issued_by = cert.issued_by();
  auto issued_by_short = issued_by.compute_short_id();
  auto it = peer_list_.root_public_keys_.find(issued_by_short);
  if (it == peer_list_.root_public_keys_.end()) {
    return td::Status::Error(ErrorCode::protoviolation, "member certificate is signed by unknown public key");
  }
  if (it->second.size() > (size_t)cert.slot()) {
    auto &el = it->second[cert.slot()];
    if (cert.expire_at() < el.expire_at) {
      return td::Status::Error(ErrorCode::protoviolation,
                               "member certificate rejected, because we know of newer certificate at the same slot");
    }
    if (cert.expire_at() == el.expire_at) {
      if (node < el.node) {
        return td::Status::Error(ErrorCode::protoviolation,
                                 "member certificate rejected, because we know of newer certificate at the same slot");
      }
    }
  }

  auto &limiter = authorized_key_limiters_[issued_by_short];
  std::pair cache_key{node, get_tl_object_sha_bits256(cert.tl())};
  bool cached = limiter.checked_member_certificates_lru_.contains(cache_key);
  if (!cached) {
    td::Timestamp now = td::Timestamp::now();
    if (!limiter.certificate_check_rate_limiter_.check(now)) {
      return td::Status::Error("member certificate rejected: rate limit exceeded");
    }
    TD_PERF_COUNTER(check_signature_overlay_member_certificate);
    TRY_STATUS_PREFIX(check_signature_from_peer(issued_by, cert.to_sign_data(node), cert.signature(),
                                                received_from_node ? node : adnl::AdnlNodeIdShort::zero()),
                      "failed to check member certificate: bad signature: ");
    limiter.certificate_check_rate_limiter_.insert(now);
    limiter.checked_member_certificates_lru_.put(cache_key, td::Unit{});
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
    if (is_persistent_node(node) || overlay_type_ == OverlayType::Public ||
        authorized_semiprivate_nodes_.contains(node)) {
      return td::Status::OK();
    }
    return td::Status::Error(ErrorCode::protoviolation, "no member certificate found");
  }
  return validate_peer_certificate(node, *cert);
}

void OverlayImpl::add_peer(OverlayNode node, bool verified, bool checked_signature) {
  td::Timestamp now = td::Timestamp::now();
  if (!verified && !receive_peers_rate_limiter_.check(now)) {
    VLOG(overlay, DEBUG) << this << ": dropping new peer: rate limit exceeded";
    return;
  }
  CHECK(overlay_type_ != OverlayType::FixedMemberList);
  if (node.overlay_id() != overlay_id_) {
    VLOG(overlay, WARNING) << this << ": received node with bad overlay";
    return;
  }
  auto t = td::Clocks::system();
  if (node.version() + Overlays::overlay_peer_ttl() < t || node.version() > t + 60) {
    VLOG(overlay, INFO) << this << ": ignoring node of too old version " << node.version();
    return;
  }

  auto pub_id = node.adnl_id_full();
  if (pub_id.compute_short_id() == local_id_) {
    VLOG(overlay, DEBUG) << this << ": ignoring self node";
    return;
  }

  if (!verified) {
    receive_peers_rate_limiter_.insert(now);
  }
  if (!checked_signature) {
    auto S = node.check_signature();
    if (S.is_error()) {
      VLOG(overlay, WARNING) << this << ": bad signature: " << S;
      return;
    }
  }

  if (overlay_type_ == OverlayType::Public) {
    node.clear_certificate();
  } else if (overlay_type_ == OverlayType::CertificatedMembers) {
    auto R = validate_peer_certificate(node.adnl_id_short(), *node.certificate());
    if (R.is_error()) {
      VLOG(overlay, WARNING) << this << ": bad peer certificate node=" << node.adnl_id_short() << ": "
                             << R.move_as_error();
      return;
    }
  }

  auto id = node.adnl_id_short();

  auto V = peer_list_.peers_.get(id);
  if (V) {
    VLOG(overlay, DEBUG) << this << ": updating peer " << id << " up to version " << node.version();
    V->update(std::move(node));
  } else if (verified) {
    VLOG(overlay, DEBUG) << this << ": adding peer " << id << " of version " << node.version();
    CHECK(overlay_type_ != OverlayType::CertificatedMembers || (node.certificate() && !node.certificate()->empty()) ||
          authorized_semiprivate_nodes_.contains(node.adnl_id_short()));
    peer_list_.peers_.insert(id, OverlayPeer(std::move(node)));
    peer_list_.pending_peers_.remove(id);
    del_some_peers();
  } else if (!processing_pending_peers_.contains(id)) {
    VLOG(overlay, DEBUG) << this << ": adding pending peer " << id << " of version " << node.version();
    auto pending = peer_list_.pending_peers_.get(id);
    if (pending) {
      pending->update(std::move(node));
    } else {
      peer_list_.pending_peers_.insert(id, std::move(node));
    }
    while (peer_list_.pending_peers_.size() > opts_.max_pending_peers_) {
      auto to_remove = peer_list_.pending_peers_.get_random();
      CHECK(to_remove);
      peer_list_.pending_peers_.remove(to_remove->adnl_id_short());
    }
    process_pending_peers();
  }
}

void OverlayImpl::process_pending_peers() {
  while (peer_list_.pending_peers_.size() > 0 && process_pending_peers_rate_limiter_.check(td::Timestamp::now())) {
    OverlayNode node = std::move(*peer_list_.pending_peers_.get_random());
    peer_list_.pending_peers_.remove(node.adnl_id_short());
    if (node.version() + Overlays::overlay_peer_ttl() < td::Clocks::system()) {
      VLOG(overlay, INFO) << this << ": dropping pending node " << node.adnl_id_short() << " of too old version "
                          << node.version();
      continue;
    }
    process_pending_peers_rate_limiter_.insert(td::Timestamp::now());
    processing_pending_peers_.insert(node.adnl_id_short());
    process_pending_peer(std::move(node)).start().detach_silent();
  }
}

td::actor::Task<> OverlayImpl::process_pending_peer(OverlayNode node) {
  CHECK(overlay_type_ == OverlayType::Public || overlay_type_ == OverlayType::CertificatedMembers);
  VLOG(overlay, INFO) << this << ": trying to probe pending peer " << node.adnl_id_short();
  for (size_t attempt = 0; attempt < 3; ++attempt) {
    auto [task, promise] = td::actor::StartedTask<td::BufferSlice>::make_bridge();
    td::actor::send_closure(manager_, &OverlayManager::send_query, node.adnl_id_short(), local_id_, overlay_id_,
                            "overlay peer probe", std::move(promise), td::Timestamp::in(5.0),
                            create_serialize_tl_object<ton_api::tonNode_getCapabilities>(), 1024);
    auto result = co_await std::move(task).wrap();
    if (result.is_error()) {
      continue;
    }
    auto capabilities = fetch_tl_object<ton_api::tonNode_capabilities>(result.move_as_ok(), true);
    if (capabilities.is_error()) {
      VLOG(overlay, INFO) << this << ": dropping pending peer " << node.adnl_id_short()
                          << " - invalid capabilities response";
      break;
    }
    auto version = std::make_pair(capabilities.ok()->version_major_, capabilities.ok()->version_minor_);
    if (version < std::make_pair<td::int32, td::int32>(3, 3)) {
      VLOG(overlay, INFO) << this << ": dropping pending peer " << node.adnl_id_short()
                          << " - unsupported protocol version " << version.first << "." << version.second;
      break;
    }
    VLOG(overlay, INFO) << this << ": adding pending peer " << node.adnl_id_short();
    processing_pending_peers_.erase(node.adnl_id_short());
    add_peer(std::move(node), /* verified = */ true, /* checked_signature = */ true);
    co_return {};
  }
  VLOG(overlay, INFO) << this << ": dropping pending node " << node.adnl_id_short() << " - probe failed";
  processing_pending_peers_.erase(node.adnl_id_short());
  co_return {};
}

void OverlayImpl::add_peers(std::vector<OverlayNode> peers, bool verified, bool checked_signature) {
  for (auto &node : peers) {
    add_peer(std::move(node), verified, checked_signature);
  }
}

void OverlayImpl::add_peers(const tl_object_ptr<ton_api::overlay_nodes> &nodes, bool verified, bool checked_signature) {
  for (auto &n : nodes->nodes_) {
    auto N = OverlayNode::create(n);
    if (N.is_ok()) {
      add_peer(N.move_as_ok(), verified, checked_signature);
    }
  }
}

void OverlayImpl::add_peers(const tl_object_ptr<ton_api::overlay_nodesV2> &nodes, bool verified,
                            bool checked_signature) {
  for (auto &n : nodes->nodes_) {
    auto N = OverlayNode::create(n);
    if (N.is_ok()) {
      add_peer(N.move_as_ok(), verified, checked_signature);
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
    if (!p->is_alive() && !p->is_permanent_member()) {
      del_peer(peer);
    }
  }
}

void OverlayImpl::receive_random_peers(adnl::AdnlNodeIdShort src, td::Result<td::BufferSlice> R, double elapsed) {
  CHECK(overlay_type_ != OverlayType::FixedMemberList);
  on_ping_result(src, R.is_ok(), elapsed);
  if (R.is_error()) {
    VLOG(overlay, INFO) << this << ": failed getRandomPeers query: " << R.move_as_error();
    return;
  }
  auto R2 = fetch_tl_object<ton_api::overlay_nodes>(R.move_as_ok(), true);
  if (R2.is_error()) {
    VLOG(overlay, WARNING) << this << ": dropping incorrect answer to overlay.getRandomPeers query from " << src << ": "
                           << R2.move_as_error();
    return;
  }

  add_peers(R2.move_as_ok(), /* verified = */ false);
}

void OverlayImpl::receive_random_peers_v2(adnl::AdnlNodeIdShort src, td::Result<td::BufferSlice> R, double elapsed) {
  CHECK(overlay_type_ != OverlayType::FixedMemberList);
  on_ping_result(src, R.is_ok(), elapsed);
  if (R.is_error()) {
    VLOG(overlay, INFO) << this << ": failed getRandomPeersV2 query: " << R.move_as_error();
    return;
  }
  auto R2 = fetch_tl_object<ton_api::overlay_nodesV2>(R.move_as_ok(), true);
  if (R2.is_error()) {
    VLOG(overlay, WARNING) << this << ": dropping incorrect answer to overlay.getRandomPeers query from " << src << ": "
                           << R2.move_as_error();
    return;
  }

  add_peers(R2.move_as_ok(), /* verified = */ false);
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
                            td::Timestamp::in(5.0 + td::Random::fast(0, 50) * 0.1), serialize_tl_object(Q, true),
                            adnl::Adnl::huge_packet_max_size());
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
    if (overlay_type_ == OverlayType::Public || is_persistent_node(local_id_) || !node.certificate()->empty() ||
        authorized_semiprivate_nodes_.contains(local_id_)) {
      vec.emplace_back(node.tl_v2());
    }
  }

  std::vector<adnl::AdnlNodeIdShort> sent_peers;
  td::uint32 max_iterations = nodes_to_send() + 100;
  for (td::uint32 i = 0; i < max_iterations && vec.size() < nodes_to_send(); i++) {
    auto P = get_random_peer(true);
    if (P) {
      if (P->has_full_id() && !P->is_permanent_member()) {
        if (std::find(sent_peers.begin(), sent_peers.end(), P->get_id()) != sent_peers.end()) {
          continue;
        }
        sent_peers.push_back(P->get_id());
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
                            td::Timestamp::in(5.0 + td::Random::fast(0, 50) * 0.1), serialize_tl_object(Q, true),
                            adnl::Adnl::huge_packet_max_size());
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
  auto peers = get_random_alive_peers(5);
  for (const adnl::AdnlNodeIdShort &peer : peers) {
    auto P = td::PromiseCreator::lambda(
        [SelfId = actor_id(this), peer, timer = td::Timer(), oid = print_id()](td::Result<td::BufferSlice> R) {
          if (R.is_error()) {
            VLOG(overlay, INFO) << oid << " ping to " << peer << " failed : " << R.move_as_error();
            return;
          }
          td::actor::send_closure(SelfId, &OverlayImpl::receive_pong, peer, timer.elapsed());
        });
    td::actor::send_closure(manager_, &OverlayManager::send_query, peer, local_id_, overlay_id_, "overlay ping",
                            std::move(P), td::Timestamp::in(5.0), create_serialize_tl_object<ton_api::overlay_ping>(),
                            1024);
  }
}

void OverlayImpl::receive_pong(adnl::AdnlNodeIdShort peer, double elapsed) {
  on_ping_result(peer, true, elapsed);
}

bool OverlayImpl::should_remove_peer(const OverlayPeer &peer, double now) {
  if (peer.is_permanent_member()) {
    return false;
  }
  if (peer.get_version() + Overlays::overlay_peer_ttl() < now) {
    return true;
  }
  if (overlay_type_ != OverlayType::CertificatedMembers) {
    return false;
  }
  if (authorized_semiprivate_nodes_.contains(peer.get_id())) {
    return false;
  }
  return peer.certificate()->is_expired(now);
}

static constexpr size_t peer_sampling_extra_attempts = 16;

OverlayPeer *OverlayImpl::get_random_peer(bool only_alive) {
  auto now = td::Clocks::system();
  for (size_t i = 0; i < 1 + peer_sampling_extra_attempts; ++i) {
    auto *peer = peer_list_.peers_.get_random();
    if (!peer) {
      break;
    }
    if (should_remove_peer(*peer, now)) {
      auto peer_id = peer->get_id();
      del_peer(peer_id);
    } else if (!only_alive || peer->is_alive()) {
      return peer;
    }
  }
  return nullptr;
}

void OverlayImpl::get_overlay_random_peers(td::uint32 max_peers,
                                           td::Promise<std::vector<adnl::AdnlNodeIdShort>> promise) {
  promise.set_value(get_random_alive_peers(max_peers));
}

void OverlayImpl::receive_nodes_from_db(tl_object_ptr<ton_api::overlay_nodes> tl_nodes) {
  if (overlay_type_ != OverlayType::FixedMemberList) {
    add_peers(tl_nodes, /* verified = */ false);
  }
}

void OverlayImpl::receive_nodes_from_db_v2(tl_object_ptr<ton_api::overlay_nodesV2> tl_nodes) {
  if (overlay_type_ != OverlayType::FixedMemberList) {
    add_peers(tl_nodes, /* verified = */ false);
  }
}

bool OverlayImpl::is_persistent_node(const adnl::AdnlNodeIdShort &id) {
  auto P = peer_list_.peers_.get(id);
  if (!P) {
    return false;
  }
  return P->is_permanent_member();
}

bool OverlayImpl::is_twostep_intermediate_node(const adnl::AdnlNodeIdShort &id) {
  if (opts_.twostep_intermediate_nodes_.empty()) {
    return is_persistent_node(id);
  }
  return opts_.twostep_intermediate_nodes_.contains(id);
}

size_t OverlayImpl::persistent_node_count() {
  return peer_list_.persistent_node_count_;
}

bool OverlayImpl::check_src_peer(const adnl::AdnlNodeIdShort &src,
                                 const ton_api::overlay_MemberCertificate *certificate) {
  if (overlay_type_ == OverlayType::Public) {
    return true;
  }
  if (overlay_type_ == OverlayType::FixedMemberList) {
    return peer_list_.peers_.get(src);
  }
  OverlayMemberCertificate cert(certificate);
  if (cert.empty() && !authorized_semiprivate_nodes_.contains(src)) {
    auto P = peer_list_.peers_.get(src);
    if (P && !P->is_permanent_member()) {
      auto C = P->certificate();
      if (C) {
        cert = *C;
      }
    }
  }

  auto S = validate_peer_certificate(src, cert, true);
  if (S.is_error()) {
    VLOG(overlay, WARNING) << "adnl=" << src << ": certificate is invalid: " << S;
    return false;
  }
  auto P = peer_list_.peers_.get(src);
  if (P && !cert.empty()) {
    P->update_certificate(std::move(cert));
  }
  return true;
}

void OverlayImpl::iterate_all_peers(std::function<void(const adnl::AdnlNodeIdShort &key, OverlayPeer &peer)> cb) {
  peer_list_.peers_.iterate([&](const adnl::AdnlNodeIdShort &key, OverlayPeer &peer) { cb(key, peer); });
}

bool OverlayImpl::peer_receives_plumtree_broadcasts(adnl::AdnlNodeIdShort peer_id) {
  auto *peer = peer_list_.peers_.get(peer_id);
  return peer && !(peer->get_node()->flags() & OverlayMemberFlags::DoNotReceivePlumtreeBroadcasts);
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

bool OverlayImpl::is_root_public_key(const PublicKeyHash &key) const {
  return peer_list_.root_public_keys_.count(key) > 0;
}

std::vector<adnl::AdnlNodeIdShort> OverlayImpl::get_random_alive_peers(td::uint32 max_size) {
  std::vector<adnl::AdnlNodeIdShort> result;
  auto now = td::Clocks::system();
  auto max_iterations = static_cast<size_t>(max_size) + peer_sampling_extra_attempts;
  for (size_t i = 0; i < max_iterations && result.size() < max_size; ++i) {
    auto *peer = peer_list_.peers_.get_random();
    if (!peer) {
      break;
    }
    auto peer_id = peer->get_id();
    if (should_remove_peer(*peer, now)) {
      del_peer(peer_id);
    } else if (peer_id != local_id_ && peer->is_alive() &&
               std::find(result.begin(), result.end(), peer_id) == result.end()) {
      result.push_back(peer_id);
    }
  }
  return result;
}

std::vector<adnl::AdnlNodeIdShort> OverlayImpl::get_broadcast_neighbours(td::uint32 max_size) {
  auto result = get_random_alive_peers(max_size);
  auto now = td::Clocks::system();
  for (const auto &peer_id : broadcasts_plumtree_.get_simple_eager_peers()) {
    auto *peer = peer_list_.peers_.get(peer_id);
    if (peer_id != local_id_ && peer && !should_remove_peer(*peer, now) && peer->is_alive() &&
        std::find(result.begin(), result.end(), peer_id) == result.end()) {
      result.push_back(peer_id);
    }
  }
  return result;
}

void OverlayImpl::update_root_member_list(std::vector<adnl::AdnlNodeIdShort> ids,
                                          std::vector<PublicKeyHash> root_public_keys, OverlayMemberCertificate cert,
                                          OverlayPrivacyRules rules) {
  protected_peers_guard_ = adnl::Adnl::ProtectedPeersGuard{adnl_, local_id_, ids};
  auto expected_size = (td::uint32)(ids.size() + root_public_keys.size() * opts_.max_slaves_in_semiprivate_overlay_);
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
  peer_list_.persistent_node_count_ = ids.size();

  update_member_certificate(std::move(cert));
  set_privacy_rules(std::move(rules));
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
  if (overlay_type_ != OverlayType::CertificatedMembers || authorized_semiprivate_nodes_.contains(local_id_)) {
    return true;
  }

  if (!peer_list_.local_cert_is_valid_until_) {
    return false;
  }

  return !peer_list_.local_cert_is_valid_until_.is_in_past();
}

void OverlayImpl::update_semiprivate_authorized_nodes(std::vector<adnl::AdnlNodeIdShort> add,
                                                      std::vector<adnl::AdnlNodeIdShort> remove) {
  CHECK(overlay_type_ == OverlayType::CertificatedMembers);
  authorized_semiprivate_nodes_.insert(add.begin(), add.end());
  for (auto &id : remove) {
    authorized_semiprivate_nodes_.erase(id);
    if (auto P = peer_list_.peers_.get(id)) {
      if (!P->is_permanent_member() && (P->certificate()->empty() || P->certificate()->is_expired())) {
        del_peer(id);
      }
    }
  }
}

}  // namespace overlay

}  // namespace ton
