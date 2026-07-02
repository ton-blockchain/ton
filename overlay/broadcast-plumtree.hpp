/*
    This file is part of TON Blockchain Library.

    TON Blockchain Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    TON Blockchain Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
    or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
    License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with TON Blockchain Library.  If not, see <http://www.gnu.org/licenses/>.

    Copyright 2017-2020 Telegram Systems LLP
*/
#pragma once

#include <memory>
#include <utility>
#include <vector>

#include "adnl/adnl-node-id.hpp"
#include "auto/tl/ton_api.h"
#include "keys/keys.hpp"
#include "td/actor/actor.h"
#include "td/utils/Status.h"
#include "td/utils/Time.h"
#include "td/utils/buffer.h"
#include "td/utils/int_types.h"

#include "overlay.h"
#include "overlays.h"

namespace ton {

namespace overlay {

class OverlayImpl;

struct PlumtreeOutboundFecPayload {
  td::Bits256 broadcast_id;
  td::uint32 flags = 0;
  double timestamp = 0.0;
  PublicKeyHash source;
  td::Bits256 full_data_hash;
  td::uint32 full_data_size = 0;
  td::uint32 part_index = 0;
  td::uint32 tree_index = 0;
  // Bytes covered by data_hash/signature: one FEC symbol.
  td::uint32 data_size = 0;
  td::Bits256 data_hash;
  td::BufferSlice data;
};

struct PlumtreeOutboundSimplePayload {
  td::Bits256 broadcast_id;
  td::uint32 flags = 0;
  double timestamp = 0.0;
  PublicKeyHash source;
  td::uint32 tree_index = 0;
  td::uint32 data_size = 0;
  td::Bits256 data_hash;
  td::BufferSlice data;
};

class BroadcastsPlumtree {
 public:
  explicit BroadcastsPlumtree(PlumtreeFecOptions options = {}, bool is_original_sender = false);
  ~BroadcastsPlumtree();

  void init_sender(td::actor::ActorId<adnl::AdnlSenderInterface> sender);

  void send_fec(OverlayImpl *overlay, PublicKeyHash send_as, td::uint32 flags, td::BufferSlice data);
  void send(OverlayImpl *overlay, PublicKeyHash send_as, td::uint32 flags, td::Bits256 broadcast_id,
            td::BufferSlice data);
  void signed_fec(OverlayImpl *overlay, PlumtreeOutboundFecPayload &&payload,
                  td::Result<std::pair<td::BufferSlice, PublicKey>> &&R);
  void signed_simple(OverlayImpl *overlay, PlumtreeOutboundSimplePayload &&payload,
                     td::Result<std::pair<td::BufferSlice, PublicKey>> &&R);

  td::actor::Task<> process_fec_payload(OverlayImpl *overlay, adnl::AdnlNodeIdShort from,
                                        tl_object_ptr<ton_api::overlay_broadcastPlumtreeFec> msg);
  td::actor::Task<> process_simple_payload(OverlayImpl *overlay, adnl::AdnlNodeIdShort from,
                                           tl_object_ptr<ton_api::overlay_broadcastPlumtreeSimple> msg);
  td::actor::Task<> process_ihave(OverlayImpl *overlay, adnl::AdnlNodeIdShort from,
                                  tl_object_ptr<ton_api::overlay_broadcastPlumtreeIHave> msg);
  void process_repair_query(OverlayImpl *overlay, adnl::AdnlNodeIdShort from,
                            ton_api::overlay_repairPlumtreePart &query, td::Promise<td::BufferSlice> promise);
  void repair_query_finished();
  td::actor::Task<> process_repair_response(OverlayImpl *overlay, adnl::AdnlNodeIdShort from,
                                            const td::Bits256 &expected_broadcast_id, td::uint32 expected_part_index,
                                            td::uint32 expected_tree_index, td::BufferSlice data);
  td::actor::Task<> process_prune(OverlayImpl *overlay, adnl::AdnlNodeIdShort from,
                                  tl_object_ptr<ton_api::overlay_broadcastPlumtreePrune> msg);
  td::actor::Task<> process_useful(OverlayImpl *overlay, adnl::AdnlNodeIdShort from,
                                   tl_object_ptr<ton_api::overlay_broadcastPlumtreeUseful> msg);

  void alarm(OverlayImpl *overlay);
  td::Timestamp next_alarm_at();
  void gc(OverlayImpl *overlay);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace overlay

}  // namespace ton
