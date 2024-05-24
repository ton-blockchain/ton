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

#include "dht-remote-node.hpp"
#include "dht.hpp"

#include <map>

namespace ton {

namespace dht {

class DhtMember;

class DhtBucket {
 private:
  td::uint32 max_missed_pings_ = 3;

  std::vector<std::unique_ptr<DhtRemoteNode>> active_nodes_;
  std::vector<std::unique_ptr<DhtRemoteNode>> backup_nodes_;

  //std::map<td::UInt256, std::unique_ptr<DhtRemoteNode>> pending_nodes_;
  td::uint32 k_;
  //bool check_one(td::actor::ActorId<adnl::Adnl> adnl, td::actor::ActorId<DhtMember> node, adnl::AdnlNodeIdShort src,
  //               const DhtMember::PrintId &print_id);
  void demote_node(size_t idx);
  void promote_node(size_t idx);
  size_t select_backup_node_to_drop() const;

 public:
  DhtBucket(td::uint32 k) : k_(k) {
    active_nodes_.resize(k);
    backup_nodes_.resize(k);
  }
  td::uint32 active_cnt();
  td::Status add_full_node(DhtKeyId id, DhtNode node, td::actor::ActorId<adnl::Adnl> adnl,
                           adnl::AdnlNodeIdShort self_id, td::int32 our_network_id, bool set_active = false);
  void check(bool client_only, td::actor::ActorId<adnl::Adnl> adnl, td::actor::ActorId<DhtMember> node,
             adnl::AdnlNodeIdShort src);
  void receive_ping(DhtKeyId id, DhtNode result, td::actor::ActorId<adnl::Adnl> adnl, adnl::AdnlNodeIdShort self_id);
  void get_nearest_nodes(DhtKeyId id, td::uint32 bit, DhtNodesList &vec, td::uint32 k);
  void dump(td::StringBuilder &sb) const;
  DhtNodesList export_nodes() const;
};

}  // namespace dht

}  // namespace ton
