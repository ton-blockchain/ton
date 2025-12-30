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

#include <map>
#include <memory>
#include <utility>

#include "adnl/adnl-node-id.hpp"
#include "auto/tl/ton_api.h"
#include "keys/keys.hpp"
#include "td/utils/List.h"
#include "td/utils/Status.h"
#include "td/utils/buffer.h"
#include "td/utils/common.h"
#include "td/utils/int_types.h"

#include "overlay.h"

namespace ton {

namespace overlay {

class OverlayImpl;
struct BroadcastTwostep;
struct BroadcastTwostepDataSimple;
struct BroadcastTwostepDataFec;

class BroadcastsTwostep {
 public:
  BroadcastsTwostep();
  ~BroadcastsTwostep();
  void send(OverlayImpl *overlay, PublicKeyHash send_as, td::BufferSlice data, td::uint32 flags);
  void signed_simple(OverlayImpl *overlay, BroadcastTwostepDataSimple &&data,
                     td::Result<std::pair<td::BufferSlice, PublicKey>> &&R);
  void signed_fec(OverlayImpl *overlay, BroadcastTwostepDataFec &&data,
                  td::Result<std::pair<td::BufferSlice, PublicKey>> &&R);
  td::Status process_broadcast(OverlayImpl *overlay, adnl::AdnlNodeIdShort src_peer_id,
                               tl_object_ptr<ton_api::overlay_broadcastTwostepSimple> broadcast);
  td::Status process_broadcast(OverlayImpl *overlay, adnl::AdnlNodeIdShort src_peer_id,
                               tl_object_ptr<ton_api::overlay_broadcastTwostepFec> broadcast);
  void checked(OverlayImpl *overlay, PublicKeyHash &&src, td::BufferSlice &&data, td::Result<td::Unit> &&R);
  void gc(OverlayImpl *overlay);

  void init_sender(td::actor::ActorId<adnl::AdnlSenderInterface> sender) {
    sender_ = std::move(sender);
  }

 private:
  td::actor::ActorId<adnl::AdnlSenderInterface> sender_;
  std::map<Overlay::BroadcastHash, std::unique_ptr<BroadcastTwostep>> broadcasts_;
  td::ListNode lru_;

  void rebroadcast(OverlayImpl *overlay, const adnl::AdnlNodeIdShort &bcast_src_adnl_id, const td::BufferSlice &data);
};
}  // namespace overlay

}  // namespace ton
