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

#include <set>

#include "adnl/adnl-node-id.hpp"
#include "auto/tl/ton_api.h"
#include "fec/fec.h"
#include "keys/keys.hpp"
#include "overlay/overlay.h"
#include "td/utils/List.h"

namespace ton {

namespace overlay {

class OverlayImpl;
class BroadcastFec;
class BroadcastFecPart;

class BroadcastsFec {
 public:
  BroadcastsFec();
  ~BroadcastsFec();
  void send(OverlayImpl *overlay, PublicKeyHash send_as, td::BufferSlice data, td::uint32 flags,
            double speed_multiplier);
  void send_part(OverlayImpl *overlay, PublicKeyHash send_as, Overlay::BroadcastDataHash data_hash, td::uint32 size,
                 td::uint32 flags, td::BufferSlice part, td::uint32 seqno, fec::FecType fec_type, td::uint32 date);
  void signed_(OverlayImpl *overlay, std::unique_ptr<BroadcastFecPart> &&part,
               td::Result<std::pair<td::BufferSlice, PublicKey>> &&R);
  td::Status process_broadcast(OverlayImpl *overlay, adnl::AdnlNodeIdShort src_peer_id,
                               tl_object_ptr<ton_api::overlay_broadcastFec> broadcast);
  td::Status process_broadcast(OverlayImpl *overlay, adnl::AdnlNodeIdShort src_peer_id,
                               tl_object_ptr<ton_api::overlay_broadcastFecShort> broadcast);
  void checked(OverlayImpl *overlay, Overlay::BroadcastHash &&hash, td::Result<td::Unit> &&R);
  void gc(OverlayImpl *overlay);

 private:
  td::Status process(OverlayImpl *overlay, BroadcastFecPart &part);

  std::map<Overlay::BroadcastHash, std::unique_ptr<BroadcastFec>> broadcasts_;
  td::ListNode lru_;
};

}  // namespace overlay

}  // namespace ton
