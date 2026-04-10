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
#include "adnl.h"
#include "utils.hpp"

namespace ton::adnl {

Adnl::ProtectedPeersGuard::ProtectedPeersGuard(td::actor::ActorId<Adnl> adnl, AdnlNodeIdShort local_id,
                                               std::vector<AdnlNodeIdShort> peer_ids)
    : adnl_(std::move(adnl)), local_id_(local_id), peer_ids_(std::move(peer_ids)) {
  if (!peer_ids_.empty()) {
    td::actor::send_closure(adnl_, &Adnl::add_protected_peers, local_id_, peer_ids_);
  }
}

Adnl::ProtectedPeersGuard::ProtectedPeersGuard(ProtectedPeersGuard &&other) noexcept
    : adnl_(std::move(other.adnl_)), local_id_(other.local_id_), peer_ids_(std::move(other.peer_ids_)) {
  other.adnl_ = {};
}

Adnl::ProtectedPeersGuard::~ProtectedPeersGuard() {
  reset();
}

Adnl::ProtectedPeersGuard &Adnl::ProtectedPeersGuard::operator=(ProtectedPeersGuard &&other) noexcept {
  if (this == &other) {
    return *this;
  }
  reset();
  adnl_ = std::move(other.adnl_);
  local_id_ = other.local_id_;
  peer_ids_ = std::move(other.peer_ids_);
  other.adnl_ = {};
  return *this;
}

void Adnl::ProtectedPeersGuard::reset() {
  if (adnl_.empty() || peer_ids_.empty()) {
    return;
  }
  td::actor::send_closure(adnl_, &Adnl::remove_protected_peers, local_id_, std::move(peer_ids_));
  peer_ids_.clear();
}

}  // namespace ton::adnl
