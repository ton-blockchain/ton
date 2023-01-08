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
#include "overlay.hpp"

namespace ton {

namespace overlay {

void OverlayImpl::del_peer(adnl::AdnlNodeIdShort id) {
  auto P = peers_.get(id);
  CHECK(P != nullptr);

  VLOG(OVERLAY_DEBUG) << this << ": deleting peer " << id;

  if (P->is_neighbour()) {
    VLOG(OVERLAY_INFO) << this << ": deleting neighbour " << id;
    bool deleted = false;
    for (auto &n : neighbours_) {
      if (n == id) {
        n = neighbours_[neighbours_.size() - 1];
        neighbours_.resize(neighbours_.size() - 1);
        deleted = true;
        break;
      }
    }
    CHECK(deleted);
    P->set_neighbour(false);
  }
  peers_.remove(id);
  bad_peers_.erase(id);
  update_neighbours(0);
}

void OverlayImpl::del_some_peers() {
  if (!public_) {
    return;
  }
  while (peers_.size() > max_peers()) {
    OverlayPeer *P;
    if (bad_peers_.empty()) {
      P = get_random_peer();
    } else {
      auto it = bad_peers_.upper_bound(next_bad_peer_);
      if (it == bad_peers_.end()) {
        it = bad_peers_.begin();
      }
      P = peers_.get(next_bad_peer_ = *it);
    }
    if (P) {
      auto id = P->get_id();
      del_peer(id);
    }
  }
}

void OverlayImpl::do_add_peer(OverlayNode node) {
  auto id = node.adnl_id_short();

  auto V = peers_.get(id);
  if (V) {
    VLOG(OVERLAY_DEBUG) << this << ": updating peer " << id << " up to version " << node.version();
    V->update(std::move(node));
  } else {
    VLOG(OVERLAY_DEBUG) << this << ": adding peer " << id << " of version " << node.version();
    peers_.insert(id, OverlayPeer(std::move(node)));

    del_some_peers();
    update_neighbours(0);
  }
}

void OverlayImpl::add_peer_in_cont(OverlayNode node) {
  CHECK(public_);

  do_add_peer(std::move(node));
}

void OverlayImpl::add_peer_in(OverlayNode node) {
  CHECK(public_);
  if (node.overlay_id() != overlay_id_) {
    VLOG(OVERLAY_WARNING) << this << ": received node with bad overlay";
    return;
  }
  auto t = td::Clocks::system();
  if (node.version() + 600 < t || node.version() > t + 60) {
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

  add_peer_in_cont(std::move(node));
}

void OverlayImpl::add_peers(std::vector<OverlayNode> peers) {
  for (auto &node : peers) {
    add_peer_in(std::move(node));
  }
}

void OverlayImpl::add_peer(OverlayNode P) {
  add_peer_in(std::move(P));
}

void OverlayImpl::on_ping_result(adnl::AdnlNodeIdShort peer, bool success) {
  if (!public_) {
    return;
  }
  if (OverlayPeer *p = peers_.get(peer)) {
    p->on_ping_result(success);
    if (p->is_alive()) {
      bad_peers_.erase(peer);
    } else {
      bad_peers_.insert(peer);
    }
  }
}

void OverlayImpl::receive_random_peers(adnl::AdnlNodeIdShort src, td::Result<td::BufferSlice> R) {
  CHECK(public_);
  on_ping_result(src, R.is_ok());
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

  auto res = R2.move_as_ok();

  std::vector<OverlayNode> nodes;
  for (auto &n : res->nodes_) {
    auto N = OverlayNode::create(n);
    if (N.is_ok()) {
      nodes.emplace_back(N.move_as_ok());
    }
  }
  add_peers(std::move(nodes));
}

void OverlayImpl::send_random_peers_cont(adnl::AdnlNodeIdShort src, OverlayNode node,
                                         td::Promise<td::BufferSlice> promise) {
  std::vector<tl_object_ptr<ton_api::overlay_node>> vec;
  if (announce_self_) {
    vec.emplace_back(node.tl());
  }

  for (td::uint32 i = 0; i < nodes_to_send(); i++) {
    auto P = get_random_peer(true);
    if (P) {
      vec.emplace_back(P->get().tl());
    } else {
      break;
    }
  }

  if (promise) {
    auto Q = create_tl_object<ton_api::overlay_nodes>(std::move(vec));
    promise.set_value(serialize_tl_object(Q, true));
  } else {
    auto P =
        td::PromiseCreator::lambda([SelfId = actor_id(this), src, oid = print_id()](td::Result<td::BufferSlice> res) {
          td::actor::send_closure(SelfId, &OverlayImpl::receive_random_peers, src, std::move(res));
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

void OverlayImpl::update_neighbours(td::uint32 nodes_to_change) {
  if (peers_.size() == 0) {
    return;
  }
  td::uint32 iter = 0;
  while (iter < 10 && (nodes_to_change > 0 || neighbours_.size() < max_neighbours())) {
    auto X = peers_.get_random();
    if (!X) {
      break;
    }

    if (X->get_id() == local_id_) {
      iter++;
      continue;
    }

    if (X->get_version() <= td::Clocks::system() - 600) {
      if (X->is_neighbour()) {
        bool found = false;
        for (auto &n : neighbours_) {
          if (n == X->get_id()) {
            n = *neighbours_.rbegin();
            found = true;
            break;
          }
        }
        CHECK(found);
        neighbours_.pop_back();
        X->set_neighbour(false);
      }
      bad_peers_.erase(X->get_id());
      peers_.remove(X->get_id());
      continue;
    }

    if (X->is_neighbour()) {
      iter++;
      continue;
    }

    if (neighbours_.size() < max_neighbours()) {
      VLOG(OVERLAY_INFO) << this << ": adding new neighbour " << X->get_id();
      neighbours_.push_back(X->get_id());
      X->set_neighbour(true);
    } else {
      CHECK(nodes_to_change > 0);
      auto i = td::Random::fast(0, static_cast<td::uint32>(neighbours_.size()) - 1);
      auto Y = peers_.get(neighbours_[i]);
      CHECK(Y != nullptr);
      CHECK(Y->is_neighbour());
      Y->set_neighbour(false);
      neighbours_[i] = X->get_id();
      X->set_neighbour(true);
      nodes_to_change--;
      VLOG(OVERLAY_INFO) << this << ": changing neighbour " << Y->get_id() << " -> " << X->get_id();
    }
  }
}

OverlayPeer *OverlayImpl::get_random_peer(bool only_alive) {
  size_t skip_bad = 3;
  while (peers_.size() > (only_alive ? bad_peers_.size() : 0)) {
    auto P = peers_.get_random();
    if (public_ && P->get_version() + 3600 < td::Clocks::system()) {
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
    return P;
  }
  return nullptr;
}

void OverlayImpl::get_overlay_random_peers(td::uint32 max_peers,
                                           td::Promise<std::vector<adnl::AdnlNodeIdShort>> promise) {
  std::vector<adnl::AdnlNodeIdShort> v;
  auto t = td::Clocks::system();
  while (v.size() < max_peers && v.size() < peers_.size() - bad_peers_.size()) {
    auto P = peers_.get_random();
    if (P->get_version() + 3600 < t) {
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
  promise.set_result(std::move(v));
}

void OverlayImpl::receive_nodes_from_db(tl_object_ptr<ton_api::overlay_nodes> tl_nodes) {
  if (public_) {
    std::vector<OverlayNode> nodes;
    for (auto &n : tl_nodes->nodes_) {
      auto N = OverlayNode::create(n);
      if (N.is_ok()) {
        nodes.emplace_back(N.move_as_ok());
      }
    }
    add_peers(std::move(nodes));
  }
}

}  // namespace overlay

}  // namespace ton
