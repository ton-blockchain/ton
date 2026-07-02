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

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

#include "adnl/adnl-node-id.hpp"
#include "auto/tl/ton_api.h"
#include "keys/keys.hpp"
#include "td/fec/raptorq/Decoder.h"
#include "td/fec/raptorq/Encoder.h"
#include "td/utils/List.h"
#include "td/utils/Random.h"
#include "td/utils/Status.h"
#include "td/utils/buffer.h"
#include "td/utils/common.h"
#include "td/utils/port/Clocks.h"
#include "td/utils/tl_helpers.h"
#include "tl-utils/common-utils.hpp"

#include "broadcast-plumtree.hpp"
#include "overlay.hpp"

namespace ton {

namespace overlay {

constexpr int VERBOSITY_NAME(PLUMTREE_WARNING) = verbosity_WARNING;
constexpr int VERBOSITY_NAME(PLUMTREE_INFO) = verbosity_DEBUG;

namespace {

constexpr double PLUMTREE_BROADCAST_TTL = 25.0;
constexpr double PLUMTREE_PENDING_FEEDBACK_TTL = 5.0;
constexpr double PLUMTREE_EAGER_PEER_INACTIVITY_TTL = 30.0;
constexpr td::uint32 PLUMTREE_EAGER_PEER_MAX_SENT_WITHOUT_ACTIVITY = 50;
constexpr std::size_t MAX_PENDING_REPAIR_PARTS = 1024;
constexpr std::size_t MAX_ACTIVE_REPAIR_QUERIES = 512;
constexpr td::uint32 PLUMTREE_SIMPLE_TREE_INDEX = 0;
constexpr td::uint32 PLUMTREE_FEC_TREE_OFFSET = 1;

struct PlumtreeSlot {
  std::set<adnl::AdnlNodeIdShort> eager;
  std::map<adnl::AdnlNodeIdShort, td::Timestamp> pending_feedback;
};

struct PlumtreeEagerPeerActivity {
  td::Timestamp last_active_at = td::Timestamp::now();
  td::uint32 sent_since_active = 0;
};

struct PlumtreePartState {
  td::uint32 part_index = 0;
  td::uint32 tree_index = 0;
  double timestamp = 0.0;
  // Bytes covered by data_hash/signature: full simple payload or one FEC symbol.
  td::uint32 data_size = 0;
  td::Bits256 data_hash;
  PublicKey source_key;
  PublicKeyHash source;
  std::shared_ptr<Certificate> certificate;
  td::BufferSlice signature;
  td::uint32 full_sends = 0;
  std::set<adnl::AdnlNodeIdShort> advertised_to;
  std::set<adnl::AdnlNodeIdShort> full_sent_to;
};

using TreePartKey = std::pair<td::uint32, td::uint32>;                   // part_index, tree_index
using MissingPartKey = std::tuple<td::Bits256, td::uint32, td::uint32>;  // broadcast_id, part_index, tree_index

struct PlumtreeFecBroadcastState : td::ListNode {
  td::Bits256 broadcast_id;
  PublicKeyHash first_valid_source;
  BroadcastCheckResult check_result = BroadcastCheckResult::Allowed;
  td::Bits256 full_data_hash;
  td::Timestamp first_seen_at;
  td::uint32 flags = 0;
  // Full decoded broadcast size; part_size is one FEC symbol size.
  td::uint32 full_data_size = 0;
  td::uint32 part_size = 0;
  bool delivered = false;
  // Stop local decode retries after poisoned input, but keep transport state for forwarding/repair.
  bool decode_failed = false;

  std::unique_ptr<td::raptorq::Decoder> decoder;
  std::set<td::uint32> decoder_parts;
  bool overlay_limiter_registered = false;
  std::map<td::uint32, td::BufferSlice> parts_by_index;
  std::map<TreePartKey, PlumtreePartState> tree_parts;
};

struct PlumtreeSimpleBroadcastState : td::ListNode {
  td::Bits256 broadcast_id;
  td::uint32 flags = 0;
  td::BufferSlice data;
  td::Timestamp first_seen_at;
  PlumtreePartState part;
};

struct PlumtreeMissingPart : td::ListNode {
  explicit PlumtreeMissingPart(MissingPartKey key) : key(std::move(key)) {
  }

  static PlumtreeMissingPart *from_list_node(td::ListNode *node) {
    return static_cast<PlumtreeMissingPart *>(node);
  }

  MissingPartKey key;
  // Maximum verified IHAVE byte size, used as repair query MTU cap.
  td::uint32 data_size = 0;
  std::vector<adnl::AdnlNodeIdShort> repair_targets;
  std::size_t sent_repair_targets = 0;
  td::Timestamp repair_at = td::Timestamp::never();
};

struct PlumtreeControlFields {
  td::Bits256 broadcast_id;
  double timestamp = 0.0;
  td::uint32 part_index = 0;
  td::uint32 tree_index = 0;
};

struct PlumtreeDecodedBroadcast {
  PublicKeyHash source;
  BroadcastCheckResult check_result = BroadcastCheckResult::Allowed;
  td::BufferSlice data;
};

struct PlumtreeFecBroadcastRef {
  PlumtreeFecBroadcastState *state = nullptr;
  bool created = false;
};

enum class PlumtreePayloadOrigin { Push, RepairResponse };

template <class T>
PlumtreeControlFields get_control_fields(const T &msg) {
  return PlumtreeControlFields{msg.broadcast_id_, msg.timestamp_, static_cast<td::uint32>(msg.part_index_),
                               static_cast<td::uint32>(msg.tree_index_)};
}

td::Bits256 compute_broadcast_id(td::uint32 flags, const td::Bits256 &full_data_hash, td::uint32 full_data_size,
                                 td::uint32 part_size) {
  return get_tl_object_sha_bits256(create_tl_object<ton_api::overlay_broadcastPlumtreeFec_id>(
      static_cast<std::int32_t>(flags), full_data_hash, static_cast<std::int32_t>(full_data_size),
      static_cast<std::int32_t>(part_size)));
}

td::BufferSlice make_fec_payload_to_sign(const td::Bits256 &broadcast_id, double timestamp, td::uint32 part_index,
                                         td::uint32 tree_index, td::uint32 data_size, const td::Bits256 &data_hash) {
  return create_serialize_tl_object<ton_api::overlay_broadcastPlumtreeFec_toSign>(
      broadcast_id, timestamp, static_cast<std::int32_t>(part_index), static_cast<std::int32_t>(tree_index),
      static_cast<std::int32_t>(data_size), data_hash);
}

td::BufferSlice make_simple_payload_to_sign(const td::Bits256 &broadcast_id, double timestamp, td::uint32 tree_index,
                                            td::uint32 data_size, const td::Bits256 &data_hash) {
  return create_serialize_tl_object<ton_api::overlay_broadcastPlumtreeSimple_toSign>(
      broadcast_id, timestamp, static_cast<std::int32_t>(tree_index), static_cast<std::int32_t>(data_size), data_hash);
}

constexpr td::uint64 PLUMTREE_PAYLOAD_MTU_OVERHEAD = 4096;

td::Status check_timestamp(double timestamp) {
  if (!std::isfinite(timestamp)) {
    return td::Status::Error(ErrorCode::protoviolation, "invalid Plumtree timestamp");
  }
  auto now = td::Clocks::system();
  if (timestamp < now - 20.0) {
    return td::Status::Error(ErrorCode::notready, "too old Plumtree broadcast");
  }
  if (timestamp > now + 20.0) {
    return td::Status::Error(ErrorCode::notready, "too new Plumtree broadcast");
  }
  return td::Status::OK();
}

td::actor::Task<> check_and_deliver(OverlayImpl *overlay, PublicKeyHash source, BroadcastCheckResult check_result,
                                    td::BufferSlice data) {
  if (check_result != BroadcastCheckResult::Allowed) {
    auto [task, promise] = td::actor::StartedTask<>::make_bridge();
    overlay->check_broadcast(source, data.clone(), std::move(promise));
    co_await std::move(task);
  }
  overlay->deliver_broadcast(source, std::move(data), {});
  co_return td::Unit{};
}

}  // namespace

class BroadcastsPlumtree::Impl {
 public:
  explicit Impl(PlumtreeFecOptions options, bool is_original_sender)
      : options_(options)
      , is_original_sender_(is_original_sender)
      , local_eager_limit_(is_original_sender_ ? options_.validator_eager_limit_ : options_.eager_limit_) {
    slots_.resize(options_.tree_slots_);
  }

  void init_sender(td::actor::ActorId<adnl::AdnlSenderInterface> sender) {
    sender_ = sender;
  }

  void send_fec(OverlayImpl *overlay, PublicKeyHash send_as, td::uint32 flags, td::BufferSlice data);
  void send(OverlayImpl *overlay, PublicKeyHash send_as, td::uint32 flags, td::Bits256 broadcast_id,
            td::BufferSlice data);
  void signed_fec(OverlayImpl *overlay, PlumtreeOutboundFecPayload &&payload,
                  td::Result<std::pair<td::BufferSlice, PublicKey>> &&R);
  void signed_simple(OverlayImpl *overlay, PlumtreeOutboundSimplePayload &&payload,
                     td::Result<std::pair<td::BufferSlice, PublicKey>> &&R);

  td::actor::Task<> process_fec_payload(OverlayImpl *overlay, adnl::AdnlNodeIdShort from,
                                        tl_object_ptr<ton_api::overlay_broadcastPlumtreeFec> msg,
                                        PlumtreePayloadOrigin origin = PlumtreePayloadOrigin::Push,
                                        const MissingPartKey *expected_key = nullptr);
  td::actor::Task<> process_simple_payload(OverlayImpl *overlay, adnl::AdnlNodeIdShort from,
                                           tl_object_ptr<ton_api::overlay_broadcastPlumtreeSimple> msg,
                                           PlumtreePayloadOrigin origin = PlumtreePayloadOrigin::Push,
                                           const MissingPartKey *expected_key = nullptr);
  td::actor::Task<> process_ihave(OverlayImpl *overlay, adnl::AdnlNodeIdShort from,
                                  tl_object_ptr<ton_api::overlay_broadcastPlumtreeIHave> msg);
  void process_repair_query(OverlayImpl *overlay, adnl::AdnlNodeIdShort from,
                            ton_api::overlay_repairPlumtreePart &query, td::Promise<td::BufferSlice> promise);
  void repair_query_finished();
  td::actor::Task<> process_repair_response(OverlayImpl *overlay, adnl::AdnlNodeIdShort from,
                                            MissingPartKey expected_key, td::BufferSlice data);
  td::actor::Task<> process_prune(OverlayImpl *overlay, adnl::AdnlNodeIdShort from,
                                  tl_object_ptr<ton_api::overlay_broadcastPlumtreePrune> msg);
  td::actor::Task<> process_useful(OverlayImpl *overlay, adnl::AdnlNodeIdShort from,
                                   tl_object_ptr<ton_api::overlay_broadcastPlumtreeUseful> msg);

  void alarm(OverlayImpl *overlay);
  td::Timestamp next_alarm_at();
  void gc(OverlayImpl *overlay);

 private:
  td::actor::ActorId<adnl::AdnlSenderInterface> sender_;
  PlumtreeFecOptions options_;
  bool is_original_sender_ = false;
  td::uint32 local_eager_limit_ = 0;

  std::vector<PlumtreeSlot> slots_;
  std::map<adnl::AdnlNodeIdShort, td::uint32> eager_peer_refcnt_;
  std::map<adnl::AdnlNodeIdShort, PlumtreeEagerPeerActivity> eager_peer_activity_;
  std::map<td::Bits256, std::unique_ptr<PlumtreeFecBroadcastState>> broadcasts_;
  std::map<td::Bits256, std::unique_ptr<PlumtreeSimpleBroadcastState>> simple_broadcasts_;
  std::map<MissingPartKey, std::unique_ptr<PlumtreeMissingPart>> missing_parts_;
  td::ListNode missing_parts_queue_;
  std::size_t active_repair_queries_ = 0;
  td::ListNode lru_;
  td::ListNode simple_lru_;

  td::Status validate_control_fields(const td::Bits256 &broadcast_id, td::uint32 part_index,
                                     td::uint32 tree_index) const;

  PlumtreeSlot *slot(td::uint32 tree_index);
  td::uint32 slot_load(const PlumtreeSlot &slot) const;
  void expire_pending_feedback(PlumtreeSlot &slot) const;
  void expire_pending_feedback();
  bool can_reserve_eager_feedback(PlumtreeSlot &slot, const adnl::AdnlNodeIdShort &peer) const;
  td::Status check_payload_origin(PlumtreePayloadOrigin origin, const MissingPartKey *expected_key,
                                  const MissingPartKey &actual_key, const PlumtreeSlot &slot,
                                  const adnl::AdnlNodeIdShort &from) const;
  void register_full_payload_send(PlumtreeSlot &slot, PlumtreePartState &part, const adnl::AdnlNodeIdShort &dst);
  void note_eager_peer_active(adnl::AdnlNodeIdShort peer);
  bool is_inactive_eager_peer(adnl::AdnlNodeIdShort peer, td::Timestamp now, bool receives_broadcasts) const;
  void remove_inactive_eager_peers(OverlayImpl *overlay, PlumtreeSlot &slot);
  bool add_eager_peer_ref(adnl::AdnlNodeIdShort peer);
  bool remove_eager_peer_ref(adnl::AdnlNodeIdShort peer);
  std::vector<adnl::AdnlNodeIdShort> get_eager_mtu_peers() const;
  void refresh_eager_mtu(OverlayImpl *overlay) const;
  void promote_eager(OverlayImpl *overlay, PlumtreeSlot &slot, adnl::AdnlNodeIdShort peer, bool force);
  void trim_eager_to_capacity(OverlayImpl *overlay, PlumtreeSlot &slot);
  void remove_eager(OverlayImpl *overlay, PlumtreeSlot &slot, adnl::AdnlNodeIdShort peer);

  PlumtreeFecBroadcastState *get_state(const td::Bits256 &broadcast_id);
  PlumtreeSimpleBroadcastState *get_simple_state(const td::Bits256 &broadcast_id);
  bool has_state(const td::Bits256 &broadcast_id) const;
  PlumtreePartState *get_part(const td::Bits256 &broadcast_id, td::uint32 part_index, td::uint32 tree_index);
  td::Result<PlumtreeFecBroadcastRef> get_or_create_fec_broadcast_state(
      const td::Bits256 &broadcast_id, PublicKeyHash source, BroadcastCheckResult check, td::uint32 flags,
      const td::Bits256 &full_data_hash, td::uint32 full_data_size, td::uint32 part_size);
  td::Status ensure_decoder(PlumtreeFecBroadcastState &broadcast);
  td::Result<PlumtreeDecodedBroadcast> add_decoder_part_and_decode(OverlayImpl *overlay,
                                                                   PlumtreeFecBroadcastState &broadcast,
                                                                   td::uint32 part_index, const td::BufferSlice &part);
  PlumtreePartState *add_fec_part_state(PlumtreeFecBroadcastState &broadcast, td::uint32 part_index,
                                        td::uint32 tree_index, double timestamp, PublicKey source_key,
                                        PublicKeyHash source, std::shared_ptr<Certificate> cert, td::uint32 data_size,
                                        td::Bits256 data_hash, td::BufferSlice signature, td::BufferSlice data);
  td::Result<PlumtreeDecodedBroadcast> decode_fec_part(OverlayImpl *overlay, PlumtreeFecBroadcastState &broadcast,
                                                       td::uint32 part_index);
  PlumtreeMissingPart *get_or_create_missing_part(const MissingPartKey &key, td::uint32 data_size);
  PlumtreeMissingPart *oldest_missing_part();
  void erase_missing_part(const MissingPartKey &key);

  bool send_control(OverlayImpl *overlay, const adnl::AdnlNodeIdShort &dst,
                    tl_object_ptr<ton_api::overlay_Broadcast> control);
  void send_repair_requests(OverlayImpl *overlay, const MissingPartKey &key, PlumtreeMissingPart &missing);
  td::Result<td::BufferSlice> prepare_fec_payload_for_peer(OverlayImpl *overlay, PlumtreeFecBroadcastState &broadcast,
                                                           PlumtreePartState &part, const adnl::AdnlNodeIdShort &dst);
  td::Result<td::BufferSlice> prepare_simple_payload_for_peer(OverlayImpl *overlay,
                                                              PlumtreeSimpleBroadcastState &payload,
                                                              PlumtreePartState &part,
                                                              const adnl::AdnlNodeIdShort &dst);
  td::Result<td::BufferSlice> prepare_payload_for_peer(OverlayImpl *overlay, const td::Bits256 &broadcast_id,
                                                       PlumtreePartState &part, const adnl::AdnlNodeIdShort &dst);
  bool send_payload_to(OverlayImpl *overlay, const td::Bits256 &broadcast_id, PlumtreePartState &part,
                       const adnl::AdnlNodeIdShort &dst);
  void send_ihave_to(OverlayImpl *overlay, PlumtreePartState &part, const td::Bits256 &broadcast_id,
                     const adnl::AdnlNodeIdShort &dst);
  void send_prune(OverlayImpl *overlay, const adnl::AdnlNodeIdShort &dst, const td::Bits256 &broadcast_id,
                  td::uint32 part_index, td::uint32 tree_index);
  void send_useful(OverlayImpl *overlay, const adnl::AdnlNodeIdShort &dst, const PlumtreePartState &part,
                   const td::Bits256 &broadcast_id);
  void forward_payload(OverlayImpl *overlay, const td::Bits256 &broadcast_id, PlumtreePartState &part,
                       adnl::AdnlNodeIdShort from);

  void send_assigned_parts(OverlayImpl *overlay, PublicKeyHash send_as, td::uint32 flags, td::BufferSlice data,
                           td::uint32 full_data_size, td::uint32 part_size, td::Bits256 full_data_hash,
                           td::Bits256 broadcast_id, std::vector<TreePartKey> assigned_parts, const char *mode);
};

td::Status BroadcastsPlumtree::Impl::validate_control_fields(const td::Bits256 &broadcast_id, td::uint32 part_index,
                                                             td::uint32 tree_index) const {
  if (broadcast_id.is_zero()) {
    return td::Status::Error(ErrorCode::protoviolation, "empty Plumtree broadcast id");
  }
  if (tree_index >= options_.tree_slots_) {
    return td::Status::Error(ErrorCode::protoviolation, "invalid Plumtree part or tree index");
  }
  if (tree_index == PLUMTREE_SIMPLE_TREE_INDEX) {
    if (part_index != 0) {
      return td::Status::Error(ErrorCode::protoviolation, "invalid Plumtree simple part index");
    }
    return td::Status::OK();
  }
  if (part_index >= options_.parts_ || tree_index != part_index + PLUMTREE_FEC_TREE_OFFSET) {
    return td::Status::Error(ErrorCode::protoviolation, "Plumtree FEC part must use the matching tree");
  }
  return td::Status::OK();
}

PlumtreeSlot *BroadcastsPlumtree::Impl::slot(td::uint32 tree_index) {
  return tree_index < slots_.size() ? &slots_[tree_index] : nullptr;
}

td::uint32 BroadcastsPlumtree::Impl::slot_load(const PlumtreeSlot &slot) const {
  return static_cast<td::uint32>(slot.eager.size() + slot.pending_feedback.size());
}

void BroadcastsPlumtree::Impl::expire_pending_feedback(PlumtreeSlot &slot) const {
  auto expired_before = td::Timestamp::now() - PLUMTREE_PENDING_FEEDBACK_TTL;
  for (auto it = slot.pending_feedback.begin(); it != slot.pending_feedback.end();) {
    if (it->second <= expired_before) {
      it = slot.pending_feedback.erase(it);
    } else {
      ++it;
    }
  }
}

void BroadcastsPlumtree::Impl::expire_pending_feedback() {
  for (auto &slot : slots_) {
    expire_pending_feedback(slot);
  }
}

bool BroadcastsPlumtree::Impl::can_reserve_eager_feedback(PlumtreeSlot &slot, const adnl::AdnlNodeIdShort &peer) const {
  expire_pending_feedback(slot);
  if (peer.is_zero()) {
    return false;
  }
  if (slot.eager.contains(peer) || slot.pending_feedback.contains(peer)) {
    return true;
  }
  return slot_load(slot) < local_eager_limit_;
}

td::Status BroadcastsPlumtree::Impl::check_payload_origin(PlumtreePayloadOrigin origin,
                                                          const MissingPartKey *expected_key,
                                                          const MissingPartKey &actual_key, const PlumtreeSlot &slot,
                                                          const adnl::AdnlNodeIdShort &from) const {
  if (origin == PlumtreePayloadOrigin::RepairResponse) {
    if (!expected_key || *expected_key != actual_key) {
      return td::Status::Error(ErrorCode::protoviolation, "Plumtree repair response key mismatch");
    }
    return td::Status::OK();
  }
  if (!slot.eager.contains(from)) {
    return td::Status::Error(ErrorCode::notready, "unsolicited Plumtree payload from non-eager peer");
  }
  return td::Status::OK();
}

void BroadcastsPlumtree::Impl::register_full_payload_send(PlumtreeSlot &slot, PlumtreePartState &part,
                                                          const adnl::AdnlNodeIdShort &dst) {
  if (slot.eager.contains(dst)) {
    slot.pending_feedback.erase(dst);
    ++eager_peer_activity_[dst].sent_since_active;
  } else {
    slot.pending_feedback[dst] = td::Timestamp::now();
  }
  part.full_sent_to.insert(dst);
  ++part.full_sends;
}

void BroadcastsPlumtree::Impl::note_eager_peer_active(adnl::AdnlNodeIdShort peer) {
  auto it = eager_peer_activity_.find(peer);
  if (it == eager_peer_activity_.end()) {
    return;
  }
  it->second.last_active_at = td::Timestamp::now();
  it->second.sent_since_active = 0;
}

bool BroadcastsPlumtree::Impl::is_inactive_eager_peer(adnl::AdnlNodeIdShort peer, td::Timestamp now,
                                                      bool receives_broadcasts) const {
  auto it = eager_peer_activity_.find(peer);
  if (it == eager_peer_activity_.end()) {
    return false;
  }
  if (it->second.last_active_at > now - PLUMTREE_EAGER_PEER_INACTIVITY_TTL) {
    return false;
  }
  if (!receives_broadcasts) {
    return true;
  }
  return it->second.sent_since_active > PLUMTREE_EAGER_PEER_MAX_SENT_WITHOUT_ACTIVITY;
}

void BroadcastsPlumtree::Impl::remove_inactive_eager_peers(OverlayImpl *overlay, PlumtreeSlot &slot) {
  bool mtu_peers_changed = false;
  auto now = td::Timestamp::now();
  for (auto it = slot.eager.begin(); it != slot.eager.end();) {
    if (!is_inactive_eager_peer(*it, now, overlay->peer_receives_broadcasts(*it))) {
      ++it;
      continue;
    }
    auto peer = *it;
    slot.pending_feedback.erase(peer);
    mtu_peers_changed = remove_eager_peer_ref(peer) || mtu_peers_changed;
    it = slot.eager.erase(it);
  }
  if (mtu_peers_changed) {
    refresh_eager_mtu(overlay);
  }
}

bool BroadcastsPlumtree::Impl::add_eager_peer_ref(adnl::AdnlNodeIdShort peer) {
  auto &cnt = eager_peer_refcnt_[peer];
  ++cnt;
  if (cnt == 1) {
    eager_peer_activity_[peer] = {};
    return true;
  }
  return false;
}

bool BroadcastsPlumtree::Impl::remove_eager_peer_ref(adnl::AdnlNodeIdShort peer) {
  auto it = eager_peer_refcnt_.find(peer);
  CHECK(it != eager_peer_refcnt_.end());
  CHECK(it->second > 0);
  --it->second;
  if (it->second != 0) {
    return false;
  }
  eager_peer_refcnt_.erase(it);
  eager_peer_activity_.erase(peer);
  return true;
}

std::vector<adnl::AdnlNodeIdShort> BroadcastsPlumtree::Impl::get_eager_mtu_peers() const {
  std::vector<adnl::AdnlNodeIdShort> peers;
  peers.reserve(eager_peer_refcnt_.size());
  for (const auto &[peer, _] : eager_peer_refcnt_) {
    peers.push_back(peer);
  }
  return peers;
}

void BroadcastsPlumtree::Impl::refresh_eager_mtu(OverlayImpl *overlay) const {
  if (is_original_sender_) {
    overlay->set_plumtree_eager_mtu_peers({});
    return;
  }
  overlay->set_plumtree_eager_mtu_peers(get_eager_mtu_peers());
}

void BroadcastsPlumtree::Impl::promote_eager(OverlayImpl *overlay, PlumtreeSlot &slot, adnl::AdnlNodeIdShort peer,
                                             bool force) {
  expire_pending_feedback(slot);
  if (peer.is_zero()) {
    return;
  }
  slot.pending_feedback.erase(peer);
  if (slot.eager.contains(peer)) {
    return;
  }
  if (local_eager_limit_ == 0) {
    return;
  }
  if (!force && slot.eager.size() >= local_eager_limit_) {
    return;
  }
  bool mtu_peers_changed = false;
  if (force && slot.eager.size() >= local_eager_limit_) {
    auto it = slot.eager.begin();
    std::advance(it, td::Random::fast(0, static_cast<td::int32>(slot.eager.size()) - 1));
    slot.pending_feedback.erase(*it);
    mtu_peers_changed = remove_eager_peer_ref(*it) || mtu_peers_changed;
    slot.eager.erase(it);
  }
  if (slot.eager.insert(peer).second) {
    mtu_peers_changed = add_eager_peer_ref(peer) || mtu_peers_changed;
  }
  if (mtu_peers_changed) {
    refresh_eager_mtu(overlay);
  }
}

void BroadcastsPlumtree::Impl::trim_eager_to_capacity(OverlayImpl *overlay, PlumtreeSlot &slot) {
  expire_pending_feedback(slot);
  bool mtu_peers_changed = false;
  while (slot.eager.size() > local_eager_limit_) {
    auto it = slot.eager.begin();
    std::advance(it, td::Random::fast(0, static_cast<td::int32>(slot.eager.size()) - 1));
    slot.pending_feedback.erase(*it);
    mtu_peers_changed = remove_eager_peer_ref(*it) || mtu_peers_changed;
    slot.eager.erase(it);
  }
  if (mtu_peers_changed) {
    refresh_eager_mtu(overlay);
  }
}

void BroadcastsPlumtree::Impl::remove_eager(OverlayImpl *overlay, PlumtreeSlot &slot, adnl::AdnlNodeIdShort peer) {
  bool mtu_peers_changed = false;
  if (slot.eager.erase(peer) > 0) {
    mtu_peers_changed = remove_eager_peer_ref(peer);
  }
  slot.pending_feedback.erase(peer);
  if (mtu_peers_changed) {
    refresh_eager_mtu(overlay);
  }
}

PlumtreeFecBroadcastState *BroadcastsPlumtree::Impl::get_state(const td::Bits256 &broadcast_id) {
  auto it = broadcasts_.find(broadcast_id);
  return it == broadcasts_.end() ? nullptr : it->second.get();
}

PlumtreeSimpleBroadcastState *BroadcastsPlumtree::Impl::get_simple_state(const td::Bits256 &broadcast_id) {
  auto it = simple_broadcasts_.find(broadcast_id);
  return it == simple_broadcasts_.end() ? nullptr : it->second.get();
}

bool BroadcastsPlumtree::Impl::has_state(const td::Bits256 &broadcast_id) const {
  return broadcasts_.contains(broadcast_id) || simple_broadcasts_.contains(broadcast_id);
}

PlumtreePartState *BroadcastsPlumtree::Impl::get_part(const td::Bits256 &broadcast_id, td::uint32 part_index,
                                                      td::uint32 tree_index) {
  if (auto *state = get_state(broadcast_id)) {
    auto it = state->tree_parts.find(TreePartKey{part_index, tree_index});
    if (it != state->tree_parts.end()) {
      return &it->second;
    }
  }
  if (auto *state = get_simple_state(broadcast_id)) {
    if (state->part.part_index == part_index && state->part.tree_index == tree_index) {
      return &state->part;
    }
  }
  return nullptr;
}

td::Result<PlumtreeFecBroadcastRef> BroadcastsPlumtree::Impl::get_or_create_fec_broadcast_state(
    const td::Bits256 &broadcast_id, PublicKeyHash source, BroadcastCheckResult check, td::uint32 flags,
    const td::Bits256 &full_data_hash, td::uint32 full_data_size, td::uint32 part_size) {
  auto it = broadcasts_.find(broadcast_id);
  if (it != broadcasts_.end()) {
    auto &state = *it->second;
    if (state.flags != flags || state.full_data_hash != full_data_hash || state.full_data_size != full_data_size ||
        state.part_size != part_size) {
      return td::Status::Error(ErrorCode::protoviolation, "Plumtree broadcast id collision");
    }
    return PlumtreeFecBroadcastRef{it->second.get(), false};
  }

  auto state = std::make_unique<PlumtreeFecBroadcastState>();
  state->broadcast_id = broadcast_id;
  state->first_valid_source = source;
  state->check_result = check;
  state->full_data_hash = full_data_hash;
  state->first_seen_at = td::Timestamp::now();
  state->flags = flags;
  state->full_data_size = full_data_size;
  state->part_size = part_size;
  auto *state_ptr = state.get();
  lru_.put(state_ptr);
  broadcasts_.emplace(broadcast_id, std::move(state));
  return PlumtreeFecBroadcastRef{state_ptr, true};
}

td::Status BroadcastsPlumtree::Impl::ensure_decoder(PlumtreeFecBroadcastState &broadcast) {
  if (broadcast.delivered || broadcast.decode_failed || broadcast.decoder) {
    return td::Status::OK();
  }
  if (broadcast.part_size == 0 || broadcast.full_data_size == 0) {
    return td::Status::Error(ErrorCode::protoviolation, "invalid Plumtree decoder parameters");
  }
  TRY_RESULT(decoder,
             td::raptorq::Decoder::create({(broadcast.full_data_size + broadcast.part_size - 1) / broadcast.part_size,
                                           broadcast.part_size, broadcast.full_data_size}));
  broadcast.decoder = std::move(decoder);
  return td::Status::OK();
}

td::Result<PlumtreeDecodedBroadcast> BroadcastsPlumtree::Impl::add_decoder_part_and_decode(
    OverlayImpl *overlay, PlumtreeFecBroadcastState &broadcast, td::uint32 part_index, const td::BufferSlice &part) {
  if (broadcast.delivered || broadcast.decode_failed || broadcast.decoder_parts.contains(part_index)) {
    return PlumtreeDecodedBroadcast{};
  }
  TRY_STATUS(ensure_decoder(broadcast));
  if (!broadcast.decoder) {
    return PlumtreeDecodedBroadcast{};
  }
  TRY_STATUS(broadcast.decoder->add_symbol({part_index, part.clone()}));
  broadcast.decoder_parts.insert(part_index);
  if (!broadcast.decoder->may_try_decode()) {
    return PlumtreeDecodedBroadcast{};
  }
  TRY_RESULT(decoded, broadcast.decoder->try_decode(false));
  if (broadcast.full_data_hash != td::sha256_bits256(decoded.data.as_slice())) {
    broadcast.decode_failed = true;
    broadcast.decoder = {};
    return td::Status::Error(ErrorCode::protoviolation, "Plumtree decoded data hash mismatch");
  }
  broadcast.delivered = true;
  broadcast.decoder = {};
  overlay->register_delivered_broadcast(broadcast.broadcast_id);
  VLOG(PLUMTREE_INFO) << overlay << ": Plumtree FINISH receiver broadcast_id=" << broadcast.broadcast_id.to_hex()
                      << " full_data_size=" << broadcast.full_data_size << " decoded=true";
  return PlumtreeDecodedBroadcast{broadcast.first_valid_source, broadcast.check_result, std::move(decoded.data)};
}

PlumtreePartState *BroadcastsPlumtree::Impl::add_fec_part_state(PlumtreeFecBroadcastState &broadcast,
                                                                td::uint32 part_index, td::uint32 tree_index,
                                                                double timestamp, PublicKey source_key,
                                                                PublicKeyHash source, std::shared_ptr<Certificate> cert,
                                                                td::uint32 data_size, td::Bits256 data_hash,
                                                                td::BufferSlice signature, td::BufferSlice data) {
  if (!broadcast.parts_by_index.contains(part_index)) {
    broadcast.parts_by_index.emplace(part_index, std::move(data));
  }

  PlumtreePartState part;
  part.part_index = part_index;
  part.tree_index = tree_index;
  part.timestamp = timestamp;
  part.data_size = data_size;
  part.data_hash = data_hash;
  part.source_key = std::move(source_key);
  part.source = source;
  part.certificate = std::move(cert);
  part.signature = std::move(signature);
  auto [part_it, _] = broadcast.tree_parts.emplace(TreePartKey{part_index, tree_index}, std::move(part));
  return &part_it->second;
}

td::Result<PlumtreeDecodedBroadcast> BroadcastsPlumtree::Impl::decode_fec_part(OverlayImpl *overlay,
                                                                               PlumtreeFecBroadcastState &broadcast,
                                                                               td::uint32 part_index) {
  auto part_data_it = broadcast.parts_by_index.find(part_index);
  if (part_data_it == broadcast.parts_by_index.end()) {
    return td::Status::Error(ErrorCode::protoviolation, "missing Plumtree FEC part data");
  }
  return add_decoder_part_and_decode(overlay, broadcast, part_index, part_data_it->second);
}

PlumtreeMissingPart *BroadcastsPlumtree::Impl::get_or_create_missing_part(const MissingPartKey &key,
                                                                          td::uint32 data_size) {
  auto it = missing_parts_.find(key);
  if (it != missing_parts_.end()) {
    it->second->data_size = std::max(it->second->data_size, data_size);
    return it->second.get();
  }

  auto missing = std::make_unique<PlumtreeMissingPart>(key);
  missing->data_size = data_size;
  auto *result = missing.get();
  missing_parts_queue_.put(result);
  missing_parts_.emplace(key, std::move(missing));
  while (missing_parts_.size() > MAX_PENDING_REPAIR_PARTS) {
    auto *oldest = oldest_missing_part();
    erase_missing_part(oldest->key);
  }
  return result;
}

PlumtreeMissingPart *BroadcastsPlumtree::Impl::oldest_missing_part() {
  auto *node = missing_parts_queue_.get_prev();
  return node == &missing_parts_queue_ ? nullptr : PlumtreeMissingPart::from_list_node(node);
}

void BroadcastsPlumtree::Impl::erase_missing_part(const MissingPartKey &key) {
  auto it = missing_parts_.find(key);
  if (it != missing_parts_.end()) {
    missing_parts_.erase(it);
  }
}

bool BroadcastsPlumtree::Impl::send_control(OverlayImpl *overlay, const adnl::AdnlNodeIdShort &dst,
                                            tl_object_ptr<ton_api::overlay_Broadcast> control) {
  if (sender_.empty() || dst.is_zero() || dst == overlay->local_id()) {
    return false;
  }
  auto wire = serialize_tl_object(control, true);
  td::actor::send_closure(overlay->overlay_manager(), &Overlays::send_message_via, dst, overlay->local_id(),
                          overlay->overlay_id(), std::move(wire), sender_);
  return true;
}

void BroadcastsPlumtree::Impl::send_repair_requests(OverlayImpl *overlay, const MissingPartKey &key,
                                                    PlumtreeMissingPart &missing) {
  const auto &[broadcast_id, part_index, tree_index] = key;
  if (missing.data_size == 0) {
    return;
  }
  while (missing.sent_repair_targets < missing.repair_targets.size()) {
    const auto &dst = missing.repair_targets[missing.sent_repair_targets++];
    if (active_repair_queries_ >= MAX_ACTIVE_REPAIR_QUERIES) {
      VLOG(PLUMTREE_WARNING) << overlay << ": dropping Plumtree repair query due to active query cap: active="
                             << active_repair_queries_ << " limit=" << MAX_ACTIVE_REPAIR_QUERIES << " dst=" << dst
                             << " broadcast_id=" << broadcast_id.to_hex() << " part_index=" << part_index
                             << " tree_index=" << tree_index;
      continue;
    }
    ++active_repair_queries_;
    auto query = create_serialize_tl_object<ton_api::overlay_repairPlumtreePart>(broadcast_id, td::Clocks::system(),
                                                                                 static_cast<std::int32_t>(part_index),
                                                                                 static_cast<std::int32_t>(tree_index));
    auto promise = td::PromiseCreator::lambda(
        [overlay_id = actor_id(overlay), dst, broadcast_id, part_index, tree_index](td::Result<td::BufferSlice> R) {
          td::actor::send_closure(overlay_id, &OverlayImpl::receive_plumtree_repair_response, dst, broadcast_id,
                                  part_index, tree_index, std::move(R));
        });
    td::actor::send_closure(overlay->overlay_manager(), &Overlays::send_query_via, dst, overlay->local_id(),
                            overlay->overlay_id(), "plumtree repair", std::move(promise),
                            td::Timestamp::in(PLUMTREE_PENDING_FEEDBACK_TTL), std::move(query),
                            static_cast<td::uint64>(missing.data_size) + PLUMTREE_PAYLOAD_MTU_OVERHEAD, sender_);
  }
}

td::Result<td::BufferSlice> BroadcastsPlumtree::Impl::prepare_fec_payload_for_peer(OverlayImpl *overlay,
                                                                                   PlumtreeFecBroadcastState &broadcast,
                                                                                   PlumtreePartState &part,
                                                                                   const adnl::AdnlNodeIdShort &dst) {
  if (sender_.empty() || dst.is_zero() || dst == overlay->local_id()) {
    return td::Status::Error(ErrorCode::notready, "invalid Plumtree FEC payload destination");
  }
  if (!overlay->peer_receives_broadcasts(dst)) {
    return td::Status::Error(ErrorCode::notready, "peer does not receive Plumtree broadcasts");
  }
  auto *s = slot(part.tree_index);
  if (!s || !can_reserve_eager_feedback(*s, dst)) {
    return td::Status::Error(ErrorCode::notready, "Plumtree FEC payload cannot reserve feedback");
  }
  if (part.full_sends >= local_eager_limit_ || part.full_sent_to.contains(dst)) {
    return td::Status::Error(ErrorCode::notready, "Plumtree FEC payload was already sent");
  }
  auto part_data = broadcast.parts_by_index.find(part.part_index);
  if (part_data == broadcast.parts_by_index.end()) {
    return td::Status::Error(ErrorCode::protoviolation, "missing Plumtree FEC part data");
  }
  auto wire = create_serialize_tl_object<ton_api::overlay_broadcastPlumtreeFec>(
      static_cast<std::int32_t>(broadcast.flags), part.timestamp, part.source_key.tl(),
      part.certificate ? part.certificate->tl() : Certificate::empty_tl(), broadcast.full_data_hash,
      static_cast<std::int32_t>(broadcast.full_data_size), static_cast<std::int32_t>(part.part_index),
      static_cast<std::int32_t>(part.tree_index), part_data->second.clone(), part.signature.clone());
  auto wire_size = wire.size();
  register_full_payload_send(*s, part, dst);
  overlay->get_broadcasts_limiter(part.source, part.certificate.get()).register_out_traffic(wire_size);
  return std::move(wire);
}

td::Result<td::BufferSlice> BroadcastsPlumtree::Impl::prepare_simple_payload_for_peer(
    OverlayImpl *overlay, PlumtreeSimpleBroadcastState &payload, PlumtreePartState &part,
    const adnl::AdnlNodeIdShort &dst) {
  if (sender_.empty() || dst.is_zero() || dst == overlay->local_id()) {
    return td::Status::Error(ErrorCode::notready, "invalid Plumtree simple payload destination");
  }
  if (!overlay->peer_receives_broadcasts(dst)) {
    return td::Status::Error(ErrorCode::notready, "peer does not receive Plumtree broadcasts");
  }
  auto *s = slot(part.tree_index);
  if (!s || !can_reserve_eager_feedback(*s, dst)) {
    return td::Status::Error(ErrorCode::notready, "Plumtree simple payload cannot reserve feedback");
  }
  if (part.full_sends >= local_eager_limit_ || part.full_sent_to.contains(dst)) {
    return td::Status::Error(ErrorCode::notready, "Plumtree simple payload was already sent");
  }
  auto wire = create_serialize_tl_object<ton_api::overlay_broadcastPlumtreeSimple>(
      static_cast<std::int32_t>(payload.flags), part.timestamp, part.source_key.tl(),
      part.certificate ? part.certificate->tl() : Certificate::empty_tl(), payload.broadcast_id,
      static_cast<std::int32_t>(part.tree_index), payload.data.clone(), part.signature.clone());
  auto wire_size = wire.size();
  register_full_payload_send(*s, part, dst);
  overlay->get_broadcasts_limiter(part.source, part.certificate.get()).register_out_traffic(wire_size);
  return std::move(wire);
}

td::Result<td::BufferSlice> BroadcastsPlumtree::Impl::prepare_payload_for_peer(OverlayImpl *overlay,
                                                                               const td::Bits256 &broadcast_id,
                                                                               PlumtreePartState &part,
                                                                               const adnl::AdnlNodeIdShort &dst) {
  if (part.tree_index == PLUMTREE_SIMPLE_TREE_INDEX) {
    auto *state = get_simple_state(broadcast_id);
    if (!state) {
      return td::Status::Error(ErrorCode::notready, "unknown Plumtree simple broadcast");
    }
    return prepare_simple_payload_for_peer(overlay, *state, part, dst);
  }
  auto *state = get_state(broadcast_id);
  if (!state) {
    return td::Status::Error(ErrorCode::notready, "unknown Plumtree FEC broadcast");
  }
  return prepare_fec_payload_for_peer(overlay, *state, part, dst);
}

bool BroadcastsPlumtree::Impl::send_payload_to(OverlayImpl *overlay, const td::Bits256 &broadcast_id,
                                               PlumtreePartState &part, const adnl::AdnlNodeIdShort &dst) {
  auto wire = prepare_payload_for_peer(overlay, broadcast_id, part, dst);
  if (wire.is_error()) {
    return false;
  }
  td::actor::send_closure(overlay->overlay_manager(), &Overlays::send_message_via, dst, overlay->local_id(),
                          overlay->overlay_id(), wire.move_as_ok(), sender_);
  return true;
}

void BroadcastsPlumtree::Impl::send_ihave_to(OverlayImpl *overlay, PlumtreePartState &part,
                                             const td::Bits256 &broadcast_id, const adnl::AdnlNodeIdShort &dst) {
  if (dst.is_zero() || dst == overlay->local_id() || part.full_sent_to.contains(dst) ||
      part.advertised_to.contains(dst)) {
    return;
  }
  if (!overlay->peer_receives_broadcasts(dst)) {
    return;
  }
  auto *s = slot(part.tree_index);
  if (!s || !can_reserve_eager_feedback(*s, dst)) {
    return;
  }
  if (part.full_sends >= local_eager_limit_) {
    return;
  }
  if (part.data_size == 0) {
    return;
  }
  if (send_control(overlay, dst,
                   create_tl_object<ton_api::overlay_broadcastPlumtreeIHave>(
                       broadcast_id, td::Clocks::system(), static_cast<std::int32_t>(part.part_index),
                       static_cast<std::int32_t>(part.tree_index), part.source_key.tl(),
                       part.certificate ? part.certificate->tl() : Certificate::empty_tl(), part.timestamp,
                       static_cast<std::int32_t>(part.data_size), part.data_hash, part.signature.clone()))) {
    part.advertised_to.insert(dst);
  }
}

void BroadcastsPlumtree::Impl::send_prune(OverlayImpl *overlay, const adnl::AdnlNodeIdShort &dst,
                                          const td::Bits256 &broadcast_id, td::uint32 part_index,
                                          td::uint32 tree_index) {
  send_control(overlay, dst,
               create_tl_object<ton_api::overlay_broadcastPlumtreePrune>(broadcast_id, td::Clocks::system(),
                                                                         static_cast<std::int32_t>(part_index),
                                                                         static_cast<std::int32_t>(tree_index)));
}

void BroadcastsPlumtree::Impl::send_useful(OverlayImpl *overlay, const adnl::AdnlNodeIdShort &dst,
                                           const PlumtreePartState &part, const td::Bits256 &broadcast_id) {
  send_control(overlay, dst,
               create_tl_object<ton_api::overlay_broadcastPlumtreeUseful>(broadcast_id, td::Clocks::system(),
                                                                          static_cast<std::int32_t>(part.part_index),
                                                                          static_cast<std::int32_t>(part.tree_index)));
}

void BroadcastsPlumtree::Impl::forward_payload(OverlayImpl *overlay, const td::Bits256 &broadcast_id,
                                               PlumtreePartState &part, adnl::AdnlNodeIdShort from) {
  auto *s = slot(part.tree_index);
  if (!s) {
    return;
  }

  auto active = overlay->get_plumtree_neighbours(options_.active_neighbours_);
  active.erase(std::remove_if(active.begin(), active.end(),
                              [&](const auto &peer) { return peer == overlay->local_id() || peer == from; }),
               active.end());
  std::set<adnl::AdnlNodeIdShort> full_sent;
  remove_inactive_eager_peers(overlay, *s);
  trim_eager_to_capacity(overlay, *s);
  for (const auto &peer : s->eager) {
    if (peer != from && send_payload_to(overlay, broadcast_id, part, peer)) {
      full_sent.insert(peer);
    }
  }

  for (const auto &peer : active) {
    if (peer != from && !full_sent.contains(peer) && !s->eager.contains(peer)) {
      send_ihave_to(overlay, part, broadcast_id, peer);
    }
  }
}

void BroadcastsPlumtree::Impl::send_fec(OverlayImpl *overlay, PublicKeyHash send_as, td::uint32 flags,
                                        td::BufferSlice data) {
  if (data.empty() || data.size() > Overlays::max_fec_broadcast_size()) {
    VLOG(PLUMTREE_WARNING) << overlay << ": invalid Plumtree FEC payload size " << data.size();
    return;
  }
  if (send_as.is_zero()) {
    VLOG(PLUMTREE_INFO) << overlay << ": no local Plumtree source";
    return;
  }
  if (options_.k_ == 0 || options_.parts_ == 0 || options_.tree_slots_ <= options_.parts_) {
    VLOG(PLUMTREE_WARNING) << overlay << ": invalid Plumtree FEC configuration";
    return;
  }

  auto full_data_size = static_cast<td::uint32>(data.size());
  auto part_size = static_cast<td::uint32>((data.size() + options_.k_ - 1) / options_.k_);
  if (part_size == 0) {
    VLOG(PLUMTREE_WARNING) << overlay << ": invalid Plumtree part size";
    return;
  }
  auto full_data_hash = td::sha256_bits256(data.as_slice());
  auto broadcast_id = compute_broadcast_id(flags, full_data_hash, full_data_size, part_size);
  auto cert = overlay->get_certificate(send_as);
  if (overlay->check_source_eligible(send_as, cert.get(), full_data_size, /* is_fec = */ true,
                                     /* is_any_sender = */ flags & Overlays::BroadcastFlagAnySender()) ==
      BroadcastCheckResult::Forbidden) {
    VLOG(PLUMTREE_WARNING) << overlay << ": Plumtree source is no longer eligible";
    return;
  }

  std::vector<TreePartKey> assigned_parts;
  assigned_parts.reserve(options_.parts_);
  for (td::uint32 part_index = 0; part_index < options_.parts_; ++part_index) {
    assigned_parts.emplace_back(part_index, part_index + PLUMTREE_FEC_TREE_OFFSET);
  }

  send_assigned_parts(overlay, send_as, flags, std::move(data), full_data_size, part_size, full_data_hash, broadcast_id,
                      std::move(assigned_parts), "all-parts");
}

void BroadcastsPlumtree::Impl::send(OverlayImpl *overlay, PublicKeyHash send_as, td::uint32 flags,
                                    td::Bits256 broadcast_id, td::BufferSlice data) {
  if (broadcast_id.is_zero()) {
    VLOG(PLUMTREE_WARNING) << overlay << ": empty Plumtree simple broadcast id";
    return;
  }
  if (data.empty() || data.size() > Overlays::max_fec_broadcast_size()) {
    VLOG(PLUMTREE_WARNING) << overlay << ": invalid Plumtree simple payload size " << data.size();
    return;
  }
  if (send_as.is_zero()) {
    VLOG(PLUMTREE_INFO) << overlay << ": no local Plumtree simple source";
    return;
  }
  if (simple_broadcasts_.contains(broadcast_id) || overlay->is_delivered(broadcast_id)) {
    VLOG(PLUMTREE_INFO) << overlay << ": duplicate Plumtree simple broadcast_id=" << broadcast_id.to_hex();
    return;
  }

  auto data_size = static_cast<td::uint32>(data.size());
  auto data_hash = td::sha256_bits256(data.as_slice());
  auto cert = overlay->get_certificate(send_as);
  if (overlay->check_source_eligible(send_as, cert.get(), data_size, /* is_fec = */ true,
                                     /* is_any_sender = */ flags & Overlays::BroadcastFlagAnySender()) ==
      BroadcastCheckResult::Forbidden) {
    VLOG(PLUMTREE_WARNING) << overlay << ": Plumtree simple source is no longer eligible";
    return;
  }

  PlumtreeOutboundSimplePayload payload;
  payload.broadcast_id = broadcast_id;
  payload.flags = flags;
  payload.timestamp = td::Clocks::system();
  payload.source = send_as;
  payload.tree_index = PLUMTREE_SIMPLE_TREE_INDEX;
  payload.data_size = data_size;
  payload.data_hash = data_hash;
  payload.data = std::move(data);

  auto to_sign =
      make_simple_payload_to_sign(payload.broadcast_id, payload.timestamp, payload.tree_index, data_size, data_hash);
  auto promise = td::PromiseCreator::lambda([overlay_id = actor_id(overlay), payload = std::move(payload)](
                                                td::Result<std::pair<td::BufferSlice, PublicKey>> R) mutable {
    td::actor::send_closure(overlay_id, &OverlayImpl::broadcast_plumtree_signed_simple, std::move(payload),
                            std::move(R));
  });
  td::actor::send_closure(overlay->keyring(), &keyring::Keyring::sign_add_get_public_key, send_as, std::move(to_sign),
                          std::move(promise));
}

void BroadcastsPlumtree::Impl::send_assigned_parts(OverlayImpl *overlay, PublicKeyHash send_as, td::uint32 flags,
                                                   td::BufferSlice data, td::uint32 full_data_size,
                                                   td::uint32 part_size, td::Bits256 full_data_hash,
                                                   td::Bits256 broadcast_id, std::vector<TreePartKey> assigned_parts,
                                                   const char *mode) {
  if (assigned_parts.empty()) {
    VLOG(PLUMTREE_INFO) << overlay
                        << ": no Plumtree parts assigned to local source for broadcast_id=" << broadcast_id.to_hex();
    return;
  }

  auto encoder_r = td::raptorq::Encoder::create(part_size, data.clone());
  if (encoder_r.is_error()) {
    VLOG(PLUMTREE_WARNING) << overlay << ": cannot create Plumtree FEC encoder: " << encoder_r.move_as_error();
    return;
  }
  auto encoder = encoder_r.move_as_ok();
  encoder->precalc();

  td::uint32 signed_parts = 0;
  for (const auto &[part_index, tree_index] : assigned_parts) {
    td::BufferSlice part_data(part_size);
    auto status = encoder->gen_symbol(part_index, part_data.as_slice());
    if (status.is_error()) {
      VLOG(PLUMTREE_WARNING) << overlay << ": cannot generate Plumtree symbol: " << status;
      continue;
    }
    auto part_hash = td::sha256_bits256(part_data.as_slice());

    PlumtreeOutboundFecPayload payload;
    payload.broadcast_id = broadcast_id;
    payload.flags = flags;
    payload.timestamp = td::Clocks::system();
    payload.source = send_as;
    payload.full_data_hash = full_data_hash;
    payload.full_data_size = full_data_size;
    payload.data_hash = part_hash;
    payload.data_size = part_size;
    payload.part_index = part_index;
    payload.tree_index = tree_index;
    payload.data = std::move(part_data);

    auto to_sign = make_fec_payload_to_sign(payload.broadcast_id, payload.timestamp, payload.part_index,
                                            payload.tree_index, payload.data_size, payload.data_hash);
    auto promise = td::PromiseCreator::lambda([overlay_id = actor_id(overlay), payload = std::move(payload)](
                                                  td::Result<std::pair<td::BufferSlice, PublicKey>> R) mutable {
      td::actor::send_closure(overlay_id, &OverlayImpl::broadcast_plumtree_signed_fec, std::move(payload),
                              std::move(R));
    });
    td::actor::send_closure(overlay->keyring(), &keyring::Keyring::sign_add_get_public_key, send_as, std::move(to_sign),
                            std::move(promise));
    ++signed_parts;
  }

  VLOG(PLUMTREE_INFO) << overlay << ": Plumtree START sender broadcast_id=" << broadcast_id.to_hex()
                      << " full_data_hash=" << full_data_hash.to_hex() << " full_data_size=" << full_data_size
                      << " scheduled_parts=" << options_.parts_ << " signed_parts=" << signed_parts << " mode=" << mode;
}

void BroadcastsPlumtree::Impl::signed_fec(OverlayImpl *overlay, PlumtreeOutboundFecPayload &&payload,
                                          td::Result<std::pair<td::BufferSlice, PublicKey>> &&R) {
  if (R.is_error()) {
    auto status = R.move_as_error();
    LOG_IF(WARNING, status.code() != ErrorCode::notready) << "failed to sign Plumtree FEC payload: " << status;
    return;
  }
  auto signed_part = R.move_as_ok();
  if (signed_part.second.compute_short_id() != payload.source) {
    VLOG(PLUMTREE_WARNING) << overlay << ": keyring signed Plumtree FEC payload with unexpected source";
    return;
  }
  if (payload.part_index >= options_.parts_ || payload.tree_index >= options_.tree_slots_ ||
      payload.tree_index != payload.part_index + PLUMTREE_FEC_TREE_OFFSET) {
    VLOG(PLUMTREE_WARNING) << overlay << ": Plumtree signed payload is outside current option bounds";
    return;
  }
  if (options_.k_ == 0 || options_.parts_ == 0 || options_.tree_slots_ <= options_.parts_) {
    VLOG(PLUMTREE_WARNING) << overlay << ": invalid Plumtree FEC configuration";
    return;
  }
  auto expected_part_size =
      static_cast<td::uint32>((static_cast<td::uint64>(payload.full_data_size) + options_.k_ - 1) / options_.k_);
  auto part_size = payload.data_size;
  if (payload.full_data_size == 0 || payload.full_data_size > Overlays::max_fec_broadcast_size() || part_size == 0 ||
      part_size != expected_part_size || payload.data.empty() || payload.data.size() != part_size ||
      td::sha256_bits256(payload.data.as_slice()) != payload.data_hash) {
    VLOG(PLUMTREE_WARNING) << overlay << ": Plumtree signed payload has invalid FEC fields";
    return;
  }

  auto cert = overlay->get_certificate(payload.source);
  auto check = overlay->check_source_eligible(payload.source, cert.get(), payload.full_data_size, /* is_fec = */ true,
                                              /* is_any_sender = */ payload.flags & Overlays::BroadcastFlagAnySender());
  if (check == BroadcastCheckResult::Forbidden) {
    VLOG(PLUMTREE_WARNING) << overlay << ": Plumtree source became ineligible before signed payload send";
    return;
  }
  auto broadcast_ref_r = get_or_create_fec_broadcast_state(payload.broadcast_id, payload.source, check, payload.flags,
                                                           payload.full_data_hash, payload.full_data_size, part_size);
  if (broadcast_ref_r.is_error()) {
    VLOG(PLUMTREE_WARNING) << overlay << ": Plumtree signed payload conflicts with existing broadcast state: "
                           << broadcast_ref_r.move_as_error();
    return;
  }
  auto broadcast_ref = broadcast_ref_r.move_as_ok();
  auto &broadcast = *broadcast_ref.state;
  if (get_part(payload.broadcast_id, payload.part_index, payload.tree_index)) {
    return;
  }

  auto *part = add_fec_part_state(broadcast, payload.part_index, payload.tree_index, payload.timestamp,
                                  std::move(signed_part.second), payload.source, cert, part_size, payload.data_hash,
                                  std::move(signed_part.first), std::move(payload.data));

  forward_payload(overlay, payload.broadcast_id, *part, adnl::AdnlNodeIdShort::zero());
  auto decoded = decode_fec_part(overlay, broadcast, payload.part_index);
  if (decoded.is_error()) {
    VLOG(PLUMTREE_WARNING) << overlay << ": failed to process local Plumtree part: " << decoded.move_as_error();
    return;
  }
  auto decoded_broadcast = decoded.move_as_ok();
  if (!decoded_broadcast.data.empty()) {
    check_and_deliver(overlay, decoded_broadcast.source, decoded_broadcast.check_result,
                      std::move(decoded_broadcast.data))
        .start()
        .detach();
  }

  if (!broadcast.overlay_limiter_registered) {
    overlay->get_broadcasts_limiter(payload.source, cert.get()).register_broadcast(payload.full_data_size);
    broadcast.overlay_limiter_registered = true;
  }
  VLOG(PLUMTREE_INFO) << overlay << ": Plumtree FEC_SEND sender broadcast_id=" << payload.broadcast_id.to_hex()
                      << " part_index=" << payload.part_index << " tree_index=" << payload.tree_index
                      << " full_sent=" << part->full_sends;
}

void BroadcastsPlumtree::Impl::signed_simple(OverlayImpl *overlay, PlumtreeOutboundSimplePayload &&payload,
                                             td::Result<std::pair<td::BufferSlice, PublicKey>> &&R) {
  if (R.is_error()) {
    auto status = R.move_as_error();
    LOG_IF(WARNING, status.code() != ErrorCode::notready) << "failed to sign Plumtree simple payload: " << status;
    return;
  }
  auto signed_payload = R.move_as_ok();
  if (signed_payload.second.compute_short_id() != payload.source) {
    VLOG(PLUMTREE_WARNING) << overlay << ": keyring signed Plumtree simple payload with unexpected source";
    return;
  }
  if (payload.broadcast_id.is_zero()) {
    VLOG(PLUMTREE_WARNING) << overlay << ": empty Plumtree simple broadcast id";
    return;
  }
  if (payload.data.empty() || payload.data.size() > Overlays::max_fec_broadcast_size() ||
      payload.data_size != payload.data.size() || td::sha256_bits256(payload.data.as_slice()) != payload.data_hash) {
    VLOG(PLUMTREE_WARNING) << overlay << ": Plumtree signed simple payload has invalid data size";
    return;
  }
  if (auto status = validate_control_fields(payload.broadcast_id, 0, payload.tree_index); status.is_error()) {
    VLOG(PLUMTREE_WARNING) << overlay << ": Plumtree signed simple payload has invalid tree: " << status;
    return;
  }
  if (simple_broadcasts_.contains(payload.broadcast_id) || overlay->is_delivered(payload.broadcast_id)) {
    return;
  }

  auto cert = overlay->get_certificate(payload.source);
  auto check = overlay->check_source_eligible(payload.source, cert.get(), payload.data_size, /* is_fec = */ true,
                                              /* is_any_sender = */ payload.flags & Overlays::BroadcastFlagAnySender());
  if (check == BroadcastCheckResult::Forbidden) {
    VLOG(PLUMTREE_WARNING) << overlay << ": Plumtree simple source became ineligible before send";
    return;
  }

  auto state = std::make_unique<PlumtreeSimpleBroadcastState>();
  state->broadcast_id = payload.broadcast_id;
  state->flags = payload.flags;
  state->data = std::move(payload.data);
  state->first_seen_at = td::Timestamp::now();
  state->part.part_index = 0;
  state->part.tree_index = payload.tree_index;
  state->part.timestamp = payload.timestamp;
  state->part.data_size = payload.data_size;
  state->part.data_hash = payload.data_hash;
  state->part.source_key = std::move(signed_payload.second);
  state->part.source = payload.source;
  state->part.certificate = cert;
  state->part.signature = std::move(signed_payload.first);

  auto *state_ptr = state.get();
  simple_lru_.put(state_ptr);
  simple_broadcasts_.emplace(state_ptr->broadcast_id, std::move(state));

  overlay->get_broadcasts_limiter(state_ptr->part.source, cert.get()).register_broadcast(payload.data_size);
  forward_payload(overlay, state_ptr->broadcast_id, state_ptr->part, adnl::AdnlNodeIdShort::zero());
  overlay->register_delivered_broadcast(state_ptr->broadcast_id);
  VLOG(PLUMTREE_INFO) << overlay << ": Plumtree SIMPLE_SEND sender broadcast_id=" << state_ptr->broadcast_id.to_hex()
                      << " data_size=" << state_ptr->data.size() << " tree_index=" << state_ptr->part.tree_index
                      << " full_sent=" << state_ptr->part.full_sends;
  check_and_deliver(overlay, state_ptr->part.source, check, state_ptr->data.clone()).start().detach();
}

td::actor::Task<> BroadcastsPlumtree::Impl::process_fec_payload(
    OverlayImpl *overlay, adnl::AdnlNodeIdShort from, tl_object_ptr<ton_api::overlay_broadcastPlumtreeFec> msg,
    PlumtreePayloadOrigin origin, const MissingPartKey *expected_key) {
  if (from.is_zero()) {
    co_return td::Status::Error(ErrorCode::protoviolation, "missing Plumtree immediate sender");
  }
  if (options_.k_ == 0 || options_.parts_ == 0 || options_.tree_slots_ <= options_.parts_) {
    co_return td::Status::Error(ErrorCode::notready, "invalid Plumtree FEC configuration");
  }
  auto flags = static_cast<td::uint32>(msg->flags_);
  auto timestamp = msg->timestamp_;
  auto full_data_size = static_cast<td::uint32>(msg->full_data_size_);
  auto part_index = static_cast<td::uint32>(msg->part_index_);
  auto tree_index = static_cast<td::uint32>(msg->tree_index_);
  CO_TRY(check_timestamp(timestamp));
  if (full_data_size == 0 || full_data_size > Overlays::max_fec_broadcast_size() || msg->data_.empty()) {
    co_return td::Status::Error(ErrorCode::protoviolation, "invalid Plumtree FEC fields");
  }
  auto part_size = static_cast<td::uint32>(msg->data_.size());
  auto expected_part_size =
      static_cast<td::uint32>((static_cast<td::uint64>(full_data_size) + options_.k_ - 1) / options_.k_);
  if (part_size != expected_part_size) {
    co_return td::Status::Error(ErrorCode::protoviolation, "invalid Plumtree part size");
  }
  auto part_hash = td::sha256_bits256(msg->data_.as_slice());

  auto broadcast_id = compute_broadcast_id(flags, msg->full_data_hash_, full_data_size, part_size);
  CO_TRY(validate_control_fields(broadcast_id, part_index, tree_index));
  if (is_original_sender_) {
    if (auto *s = slot(tree_index)) {
      remove_eager(overlay, *s, from);
    }
    send_prune(overlay, from, broadcast_id, part_index, tree_index);
    co_return td::Status::Error(ErrorCode::notready, "original sender ignores Plumtree part");
  }
  MissingPartKey payload_key{broadcast_id, part_index, tree_index};
  auto *s = slot(tree_index);
  if (!s) {
    co_return td::Status::Error(ErrorCode::protoviolation, "invalid Plumtree slot");
  }
  auto origin_status = check_payload_origin(origin, expected_key, payload_key, *s, from);
  if (origin_status.is_error()) {
    if (origin == PlumtreePayloadOrigin::Push) {
      send_prune(overlay, from, broadcast_id, part_index, tree_index);
    }
    co_return std::move(origin_status);
  }
  PublicKey source_key(msg->src_);
  auto source_hash = source_key.compute_short_id();
  auto it = broadcasts_.find(broadcast_id);
  if (it == broadcasts_.end() && overlay->is_delivered(broadcast_id)) {
    co_return td::Status::Error(ErrorCode::notready, "known Plumtree broadcast");
  }
  bool new_broadcast = it == broadcasts_.end();
  if (!new_broadcast) {
    auto &state = *it->second;
    if (state.flags != flags || state.full_data_hash != msg->full_data_hash_ ||
        state.full_data_size != full_data_size || state.part_size != part_size) {
      co_return td::Status::Error(ErrorCode::protoviolation, "Plumtree broadcast id collision");
    }
  }

  auto cert = CO_TRY(Certificate::create(msg->certificate_));
  auto check = overlay->check_source_eligible(source_hash, cert.get(), full_data_size, /* is_fec = */ true,
                                              /* is_any_sender = */ flags & Overlays::BroadcastFlagAnySender(), from);
  if (check == BroadcastCheckResult::Forbidden) {
    co_return td::Status::Error(ErrorCode::protoviolation, "Plumtree source is not allowed");
  }

  if (new_broadcast) {
    CO_TRY(overlay->get_broadcasts_limiter(source_hash, cert.get()).precheck_new_broadcast(full_data_size));
    co_await overlay->precheck_broadcast(source_hash, broadcast_id, {}, false).trace("precheck Plumtree broadcast");
  }

  auto to_sign = make_fec_payload_to_sign(broadcast_id, timestamp, part_index, tree_index, part_size, part_hash);
  {
    TD_PERF_COUNTER(check_signature_overlay_broadcast_plumtree_fec_payload);
    CO_TRY(overlay->check_signature_from_peer(source_key, to_sign, msg->signature_, from));
  }

  PlumtreeFecBroadcastState *broadcast = nullptr;
  if (new_broadcast) {
    co_await overlay->precheck_broadcast(source_hash, broadcast_id, {}, true).trace("precheck Plumtree broadcast");
    CO_TRY(check_timestamp(timestamp));
    it = broadcasts_.find(broadcast_id);
    if (it == broadcasts_.end()) {
      CO_TRY(overlay->get_broadcasts_limiter(source_hash, cert.get()).try_register_broadcast(full_data_size));
      auto broadcast_ref = CO_TRY(get_or_create_fec_broadcast_state(broadcast_id, source_hash, check, flags,
                                                                    msg->full_data_hash_, full_data_size, part_size));
      broadcast = broadcast_ref.state;
      if (broadcast_ref.created) {
        VLOG(PLUMTREE_INFO) << overlay << ": Plumtree START receiver broadcast_id=" << broadcast_id.to_hex()
                            << " full_data_hash=" << msg->full_data_hash_.to_hex()
                            << " full_data_size=" << full_data_size << " from=" << from;
      }
    } else {
      auto &state = *it->second;
      if (state.flags != flags || state.full_data_hash != msg->full_data_hash_ ||
          state.full_data_size != full_data_size || state.part_size != part_size) {
        co_return td::Status::Error(ErrorCode::protoviolation, "Plumtree broadcast id collision");
      }
      broadcast = it->second.get();
    }
  } else {
    broadcast = it->second.get();
  }
  CO_TRY(ensure_decoder(*broadcast));
  note_eager_peer_active(from);

  if (auto *known = get_part(broadcast_id, part_index, tree_index)) {
    if (auto *s = slot(tree_index)) {
      remove_eager(overlay, *s, from);
    }
    send_prune(overlay, from, broadcast_id, known->part_index, known->tree_index);
    co_return td::Status::Error(ErrorCode::notready, "duplicate Plumtree part");
  }

  auto *part = add_fec_part_state(*broadcast, part_index, tree_index, timestamp, std::move(source_key), source_hash,
                                  cert, part_size, part_hash, std::move(msg->signature_), msg->data_.clone());
  erase_missing_part(payload_key);

  promote_eager(overlay, *s, from, true);
  send_useful(overlay, from, *part, broadcast_id);
  forward_payload(overlay, broadcast_id, *part, from);

  auto decoded = CO_TRY(decode_fec_part(overlay, *broadcast, part_index));
  if (!decoded.data.empty()) {
    co_await check_and_deliver(overlay, decoded.source, decoded.check_result, std::move(decoded.data))
        .trace("check Plumtree broadcast");
  }
  co_return td::Unit{};
}

td::actor::Task<> BroadcastsPlumtree::Impl::process_simple_payload(
    OverlayImpl *overlay, adnl::AdnlNodeIdShort from, tl_object_ptr<ton_api::overlay_broadcastPlumtreeSimple> msg,
    PlumtreePayloadOrigin origin, const MissingPartKey *expected_key) {
  if (from.is_zero()) {
    co_return td::Status::Error(ErrorCode::protoviolation, "missing Plumtree simple immediate sender");
  }
  auto flags = static_cast<td::uint32>(msg->flags_);
  auto timestamp = msg->timestamp_;
  auto broadcast_id = msg->broadcast_id_;
  td::uint32 part_index = 0;
  auto tree_index = static_cast<td::uint32>(msg->tree_index_);
  CO_TRY(check_timestamp(timestamp));
  if (broadcast_id.is_zero()) {
    co_return td::Status::Error(ErrorCode::protoviolation, "empty Plumtree simple broadcast id");
  }
  CO_TRY(validate_control_fields(broadcast_id, part_index, tree_index));
  if (msg->data_.empty() || msg->data_.size() > Overlays::max_fec_broadcast_size()) {
    co_return td::Status::Error(ErrorCode::protoviolation, "invalid Plumtree simple payload size");
  }
  if (is_original_sender_) {
    if (auto *s = slot(tree_index)) {
      remove_eager(overlay, *s, from);
    }
    send_prune(overlay, from, broadcast_id, part_index, tree_index);
    co_return td::Status::Error(ErrorCode::notready, "original sender ignores Plumtree simple payload");
  }
  MissingPartKey payload_key{broadcast_id, part_index, tree_index};
  auto *s = slot(tree_index);
  if (!s) {
    co_return td::Status::Error(ErrorCode::protoviolation, "invalid Plumtree slot");
  }
  auto origin_status = check_payload_origin(origin, expected_key, payload_key, *s, from);
  if (origin_status.is_error()) {
    if (origin == PlumtreePayloadOrigin::Push) {
      send_prune(overlay, from, broadcast_id, part_index, tree_index);
    }
    co_return std::move(origin_status);
  }

  auto data_size = static_cast<td::uint32>(msg->data_.size());
  auto data_hash = td::sha256_bits256(msg->data_.as_slice());
  auto it = simple_broadcasts_.find(broadcast_id);
  if (it == simple_broadcasts_.end() && overlay->is_delivered(broadcast_id)) {
    co_return td::Status::Error(ErrorCode::notready, "known Plumtree simple broadcast");
  }
  bool new_broadcast = it == simple_broadcasts_.end();

  PublicKey source_key(msg->src_);
  auto source_hash = source_key.compute_short_id();
  auto cert = CO_TRY(Certificate::create(msg->certificate_));
  auto check = overlay->check_source_eligible(source_hash, cert.get(), data_size, /* is_fec = */ true,
                                              /* is_any_sender = */ flags & Overlays::BroadcastFlagAnySender(), from);
  if (check == BroadcastCheckResult::Forbidden) {
    co_return td::Status::Error(ErrorCode::protoviolation, "Plumtree simple source is not allowed");
  }

  auto &limiter = overlay->get_broadcasts_limiter(source_hash, cert.get());
  if (new_broadcast) {
    CO_TRY(limiter.precheck_new_broadcast(data_size));
    co_await overlay->precheck_broadcast(source_hash, broadcast_id, {}, false)
        .trace("precheck Plumtree simple broadcast");
  }

  auto to_sign = make_simple_payload_to_sign(broadcast_id, timestamp, tree_index, data_size, data_hash);
  {
    TD_PERF_COUNTER(check_signature_overlay_broadcast_plumtree_simple_payload);
    CO_TRY(overlay->check_signature_from_peer(source_key, to_sign, msg->signature_, from));
  }
  note_eager_peer_active(from);

  if (auto *known = get_part(broadcast_id, part_index, tree_index)) {
    if (auto *s = slot(tree_index)) {
      remove_eager(overlay, *s, from);
    }
    send_prune(overlay, from, broadcast_id, known->part_index, known->tree_index);
    co_return td::Status::Error(ErrorCode::notready, "duplicate Plumtree simple payload");
  }

  co_await overlay->precheck_broadcast(source_hash, broadcast_id, {}, true).trace("precheck Plumtree simple broadcast");
  CO_TRY(check_timestamp(timestamp));
  if (auto *known = get_part(broadcast_id, part_index, tree_index)) {
    if (auto *s = slot(tree_index)) {
      remove_eager(overlay, *s, from);
    }
    send_prune(overlay, from, broadcast_id, known->part_index, known->tree_index);
    co_return td::Status::Error(ErrorCode::notready, "duplicate Plumtree simple payload");
  }
  if (!has_state(broadcast_id) && overlay->is_delivered(broadcast_id)) {
    co_return td::Status::Error(ErrorCode::notready, "known Plumtree simple broadcast");
  }

  CO_TRY(limiter.try_register_broadcast(data_size));

  auto state = std::make_unique<PlumtreeSimpleBroadcastState>();
  state->broadcast_id = broadcast_id;
  state->flags = flags;
  state->data = std::move(msg->data_);
  state->first_seen_at = td::Timestamp::now();
  state->part.part_index = part_index;
  state->part.tree_index = tree_index;
  state->part.timestamp = timestamp;
  state->part.data_size = data_size;
  state->part.data_hash = data_hash;
  state->part.source_key = std::move(source_key);
  state->part.source = source_hash;
  state->part.certificate = cert;
  state->part.signature = std::move(msg->signature_);

  auto *state_ptr = state.get();
  simple_lru_.put(state_ptr);
  simple_broadcasts_.emplace(broadcast_id, std::move(state));
  erase_missing_part(payload_key);
  VLOG(PLUMTREE_INFO) << overlay << ": Plumtree SIMPLE_RECV broadcast_id=" << state_ptr->broadcast_id.to_hex()
                      << " tree_index=" << state_ptr->part.tree_index << " data_size=" << state_ptr->data.size()
                      << " from=" << from;

  promote_eager(overlay, *s, from, true);
  send_useful(overlay, from, state_ptr->part, broadcast_id);
  forward_payload(overlay, broadcast_id, state_ptr->part, from);
  overlay->register_delivered_broadcast(state_ptr->broadcast_id);
  co_await check_and_deliver(overlay, state_ptr->part.source, check, state_ptr->data.clone())
      .trace("check Plumtree simple broadcast");
  co_return td::Unit{};
}

td::actor::Task<> BroadcastsPlumtree::Impl::process_ihave(OverlayImpl *overlay, adnl::AdnlNodeIdShort from,
                                                          tl_object_ptr<ton_api::overlay_broadcastPlumtreeIHave> msg) {
  if (is_original_sender_) {
    co_return td::Unit{};
  }
  if (from.is_zero()) {
    co_return td::Status::Error(ErrorCode::protoviolation, "missing Plumtree immediate sender");
  }
  auto control = get_control_fields(*msg);
  CO_TRY(check_timestamp(control.timestamp));
  CO_TRY(validate_control_fields(control.broadcast_id, control.part_index, control.tree_index));
  auto part_index = control.part_index;
  auto tree_index = control.tree_index;

  if (!has_state(control.broadcast_id) && overlay->is_delivered(control.broadcast_id)) {
    co_return td::Unit{};
  }
  if (get_part(control.broadcast_id, part_index, tree_index)) {
    co_return td::Unit{};
  }
  auto *s = slot(tree_index);
  if (!s) {
    co_return td::Status::Error(ErrorCode::protoviolation, "invalid Plumtree slot");
  }
  CO_TRY(check_timestamp(msg->payload_timestamp_));
  if (msg->data_size_ <= 0) {
    co_return td::Status::Error(ErrorCode::protoviolation, "invalid Plumtree IHAVE data size");
  }
  auto data_size = static_cast<td::uint32>(msg->data_size_);
  if (data_size > Overlays::max_fec_broadcast_size()) {
    co_return td::Status::Error(ErrorCode::protoviolation, "too large Plumtree IHAVE data size");
  }
  if (tree_index != PLUMTREE_SIMPLE_TREE_INDEX) {
    if (options_.k_ == 0) {
      co_return td::Status::Error(ErrorCode::notready, "invalid Plumtree FEC configuration");
    }
    auto max_fec_part_size = static_cast<td::uint32>(
        (static_cast<td::uint64>(Overlays::max_fec_broadcast_size()) + options_.k_ - 1) / options_.k_);
    if (data_size > max_fec_part_size) {
      co_return td::Status::Error(ErrorCode::protoviolation, "too large Plumtree IHAVE FEC part size");
    }
  }

  PublicKey source_key(msg->src_);
  auto source_hash = source_key.compute_short_id();
  auto cert = CO_TRY(Certificate::create(msg->certificate_));
  if (overlay->check_source_eligible(source_hash, cert.get(), data_size, /* is_fec = */ true,
                                     /* is_any_sender = */ false, from) != BroadcastCheckResult::Allowed) {
    co_return td::Status::Error(ErrorCode::protoviolation, "Plumtree IHAVE source is not allowed");
  }

  td::BufferSlice to_sign;
  if (tree_index == PLUMTREE_SIMPLE_TREE_INDEX) {
    to_sign = make_simple_payload_to_sign(control.broadcast_id, msg->payload_timestamp_, tree_index, data_size,
                                          msg->data_hash_);
  } else {
    to_sign = make_fec_payload_to_sign(control.broadcast_id, msg->payload_timestamp_, part_index, tree_index, data_size,
                                       msg->data_hash_);
  }
  CO_TRY(overlay->check_signature_from_peer(source_key, to_sign, msg->signature_, from));

  MissingPartKey key{control.broadcast_id, part_index, tree_index};
  auto *missing = get_or_create_missing_part(key, data_size);
  if (!missing->repair_at) {
    missing->repair_at = td::Timestamp::in(options_.repair_timeout_ms_ / 1000.0);
    overlay->relax_plumtree_alarm(missing->repair_at);
  }
  if (std::find(missing->repair_targets.begin(), missing->repair_targets.end(), from) ==
          missing->repair_targets.end() &&
      missing->repair_targets.size() < options_.max_repair_targets_) {
    missing->repair_targets.push_back(from);
  }
  if (local_eager_limit_ == 0 || s->eager.empty()) {
    send_repair_requests(overlay, key, *missing);
  }
  co_return td::Unit{};
}

void BroadcastsPlumtree::Impl::process_repair_query(OverlayImpl *overlay, adnl::AdnlNodeIdShort from,
                                                    ton_api::overlay_repairPlumtreePart &query,
                                                    td::Promise<td::BufferSlice> promise) {
  if (from.is_zero()) {
    promise.set_error(td::Status::Error(ErrorCode::protoviolation, "missing Plumtree repair requester"));
    return;
  }
  auto broadcast_id = query.broadcast_id_;
  auto timestamp = query.timestamp_;
  auto part_index = static_cast<td::uint32>(query.part_index_);
  auto tree_index = static_cast<td::uint32>(query.tree_index_);
  if (auto status = check_timestamp(timestamp); status.is_error()) {
    promise.set_error(std::move(status));
    return;
  }
  if (auto status = validate_control_fields(broadcast_id, part_index, tree_index); status.is_error()) {
    promise.set_error(std::move(status));
    return;
  }

  if (!has_state(broadcast_id)) {
    if (overlay->is_delivered(broadcast_id)) {
      promise.set_value(create_serialize_tl_object<ton_api::overlay_broadcastNotFound>());
    } else {
      promise.set_error(td::Status::Error(ErrorCode::protoviolation, "unknown Plumtree repair broadcast"));
    }
    return;
  }
  auto *part = get_part(broadcast_id, part_index, tree_index);
  if (!part) {
    promise.set_error(td::Status::Error(ErrorCode::protoviolation, "Plumtree repair for unknown part"));
    return;
  }
  if (!part->advertised_to.contains(from)) {
    promise.set_error(td::Status::Error(ErrorCode::protoviolation, "Plumtree repair without IHAVE"));
    return;
  }
  if (part->full_sent_to.contains(from)) {
    promise.set_error(td::Status::Error(ErrorCode::protoviolation, "duplicate Plumtree repair"));
    return;
  }
  auto &s = slots_[tree_index];
  if (!can_reserve_eager_feedback(s, from) || part->full_sends >= local_eager_limit_) {
    promise.set_value(create_serialize_tl_object<ton_api::overlay_broadcastNotFound>());
    return;
  }
  auto wire = prepare_payload_for_peer(overlay, broadcast_id, *part, from);
  if (wire.is_error()) {
    promise.set_error(wire.move_as_error());
    return;
  }
  promise.set_value(wire.move_as_ok());
}

void BroadcastsPlumtree::Impl::repair_query_finished() {
  CHECK(active_repair_queries_ > 0);
  --active_repair_queries_;
}

td::actor::Task<> BroadcastsPlumtree::Impl::process_repair_response(OverlayImpl *overlay, adnl::AdnlNodeIdShort from,
                                                                    MissingPartKey expected_key, td::BufferSlice data) {
  if (from.is_zero()) {
    co_return td::Status::Error(ErrorCode::protoviolation, "missing Plumtree repair response sender");
  }
  auto bcast = fetch_tl_object<ton_api::overlay_Broadcast>(std::move(data), true);
  if (bcast.is_error()) {
    co_return bcast.move_as_error_prefix("invalid Plumtree repair response: ");
  }
  auto obj = bcast.move_as_ok();
  switch (obj->get_id()) {
    case ton_api::overlay_broadcastPlumtreeFec::ID:
      co_await process_fec_payload(overlay, from, move_tl_object_as<ton_api::overlay_broadcastPlumtreeFec>(obj),
                                   PlumtreePayloadOrigin::RepairResponse, &expected_key);
      break;
    case ton_api::overlay_broadcastPlumtreeSimple::ID:
      co_await process_simple_payload(overlay, from, move_tl_object_as<ton_api::overlay_broadcastPlumtreeSimple>(obj),
                                      PlumtreePayloadOrigin::RepairResponse, &expected_key);
      break;
    case ton_api::overlay_broadcastNotFound::ID:
      break;
    default:
      co_return td::Status::Error(ErrorCode::protoviolation, "unexpected Plumtree repair response");
  }
  co_return td::Unit{};
}

td::actor::Task<> BroadcastsPlumtree::Impl::process_prune(OverlayImpl *overlay, adnl::AdnlNodeIdShort from,
                                                          tl_object_ptr<ton_api::overlay_broadcastPlumtreePrune> msg) {
  if (from.is_zero()) {
    co_return td::Status::Error(ErrorCode::protoviolation, "missing Plumtree immediate sender");
  }
  auto control = get_control_fields(*msg);
  CO_TRY(check_timestamp(control.timestamp));
  CO_TRY(validate_control_fields(control.broadcast_id, control.part_index, control.tree_index));
  auto part_index = control.part_index;
  auto tree_index = control.tree_index;

  if (!has_state(control.broadcast_id) && overlay->is_delivered(control.broadcast_id)) {
    co_return td::Unit{};
  }
  auto *part = get_part(control.broadcast_id, part_index, tree_index);
  auto *s = slot(tree_index);
  if (!part || !s) {
    co_return td::Unit{};
  }
  if (!part->full_sent_to.contains(from)) {
    co_return td::Unit{};
  }
  note_eager_peer_active(from);
  remove_eager(overlay, *s, from);
  co_return td::Unit{};
}

td::actor::Task<> BroadcastsPlumtree::Impl::process_useful(
    OverlayImpl *overlay, adnl::AdnlNodeIdShort from, tl_object_ptr<ton_api::overlay_broadcastPlumtreeUseful> msg) {
  if (from.is_zero()) {
    co_return td::Status::Error(ErrorCode::protoviolation, "missing Plumtree immediate sender");
  }
  auto control = get_control_fields(*msg);
  CO_TRY(check_timestamp(control.timestamp));
  CO_TRY(validate_control_fields(control.broadcast_id, control.part_index, control.tree_index));
  auto part_index = control.part_index;
  auto tree_index = control.tree_index;

  if (!has_state(control.broadcast_id) && overlay->is_delivered(control.broadcast_id)) {
    co_return td::Unit{};
  }
  auto *part = get_part(control.broadcast_id, part_index, tree_index);
  auto *s = slot(tree_index);
  if (!part || !s || !part->full_sent_to.contains(from)) {
    co_return td::Unit{};
  }
  note_eager_peer_active(from);
  bool was_pending = s->pending_feedback.erase(from) > 0;
  if (was_pending) {
    promote_eager(overlay, *s, from, false);
  }
  co_return td::Unit{};
}

void BroadcastsPlumtree::Impl::alarm(OverlayImpl *overlay) {
  expire_pending_feedback();
  auto now = td::Timestamp::now();
  while (auto *missing = oldest_missing_part()) {
    if (!missing->repair_at) {
      erase_missing_part(missing->key);
      continue;
    }
    if (!missing->repair_at.is_in_past(now)) {
      break;
    }
    auto key = missing->key;
    send_repair_requests(overlay, key, *missing);
    erase_missing_part(key);
  }
}

td::Timestamp BroadcastsPlumtree::Impl::next_alarm_at() {
  auto *missing = oldest_missing_part();
  return missing ? missing->repair_at : td::Timestamp::never();
}

void BroadcastsPlumtree::Impl::gc(OverlayImpl *overlay) {
  expire_pending_feedback();
  auto now = td::Timestamp::now();
  auto broadcast_expired_before = now - PLUMTREE_BROADCAST_TTL;
  while (!broadcasts_.empty()) {
    auto *bcast = static_cast<PlumtreeFecBroadcastState *>(lru_.prev);
    CHECK(bcast);
    if (bcast->first_seen_at > broadcast_expired_before) {
      break;
    }
    auto broadcast_id = bcast->broadcast_id;
    if (!bcast->delivered) {
      VLOG(PLUMTREE_INFO) << overlay << ": Plumtree GC_INCOMPLETE broadcast_id=" << broadcast_id.to_hex()
                          << " tree_parts=" << bcast->tree_parts.size()
                          << " payload_parts=" << bcast->parts_by_index.size()
                          << " decoder_parts=" << bcast->decoder_parts.size();
    }
    CHECK(broadcasts_.erase(broadcast_id));
    overlay->register_delivered_broadcast(broadcast_id);
  }

  while (!simple_broadcasts_.empty()) {
    auto *bcast = static_cast<PlumtreeSimpleBroadcastState *>(simple_lru_.prev);
    CHECK(bcast);
    if (bcast->first_seen_at > broadcast_expired_before) {
      break;
    }
    auto broadcast_id = bcast->broadcast_id;
    CHECK(simple_broadcasts_.erase(broadcast_id));
    overlay->register_delivered_broadcast(broadcast_id);
  }
}

BroadcastsPlumtree::BroadcastsPlumtree(PlumtreeFecOptions options, bool is_original_sender)
    : impl_(std::make_unique<Impl>(options, is_original_sender)) {
}

BroadcastsPlumtree::~BroadcastsPlumtree() = default;

void BroadcastsPlumtree::init_sender(td::actor::ActorId<adnl::AdnlSenderInterface> sender) {
  impl_->init_sender(sender);
}

void BroadcastsPlumtree::send_fec(OverlayImpl *overlay, PublicKeyHash send_as, td::uint32 flags, td::BufferSlice data) {
  impl_->send_fec(overlay, send_as, flags, std::move(data));
}

void BroadcastsPlumtree::send(OverlayImpl *overlay, PublicKeyHash send_as, td::uint32 flags, td::Bits256 broadcast_id,
                              td::BufferSlice data) {
  impl_->send(overlay, send_as, flags, broadcast_id, std::move(data));
}

void BroadcastsPlumtree::signed_fec(OverlayImpl *overlay, PlumtreeOutboundFecPayload &&payload,
                                    td::Result<std::pair<td::BufferSlice, PublicKey>> &&R) {
  impl_->signed_fec(overlay, std::move(payload), std::move(R));
}

void BroadcastsPlumtree::signed_simple(OverlayImpl *overlay, PlumtreeOutboundSimplePayload &&payload,
                                       td::Result<std::pair<td::BufferSlice, PublicKey>> &&R) {
  impl_->signed_simple(overlay, std::move(payload), std::move(R));
}

td::actor::Task<> BroadcastsPlumtree::process_fec_payload(OverlayImpl *overlay, adnl::AdnlNodeIdShort from,
                                                          tl_object_ptr<ton_api::overlay_broadcastPlumtreeFec> msg) {
  co_await impl_->process_fec_payload(overlay, from, std::move(msg));
  co_return td::Unit{};
}

td::actor::Task<> BroadcastsPlumtree::process_simple_payload(
    OverlayImpl *overlay, adnl::AdnlNodeIdShort from, tl_object_ptr<ton_api::overlay_broadcastPlumtreeSimple> msg) {
  co_await impl_->process_simple_payload(overlay, from, std::move(msg));
  co_return td::Unit{};
}

td::actor::Task<> BroadcastsPlumtree::process_ihave(OverlayImpl *overlay, adnl::AdnlNodeIdShort from,
                                                    tl_object_ptr<ton_api::overlay_broadcastPlumtreeIHave> msg) {
  co_await impl_->process_ihave(overlay, from, std::move(msg));
  co_return td::Unit{};
}

void BroadcastsPlumtree::process_repair_query(OverlayImpl *overlay, adnl::AdnlNodeIdShort from,
                                              ton_api::overlay_repairPlumtreePart &query,
                                              td::Promise<td::BufferSlice> promise) {
  impl_->process_repair_query(overlay, from, query, std::move(promise));
}

void BroadcastsPlumtree::repair_query_finished() {
  impl_->repair_query_finished();
}

td::actor::Task<> BroadcastsPlumtree::process_repair_response(OverlayImpl *overlay, adnl::AdnlNodeIdShort from,
                                                              const td::Bits256 &expected_broadcast_id,
                                                              td::uint32 expected_part_index,
                                                              td::uint32 expected_tree_index, td::BufferSlice data) {
  co_await impl_->process_repair_response(
      overlay, from, MissingPartKey{expected_broadcast_id, expected_part_index, expected_tree_index}, std::move(data));
  co_return td::Unit{};
}

td::actor::Task<> BroadcastsPlumtree::process_prune(OverlayImpl *overlay, adnl::AdnlNodeIdShort from,
                                                    tl_object_ptr<ton_api::overlay_broadcastPlumtreePrune> msg) {
  co_await impl_->process_prune(overlay, from, std::move(msg));
  co_return td::Unit{};
}

td::actor::Task<> BroadcastsPlumtree::process_useful(OverlayImpl *overlay, adnl::AdnlNodeIdShort from,
                                                     tl_object_ptr<ton_api::overlay_broadcastPlumtreeUseful> msg) {
  co_await impl_->process_useful(overlay, from, std::move(msg));
  co_return td::Unit{};
}

void BroadcastsPlumtree::alarm(OverlayImpl *overlay) {
  impl_->alarm(overlay);
}

td::Timestamp BroadcastsPlumtree::next_alarm_at() {
  return impl_->next_alarm_at();
}

void BroadcastsPlumtree::gc(OverlayImpl *overlay) {
  impl_->gc(overlay);
}

}  // namespace overlay

}  // namespace ton
