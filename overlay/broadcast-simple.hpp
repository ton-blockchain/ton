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

#include <memory>
#include <utility>

#include "adnl/adnl-node-id.hpp"
#include "auto/tl/ton_api.h"
#include "keys/keys.hpp"
#include "overlay/overlay.h"
#include "td/actor/PromiseFuture.h"
#include "td/utils/List.h"
#include "td/utils/Status.h"
#include "td/utils/buffer.h"
#include "td/utils/common.h"

namespace ton {

namespace overlay {

class OverlayImpl;
class BroadcastSimple;

class BroadcastsSimple {
 public:
  BroadcastsSimple();
  ~BroadcastsSimple();
  void send(OverlayImpl *overlay, PublicKeyHash send_as, td::BufferSlice data, td::uint32 flags);
  void signed_(OverlayImpl *overlay, std::unique_ptr<BroadcastSimple> &&bcast,
               td::Result<std::pair<td::BufferSlice, PublicKey>> &&R);
  td::Status process_broadcast(OverlayImpl *overlay, adnl::AdnlNodeIdShort src_peer_id,
                               tl_object_ptr<ton_api::overlay_broadcast> broadcast);
  void process_query(adnl::AdnlNodeIdShort src, ton_api::overlay_getBroadcast &query,
                     td::Promise<td::BufferSlice> promise);
  void checked(OverlayImpl *overlay, Overlay::BroadcastHash &&hash, td::Result<td::Unit> &&R);
  void gc(OverlayImpl *overlay);

 private:
  bool has(const Overlay::BroadcastHash &hash);
  void register_(OverlayImpl *overlay, std::unique_ptr<BroadcastSimple> bcast);

  std::map<Overlay::BroadcastHash, std::unique_ptr<BroadcastSimple>> broadcasts_;
  td::ListNode lru_;
};

}  // namespace overlay

}  // namespace ton
