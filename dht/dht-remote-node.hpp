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

#include "td/utils/int_types.h"
#include "td/actor/actor.h"

#include "adnl/adnl.h"
#include "adnl/utils.hpp"

#include "dht.hpp"

#include "auto/tl/ton_api.hpp"

namespace ton {

namespace dht {

class DhtMember;

class DhtRemoteNode {
 private:
  DhtKeyId id_;
  DhtNode node_;

  td::uint32 max_missed_pings_;
  td::int32 our_network_id_;
  td::uint32 missed_pings_ = 0;
  double last_ping_at_ = 0;
  double ready_from_ = 0;
  double failed_from_ = 0;
  double ping_interval_;
  td::int32 version_;

 public:
  DhtRemoteNode(DhtNode node, td::uint32 max_missed_pings, td::int32 our_network_id);
  static td::Result<std::unique_ptr<DhtRemoteNode>> create(DhtNode node, td::uint32 max_missed_pings,
                                                           td::int32 our_network_id);
  DhtNode get_node() const {
    return node_.clone();
  }
  double failed_from() const {
    return failed_from_;
  }
  adnl::AdnlAddressList get_addr_list() const;
  adnl::AdnlNodeIdFull get_full_id() const;
  DhtKeyId get_key() const {
    return id_;
  }
  td::uint32 missed_pings() const {
    return missed_pings_;
  }
  bool is_ready() const {
    return ready_from_ > 0;
  }
  double ready_from() const {
    return ready_from_;
  }
  double last_ping_at() const {
    return last_ping_at_;
  }
  double ping_interval() const {
    return ping_interval_;
  }
  void send_ping(bool client_only, td::actor::ActorId<adnl::Adnl> adnl, td::actor::ActorId<DhtMember> node,
                 adnl::AdnlNodeIdShort src);
  td::Status receive_ping(DhtNode node, td::actor::ActorId<adnl::Adnl> adnl, adnl::AdnlNodeIdShort self_id);
  void receive_ping();
  td::Status update_value(DhtNode node, td::actor::ActorId<adnl::Adnl> adnl, adnl::AdnlNodeIdShort self_id);
};

}  // namespace dht

}  // namespace ton
