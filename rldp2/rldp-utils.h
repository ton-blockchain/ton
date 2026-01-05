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

#include "rldp.h"

namespace ton::rldp2 {

  class PeersMtuLimitGuard {
   public:
    PeersMtuLimitGuard() = default;
    PeersMtuLimitGuard(td::actor::ActorId<Rldp> rldp, adnl::AdnlNodeIdShort local_id,
                       std::vector<adnl::AdnlNodeIdShort> peer_ids, td::uint64 mtu)
        : rldp_(std::move(rldp)), local_id_(local_id), peer_ids_(std::move(peer_ids)), mtu_(mtu) {
      for (const adnl::AdnlNodeIdShort peer_id : peer_ids_) {
        td::actor::send_closure(rldp_, &Rldp::add_peer_mtu_limit, local_id_, peer_id, mtu_);
      }
    }
    PeersMtuLimitGuard(const PeersMtuLimitGuard&) = delete;
    PeersMtuLimitGuard(PeersMtuLimitGuard&& other) noexcept
        : rldp_(std::move(other.rldp_))
        , local_id_(other.local_id_)
        , peer_ids_(std::move(other.peer_ids_))
        , mtu_(other.mtu_) {
      other.rldp_ = {};
    }
    ~PeersMtuLimitGuard() {
      reset();
    }
    PeersMtuLimitGuard& operator=(PeersMtuLimitGuard&& other) noexcept {
      if (this == &other) {
        return *this;
      }
      reset();
      rldp_ = std::move(other.rldp_);
      local_id_ = other.local_id_;
      peer_ids_ = std::move(other.peer_ids_);
      mtu_ = other.mtu_;
      other.rldp_ = {};
      return *this;
    }

   private:
    td::actor::ActorId<Rldp> rldp_;
    adnl::AdnlNodeIdShort local_id_;
    std::vector<adnl::AdnlNodeIdShort> peer_ids_;
    td::uint64 mtu_ = 0;

    void reset() {
      if (rldp_.empty()) {
        return;
      }
      for (const adnl::AdnlNodeIdShort peer_id : peer_ids_) {
        td::actor::send_closure(rldp_, &Rldp::remove_peer_mtu_limit, local_id_, peer_id, mtu_);
      }
    }
  };

}  // namespace ton::rldp2