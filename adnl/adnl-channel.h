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

#include "adnl-local-id.h"
#include "adnl-peer.h"
#include "adnl-peer-table.h"
#include "adnl-network-manager.h"

namespace ton {

namespace adnl {

class AdnlPeerPair;

class AdnlChannel : public td::actor::Actor {
 public:
  static td::Result<td::actor::ActorOwn<AdnlChannel>> create(privkeys::Ed25519 pk, pubkeys::Ed25519 pub,
                                                             AdnlNodeIdShort local_id, AdnlNodeIdShort peer_id,
                                                             AdnlChannelIdShort &out_id, AdnlChannelIdShort &in_id,
                                                             td::actor::ActorId<AdnlPeerPair> peer_pair);
  virtual void receive(td::IPAddress addr, td::BufferSlice data) = 0;
  virtual void send_message(td::uint32 priority, td::actor::ActorId<AdnlNetworkConnection> conn,
                            td::BufferSlice data) = 0;
  virtual ~AdnlChannel() = default;
};

}  // namespace adnl

}  // namespace ton
