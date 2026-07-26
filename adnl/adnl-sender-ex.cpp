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
#include "adnl-sender-ex.h"

namespace ton::adnl {

void AdnlSenderEx::set_default_mtu(td::uint64 mtu) {
  default_mtu_ = mtu;
  on_mtu_updated({}, {});
}

void AdnlSenderEx::set_local_id_mtu(AdnlNodeIdShort local_id, td::uint64 mtu) {
  auto& s = mtu_local_ids_[local_id];
  s.mtu = mtu;
  if (s.mtu == 0 && s.mtu_peers.empty()) {
    mtu_local_ids_.erase(local_id);
  }
  on_mtu_updated(local_id, {});
}

void AdnlSenderEx::add_peer_mtu(AdnlNodeIdShort local_id, AdnlNodeIdShort peer_id, td::uint64 mtu) {
  mtu_local_ids_[local_id].mtu_peers[peer_id].insert(mtu);
  on_mtu_updated(local_id, peer_id);
}

void AdnlSenderEx::remove_peer_mtu(AdnlNodeIdShort local_id, AdnlNodeIdShort peer_id, td::uint64 mtu) {
  auto it = mtu_local_ids_.find(local_id);
  if (it == mtu_local_ids_.end()) {
    LOG(WARNING) << "Removing nonexistent peer mtu " << local_id << " " << peer_id << " " << mtu;
    return;
  }
  auto it2 = it->second.mtu_peers.find(peer_id);
  if (it2 == it->second.mtu_peers.end()) {
    LOG(WARNING) << "Removing nonexistent peer mtu " << local_id << " " << peer_id << " " << mtu;
    return;
  }
  auto it3 = it2->second.find(mtu);
  if (it3 == it2->second.end()) {
    LOG(WARNING) << "Removing nonexistent peer mtu " << local_id << " " << peer_id << " " << mtu;
    return;
  }
  it2->second.erase(it3);
  if (it2->second.empty()) {
    it->second.mtu_peers.erase(it2);
    if (it->second.mtu_peers.empty() && it->second.mtu == 0) {
      mtu_local_ids_.erase(it);
    }
  }
  on_mtu_updated(local_id, peer_id);
}

td::uint64 AdnlSenderEx::get_peer_mtu(AdnlNodeIdShort local_id, AdnlNodeIdShort peer_id) {
  td::uint64 mtu = default_mtu_;
  auto it = mtu_local_ids_.find(local_id);
  if (it != mtu_local_ids_.end()) {
    mtu = std::max(mtu, it->second.mtu);
    auto it2 = it->second.mtu_peers.find(peer_id);
    if (it2 != it->second.mtu_peers.end()) {
      mtu = std::max(mtu, *it2->second.rbegin());
    }
  }
  return mtu;
}

td::uint64 AdnlSenderEx::get_peer_mtu_inner(AdnlNodeIdShort local_id, AdnlNodeIdShort peer_id) {
  auto it = mtu_local_ids_.find(local_id);
  if (it == mtu_local_ids_.end()) {
    return 0;
  }
  auto it2 = it->second.mtu_peers.find(peer_id);
  return it2 == it->second.mtu_peers.end() ? 0 : *it2->second.rbegin();
}

std::vector<std::pair<AdnlNodeIdShort, td::uint64>> AdnlSenderEx::get_local_id_peers_mtu(AdnlNodeIdShort local_id) {
  std::vector<std::pair<AdnlNodeIdShort, td::uint64>> result;
  auto it = mtu_local_ids_.find(local_id);
  if (it != mtu_local_ids_.end()) {
    result.reserve(it->second.mtu_peers.size());
    for (auto& [peer_id, mtu] : it->second.mtu_peers) {
      result.emplace_back(peer_id, *mtu.rbegin());
    }
  }
  return result;
}

td::uint64 AdnlSenderEx::get_local_id_mtu(AdnlNodeIdShort local_id) {
  td::uint64 mtu = default_mtu_;
  auto it = mtu_local_ids_.find(local_id);
  if (it != mtu_local_ids_.end()) {
    mtu = std::max(mtu, it->second.mtu);
  }
  return mtu;
}

}  // namespace ton::adnl
