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

#include "adnl/adnl-db.h"
#include "adnl/adnl-query.h"
#include "auto/tl/ton_api.h"
#include "common/io.hpp"
#include "metrics/well-known.h"
#include "td/actor/actor.h"
#include "td/utils/BufferedUdp.h"
#include "td/utils/logging.h"

#include "adnl-packet.h"
#include "adnl.h"
#include "utils.hpp"

DECLARE_LOG_CATEGORY(adnl)

namespace ton {

namespace adnl {

// Metrics a peer-pair accumulates on its own thread and periodically drains into the peer table's
// aggregate (Counter is non-atomic, hence the drain/absorb instead of a cross-thread bump per event):
//   - app: the shared app traffic tier (send + deliver of message/query/answer, plus MTU/size drops).
//   - transport_dropped: transport-tier packet drops detected while (de)processing a peer's packets;
//     merged into the peer table's adnl_transport_dropped_total{direction,reason}.
// The peer table has no field of this type — absorb_metrics splits the two members into their
// places in AdnlPeerTableMetrics — so there is deliberately no operator+= here.
struct AdnlPeerPairMetrics {
  metrics::App app;
  metrics::Labeled<metrics::Counter, metrics::Direction, metrics::Reason> transport_dropped;
};

class AdnlChannelIdShortImpl {
 public:
  explicit AdnlChannelIdShortImpl(PublicKeyHash value) {
    value_ = value.bits256_value();
  }
  explicit AdnlChannelIdShortImpl(td::Bits256 value) {
    value_ = value;
  }
  AdnlChannelIdShortImpl() {
  }
  td::Bits256 bits256_value() const {
    return value_;
  }
  auto tl() const {
    return value_;
  }

  bool operator<(const AdnlChannelIdShortImpl &with) const {
    return value_ < with.value_;
  }
  bool operator==(const AdnlChannelIdShortImpl &with) const {
    return value_ == with.value_;
  }
  bool operator!=(const AdnlChannelIdShortImpl &with) const {
    return value_ != with.value_;
  }
  td::Slice as_slice() const {
    return td::as_slice(value_);
  }

 private:
  td::Bits256 value_;
};

using AdnlChannelIdShort = AdnlChannelIdShortImpl;

class AdnlLocalId;
class AdnlChannel;

class AdnlPeerTable : public Adnl {
 public:
  static constexpr double republish_addr_list_timeout() {
    return 60.0;
  }

  virtual void answer_query(AdnlNodeIdShort src, AdnlNodeIdShort dst, AdnlQueryId query_id, td::BufferSlice data) = 0;

  virtual void receive_packet(td::IPAddress addr, AdnlCategoryMask cat_mask, td::BufferSlice data) = 0;
  virtual void receive_decrypted_packet(AdnlNodeIdShort dst, AdnlPacket packet, td::uint64 serialized_size) = 0;
  virtual void send_message_in(AdnlNodeIdShort src, AdnlNodeIdShort dst, AdnlMessage message, td::uint32 flags) = 0;

  virtual void register_channel(AdnlChannelIdShort id, AdnlNodeIdShort local_id,
                                td::actor::ActorId<AdnlChannel> channel) = 0;
  virtual void unregister_channel(AdnlChannelIdShort id) = 0;

  virtual td::Result<AdnlNode> get_static_node(AdnlNodeIdShort id) = 0;

  virtual void write_new_addr_list_to_db(AdnlNodeIdShort local_id, AdnlNodeIdShort peer_id, AdnlDbItem node,
                                         td::Promise<td::Unit> promise) = 0;
  virtual void get_addr_list_from_db(AdnlNodeIdShort local_id, AdnlNodeIdShort peer_id,
                                     td::Promise<AdnlDbItem> promise) = 0;

  virtual void deliver(AdnlNodeIdShort src, AdnlNodeIdShort dst, td::BufferSlice data) = 0;
  virtual void deliver_query(AdnlNodeIdShort src, AdnlNodeIdShort dst, td::BufferSlice data,
                             td::Promise<td::BufferSlice> promise) = 0;
  virtual void decrypt_message(AdnlNodeIdShort dst, td::BufferSlice data, td::Promise<td::BufferSlice> promise) = 0;

  virtual void set_peer_pair_idle(AdnlNodeIdShort l_id, AdnlNodeIdShort p_id, bool value) = 0;

  // Merge a peer-pair's drained metrics delta into the peer table's aggregate (Counter is non-atomic,
  // so it must be accumulated on the peer-table thread). `done` is fulfilled after the merge. Called
  // both from the collect() drain round-trip and from a peer-pair's tear_down.
  virtual void absorb_metrics(AdnlPeerPairMetrics delta, td::Promise<td::Unit> done) = 0;

  // How long a query delivered to a local id took to be answered. Called from the query's completion,
  // which may run on any thread — hence an actor message rather than a direct bump.
  virtual void record_query_duration(AdnlNodeIdShort src, td::int32 magic, double seconds, bool ok) = 0;

  // Round trip of an outbound query: from the peer pair accepting it to the caller's promise being
  // fulfilled. Completes on the query's own actor, so it takes the same hop.
  virtual void record_query_roundtrip(AdnlNodeIdShort dst, td::int32 magic, double seconds, bool ok) = 0;
};

}  // namespace adnl

}  // namespace ton

namespace td {

inline td::StringBuilder &operator<<(td::StringBuilder &stream, const ton::adnl::AdnlChannelIdShort &value) {
  return stream << value.bits256_value();
}

}  // namespace td
