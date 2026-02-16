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
*/
#pragma once

#include "adnl.h"

namespace ton::adnl {

class AdnlSenderEx : public AdnlSenderInterface {
 public:
  AdnlSenderEx() = default;
  explicit AdnlSenderEx(td::uint64 default_mtu) : default_mtu_(default_mtu) {
  }

  virtual void add_id(AdnlNodeIdShort local_id) = 0;

  // MTU for incoming messages in peer pair (local_id, peer_id) is max of:
  // - default mtu
  // - local id mtu of local_id
  // - max peer mtu of (local_id, peer_id)
  // MTU = 0 means that incoming connections from this peer are not accepted
  // Use PeersMtuGuard instead of calling add_peer_mtu/remove_peer_mtu directly
  void set_default_mtu(td::uint64 mtu);
  void set_local_id_mtu(AdnlNodeIdShort local_id, td::uint64 mtu);
  void add_peer_mtu(AdnlNodeIdShort local_id, AdnlNodeIdShort peer_id, td::uint64 mtu);
  void remove_peer_mtu(AdnlNodeIdShort local_id, AdnlNodeIdShort peer_id, td::uint64 mtu);

 protected:
  // Called after changing mtu through methods above
  // No local_id or peer_id means all local ids/peer ids
  // Use get_peer_mtu to get mtu value
  // If peer_id is present, local_id is guaranteed to be present
  virtual void on_mtu_updated(td::optional<AdnlNodeIdShort> local_id, td::optional<AdnlNodeIdShort> peer_id) = 0;

  td::uint64 get_peer_mtu(AdnlNodeIdShort local_id, AdnlNodeIdShort peer_id);

 private:
  td::uint64 default_mtu_ = Adnl::get_mtu();

  struct LocalIdMtu {
    td::uint64 mtu;
    std::map<AdnlNodeIdShort, std::multiset<td::uint64>> mtu_peers;
  };
  std::map<AdnlNodeIdShort, LocalIdMtu> mtu_local_ids_;
};

class PeersMtuGuard {
 public:
  PeersMtuGuard() = default;
  PeersMtuGuard(td::actor::ActorId<AdnlSenderEx> sender, AdnlNodeIdShort local_id,
                std::vector<AdnlNodeIdShort> peer_ids, td::uint64 mtu)
      : sender_(std::move(sender)), local_id_(local_id), peer_ids_(std::move(peer_ids)), mtu_(mtu) {
    for (const AdnlNodeIdShort peer_id : peer_ids_) {
      td::actor::send_closure(sender_, &AdnlSenderEx::add_peer_mtu, local_id_, peer_id, mtu_);
    }
  }
  PeersMtuGuard(const PeersMtuGuard&) = delete;
  PeersMtuGuard(PeersMtuGuard&& other) noexcept
      : sender_(std::move(other.sender_))
      , local_id_(other.local_id_)
      , peer_ids_(std::move(other.peer_ids_))
      , mtu_(other.mtu_) {
    other.sender_ = {};
  }
  ~PeersMtuGuard() {
    reset();
  }
  PeersMtuGuard& operator=(const PeersMtuGuard& other) = delete;
  PeersMtuGuard& operator=(PeersMtuGuard&& other) noexcept {
    if (this == &other) {
      return *this;
    }
    reset();
    sender_ = std::move(other.sender_);
    local_id_ = other.local_id_;
    peer_ids_ = std::move(other.peer_ids_);
    mtu_ = other.mtu_;
    other.sender_ = {};
    return *this;
  }

 private:
  td::actor::ActorId<AdnlSenderEx> sender_;
  AdnlNodeIdShort local_id_;
  std::vector<AdnlNodeIdShort> peer_ids_;
  td::uint64 mtu_ = 0;

  void reset() {
    if (sender_.empty()) {
      return;
    }
    for (const AdnlNodeIdShort peer_id : peer_ids_) {
      td::actor::send_closure(sender_, &AdnlSenderEx::remove_peer_mtu, local_id_, peer_id, mtu_);
    }
    peer_ids_.clear();
  }
};

}  // namespace ton::adnl
