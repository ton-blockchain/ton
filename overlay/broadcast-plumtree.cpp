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

constexpr td::Slice PLUMTREE_ROOT_PRF_DOMAIN{"ton-overlay-plumtree-root-v1"};
constexpr td::Slice PLUMTREE_BROADCAST_PRF_DOMAIN{"ton-overlay-plumtree-broadcast-v1"};
constexpr double PLUMTREE_BROADCAST_TTL = 25.0;
constexpr double PLUMTREE_PENDING_FEEDBACK_TTL = 5.0;
constexpr double PLUMTREE_MISSING_PART_TTL_REPAIR_TIMEOUTS = 10.0;
constexpr std::size_t MAX_SPECULATIVE_REPAIR_PARTS = 4096;

struct PlumtreeSlot {
  bool initialized = false;
  std::set<adnl::AdnlNodeIdShort> eager;
  std::map<adnl::AdnlNodeIdShort, td::Timestamp> pending_feedback;
};

struct PlumtreePartState {
  td::uint32 part_index = 0;
  td::uint32 tree_index = 0;
  double timestamp = 0.0;
  PublicKey source_key;
  PublicKeyHash source;
  std::shared_ptr<Certificate> certificate;
  td::BufferSlice signature;
  td::uint32 full_sends = 0;
  std::set<adnl::AdnlNodeIdShort> advertised_to;
  std::set<adnl::AdnlNodeIdShort> full_sent_to;
};

using TreePartKey = std::pair<td::uint32, td::uint32>;
using MissingPartKey = std::tuple<td::Bits256, td::uint32, td::uint32>;

struct PlumtreeBroadcastState : td::ListNode {
  td::Bits256 broadcast_id;
  PublicKeyHash first_valid_source;
  td::Bits256 data_hash;
  td::Timestamp first_seen_at;
  td::uint32 flags = 0;
  td::uint32 data_size = 0;
  td::uint32 part_size = 0;
  bool delivered = false;

  std::unique_ptr<td::raptorq::Decoder> decoder;
  std::set<td::uint32> decoder_parts;
  bool overlay_limiter_registered = false;
  std::map<td::uint32, td::BufferSlice> parts_by_index;
  std::map<TreePartKey, PlumtreePartState> tree_parts;
};

struct PlumtreeMissingPart {
  std::vector<adnl::AdnlNodeIdShort> repair_targets;
  td::Timestamp repair_at = td::Timestamp::never();
  td::Timestamp created_at = td::Timestamp::now();
  bool repair_sent = false;
};

void normalize_schedule_sources(std::vector<PublicKeyHash> &sources) {
  std::sort(sources.begin(), sources.end());
  sources.erase(std::unique(sources.begin(), sources.end()), sources.end());
}

class PlumtreeSchedule {
 public:
  PlumtreeSchedule(OverlayIdShort overlay_id, PlumtreeFecOptions options, std::vector<PublicKeyHash> sources)
      : overlay_id_(overlay_id), options_(options) {
    normalize_schedule_sources(sources);
    if (!sources.empty() && sources.size() <= options_.tree_slots_) {
      build_tree_assignments(sources);
    }
  }

  td::Result<std::vector<TreePartKey>> assigned_parts(const td::Bits256 &broadcast_id, PublicKeyHash source,
                                                      td::uint32 local_validator_index,
                                                      td::uint32 validator_count) const {
    if (!valid_) {
      return td::Status::Error(ErrorCode::notready, "invalid Plumtree schedule inputs");
    }
    if (validator_count == 0 || local_validator_index >= validator_count) {
      return td::Status::Error(ErrorCode::protoviolation, "invalid validator group index");
    }
    auto it = tree_by_source_.find(source);
    if (it == tree_by_source_.end()) {
      return td::Status::Error(ErrorCode::notready, "local Plumtree source is not in the schedule");
    }
    auto tree_index = it->second;
    auto parts = broadcast_part_order(broadcast_id);
    std::vector<TreePartKey> result;
    result.reserve(options_.parts_);
    for (td::uint32 pos = 0; pos < parts.size(); ++pos) {
      if (pos % validator_count == local_validator_index) {
        result.emplace_back(parts[pos], tree_index);
      }
    }
    return result;
  }

 private:
  struct PrfSeed {
    td::Slice domain;
    td::Bits256 overlay_id;
    td::Slice broadcast_id;

    template <class StorerT>
    void store(StorerT &storer) const {
      td::store(domain, storer);
      td::store(overlay_id.as_slice(), storer);
      if (!broadcast_id.empty()) {
        td::store(broadcast_id, storer);
      }
    }
  };

  struct PrfInput {
    td::Slice seed;
    td::Slice label;
    td::uint32 index = 0;
    td::Slice value;

    template <class StorerT>
    void store(StorerT &storer) const {
      td::store(seed, storer);
      td::store(label, storer);
      td::store(index, storer);
      td::store(value, storer);
    }
  };

  void build_tree_assignments(const std::vector<PublicKeyHash> &sources) {
    auto seed = root_prf_seed();
    auto source_count = static_cast<td::uint32>(sources.size());
    std::vector<bool> tree_assigned(options_.tree_slots_);
    for (td::uint32 source_index = 0; source_index < source_count; ++source_index) {
      td::uint32 best_tree = options_.tree_slots_;
      td::Bits256 best_score;
      for (td::uint32 tree_index = 0; tree_index < options_.tree_slots_; ++tree_index) {
        if (tree_assigned[tree_index]) {
          continue;
        }
        auto score = prf_hash(seed, td::Slice("tree-root"), tree_index, sources[source_index].as_slice());
        if (best_tree == options_.tree_slots_ || score < best_score) {
          best_tree = tree_index;
          best_score = score;
        }
      }
      CHECK(best_tree < options_.tree_slots_);
      tree_by_source_[sources[source_index]] = best_tree;
      tree_assigned[best_tree] = true;
    }
    valid_ = true;
  }

  std::string root_prf_seed() const {
    return td::serialize(PrfSeed{PLUMTREE_ROOT_PRF_DOMAIN, overlay_id_.bits256_value(), {}});
  }

  std::string broadcast_prf_seed(const td::Bits256 &broadcast_id) const {
    return td::serialize(PrfSeed{PLUMTREE_BROADCAST_PRF_DOMAIN, overlay_id_.bits256_value(), broadcast_id.as_slice()});
  }

  std::vector<td::uint32> broadcast_part_order(const td::Bits256 &broadcast_id) const {
    return prf_permutation(broadcast_prf_seed(broadcast_id), td::Slice("part"), options_.parts_);
  }

  static td::Bits256 prf_hash(td::Slice seed, td::Slice label, td::uint32 index, td::Slice value = {}) {
    return td::sha256_bits256(td::serialize(PrfInput{seed, label, index, value}));
  }

  static std::vector<td::uint32> prf_permutation(const std::string &seed, td::Slice label, td::uint32 count) {
    std::vector<std::pair<td::Bits256, td::uint32>> scored;
    scored.reserve(count);
    for (td::uint32 i = 0; i < count; ++i) {
      scored.emplace_back(prf_hash(seed, label, i), i);
    }
    std::sort(scored.begin(), scored.end());

    std::vector<td::uint32> result;
    result.reserve(scored.size());
    for (const auto &[_, index] : scored) {
      result.push_back(index);
    }
    return result;
  }

  OverlayIdShort overlay_id_;
  PlumtreeFecOptions options_;
  std::map<PublicKeyHash, td::uint32> tree_by_source_;
  bool valid_ = false;
};

struct PlumtreeControlFields {
  td::Bits256 broadcast_id;
  double timestamp = 0.0;
  td::uint32 part_index = 0;
  td::uint32 tree_index = 0;
};

template <class T>
PlumtreeControlFields get_control_fields(const T &msg) {
  return PlumtreeControlFields{msg.broadcast_id_, msg.timestamp_, static_cast<td::uint32>(msg.part_index_),
                               static_cast<td::uint32>(msg.tree_index_)};
}

td::Bits256 compute_broadcast_id(td::uint32 flags, const td::Bits256 &data_hash, td::uint32 data_size,
                                 td::uint32 part_size) {
  return get_tl_object_sha_bits256(create_tl_object<ton_api::overlay_broadcastPlumtree_id>(
      static_cast<std::int32_t>(flags), data_hash, static_cast<std::int32_t>(data_size),
      static_cast<std::int32_t>(part_size)));
}

td::BufferSlice make_payload_to_sign(const td::Bits256 &broadcast_id, double timestamp, td::uint32 part_index,
                                     td::uint32 tree_index, const td::BufferSlice &part) {
  return create_serialize_tl_object<ton_api::overlay_broadcastPlumtreePayload_toSign>(
      broadcast_id, timestamp, static_cast<std::int32_t>(part_index), static_cast<std::int32_t>(tree_index),
      part.clone());
}

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

}  // namespace

class BroadcastsPlumtree::Impl {
 public:
  explicit Impl(PlumtreeFecOptions options) : options_(options) {
    if (options_.tree_slots_ != 0) {
      slots_.resize(options_.tree_slots_);
    }
  }

  void init_sender(td::actor::ActorId<adnl::AdnlSenderInterface> sender) {
    sender_ = sender;
  }

  void send(OverlayImpl *overlay, PublicKeyHash send_as, td::uint32 flags, td::BufferSlice data,
            td::uint32 local_validator_index, td::uint32 validator_count);
  void signed_payload(OverlayImpl *overlay, PlumtreeOutboundPayload &&payload,
                      td::Result<std::pair<td::BufferSlice, PublicKey>> &&R);

  td::actor::Task<> process_payload(OverlayImpl *overlay, adnl::AdnlNodeIdShort from,
                                    tl_object_ptr<ton_api::overlay_broadcastPlumtreePayload> msg);
  td::actor::Task<> process_ihave(OverlayImpl *overlay, adnl::AdnlNodeIdShort from,
                                  tl_object_ptr<ton_api::overlay_broadcastPlumtreeIHave> msg);
  td::actor::Task<> process_repair(OverlayImpl *overlay, adnl::AdnlNodeIdShort from,
                                   tl_object_ptr<ton_api::overlay_broadcastPlumtreeRepair> msg);
  td::actor::Task<> process_prune(OverlayImpl *overlay, adnl::AdnlNodeIdShort from,
                                  tl_object_ptr<ton_api::overlay_broadcastPlumtreePrune> msg);
  td::actor::Task<> process_useful(OverlayImpl *overlay, adnl::AdnlNodeIdShort from,
                                   tl_object_ptr<ton_api::overlay_broadcastPlumtreeUseful> msg);

  void alarm(OverlayImpl *overlay);
  td::Timestamp next_alarm_at() const;
  void gc(OverlayImpl *overlay);

 private:
  td::actor::ActorId<adnl::AdnlSenderInterface> sender_;
  PlumtreeFecOptions options_;

  std::vector<PlumtreeSlot> slots_;
  std::map<td::Bits256, std::unique_ptr<PlumtreeBroadcastState>> broadcasts_;
  std::map<MissingPartKey, PlumtreeMissingPart> missing_parts_;
  td::ListNode lru_;
  std::unique_ptr<PlumtreeSchedule> schedule_;
  std::vector<PublicKeyHash> schedule_sources_;

  td::Status validate_control_fields(const td::Bits256 &broadcast_id, td::uint32 part_index,
                                     td::uint32 tree_index) const;
  PlumtreeSchedule *schedule(OverlayImpl *overlay);

  PlumtreeSlot *slot(td::uint32 tree_index);
  td::uint32 slot_load(const PlumtreeSlot &slot) const;
  void expire_pending_feedback(PlumtreeSlot &slot) const;
  void expire_pending_feedback();
  void promote_eager(PlumtreeSlot &slot, adnl::AdnlNodeIdShort peer, bool force);
  void trim_eager_to_capacity(PlumtreeSlot &slot);
  void remove_eager(PlumtreeSlot &slot, adnl::AdnlNodeIdShort peer);

  PlumtreeBroadcastState *get_state(const td::Bits256 &broadcast_id);
  PlumtreePartState *get_part(const td::Bits256 &broadcast_id, td::uint32 part_index, td::uint32 tree_index);
  td::Status ensure_decoder(PlumtreeBroadcastState &broadcast);

  bool send_control(OverlayImpl *overlay, const adnl::AdnlNodeIdShort &dst,
                    tl_object_ptr<ton_api::overlay_Broadcast> control);
  bool send_payload_to(OverlayImpl *overlay, PlumtreeBroadcastState &broadcast, PlumtreePartState &part,
                       const adnl::AdnlNodeIdShort &dst);
  void send_ihave_to(OverlayImpl *overlay, PlumtreePartState &part, const td::Bits256 &broadcast_id,
                     const adnl::AdnlNodeIdShort &dst);
  void send_prune(OverlayImpl *overlay, const adnl::AdnlNodeIdShort &dst, const PlumtreePartState &part,
                  const td::Bits256 &broadcast_id);
  void send_useful(OverlayImpl *overlay, const adnl::AdnlNodeIdShort &dst, const PlumtreePartState &part,
                   const td::Bits256 &broadcast_id);
  void forward_payload(OverlayImpl *overlay, PlumtreeBroadcastState &broadcast, PlumtreePartState &part,
                       adnl::AdnlNodeIdShort from);

  void trim_missing_parts();
};

td::Status BroadcastsPlumtree::Impl::validate_control_fields(const td::Bits256 &broadcast_id, td::uint32 part_index,
                                                             td::uint32 tree_index) const {
  if (broadcast_id.is_zero()) {
    return td::Status::Error(ErrorCode::protoviolation, "empty Plumtree broadcast id");
  }
  if (part_index >= options_.parts_ || tree_index >= options_.tree_slots_) {
    return td::Status::Error(ErrorCode::protoviolation, "invalid Plumtree part or tree index");
  }
  return td::Status::OK();
}

PlumtreeSchedule *BroadcastsPlumtree::Impl::schedule(OverlayImpl *overlay) {
  auto sources = overlay->get_authorized_broadcast_sources();
  normalize_schedule_sources(sources);
  if (!schedule_ || sources != schedule_sources_) {
    schedule_sources_ = sources;
    schedule_ = std::make_unique<PlumtreeSchedule>(overlay->overlay_id(), options_, schedule_sources_);
  }
  return schedule_.get();
}

PlumtreeSlot *BroadcastsPlumtree::Impl::slot(td::uint32 tree_index) {
  return tree_index < slots_.size() ? &slots_[tree_index] : nullptr;
}

td::uint32 BroadcastsPlumtree::Impl::slot_load(const PlumtreeSlot &slot) const {
  td::uint32 load = static_cast<td::uint32>(slot.eager.size());
  for (const auto &[peer, _] : slot.pending_feedback) {
    if (!slot.eager.contains(peer)) {
      ++load;
    }
  }
  return load;
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

void BroadcastsPlumtree::Impl::promote_eager(PlumtreeSlot &slot, adnl::AdnlNodeIdShort peer, bool force) {
  expire_pending_feedback(slot);
  if (peer.is_zero()) {
    return;
  }
  slot.pending_feedback.erase(peer);
  if (slot.eager.contains(peer)) {
    return;
  }
  auto capacity = options_.eager_limit_;
  auto load = slot_load(slot);
  if (!force && load >= capacity) {
    return;
  }
  if (force && load >= capacity && !slot.eager.empty()) {
    auto it = slot.eager.begin();
    std::advance(it, td::Random::fast(0, static_cast<td::int32>(slot.eager.size()) - 1));
    slot.eager.erase(it);
  } else if (force && load >= capacity && !slot.pending_feedback.empty()) {
    auto it = slot.pending_feedback.begin();
    std::advance(it, td::Random::fast(0, static_cast<td::int32>(slot.pending_feedback.size()) - 1));
    slot.pending_feedback.erase(it);
  }
  slot.eager.insert(peer);
}

void BroadcastsPlumtree::Impl::trim_eager_to_capacity(PlumtreeSlot &slot) {
  expire_pending_feedback(slot);
  auto capacity = options_.eager_limit_;
  while (slot.eager.size() > capacity) {
    auto it = slot.eager.begin();
    std::advance(it, td::Random::fast(0, static_cast<td::int32>(slot.eager.size()) - 1));
    slot.pending_feedback.erase(*it);
    slot.eager.erase(it);
  }
}

void BroadcastsPlumtree::Impl::remove_eager(PlumtreeSlot &slot, adnl::AdnlNodeIdShort peer) {
  slot.eager.erase(peer);
  slot.pending_feedback.erase(peer);
}

PlumtreeBroadcastState *BroadcastsPlumtree::Impl::get_state(const td::Bits256 &broadcast_id) {
  auto it = broadcasts_.find(broadcast_id);
  return it == broadcasts_.end() ? nullptr : it->second.get();
}

PlumtreePartState *BroadcastsPlumtree::Impl::get_part(const td::Bits256 &broadcast_id, td::uint32 part_index,
                                                          td::uint32 tree_index) {
  auto *state = get_state(broadcast_id);
  if (!state) {
    return nullptr;
  }
  auto it = state->tree_parts.find(TreePartKey{part_index, tree_index});
  return it == state->tree_parts.end() ? nullptr : &it->second;
}

td::Status BroadcastsPlumtree::Impl::ensure_decoder(PlumtreeBroadcastState &broadcast) {
  if (broadcast.delivered || broadcast.decoder) {
    return td::Status::OK();
  }
  if (broadcast.part_size == 0 || broadcast.data_size == 0) {
    return td::Status::Error(ErrorCode::protoviolation, "invalid Plumtree decoder parameters");
  }
  TRY_RESULT(decoder,
             td::raptorq::Decoder::create({(broadcast.data_size + broadcast.part_size - 1) / broadcast.part_size,
                                           broadcast.part_size, broadcast.data_size}));
  broadcast.decoder = std::move(decoder);
  return td::Status::OK();
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

bool BroadcastsPlumtree::Impl::send_payload_to(OverlayImpl *overlay, PlumtreeBroadcastState &broadcast,
                                               PlumtreePartState &part, const adnl::AdnlNodeIdShort &dst) {
  if (sender_.empty() || dst.is_zero() || dst == overlay->local_id()) {
    return false;
  }
  if (part.full_sends >= options_.eager_limit_ || part.full_sent_to.contains(dst)) {
    return false;
  }
  auto part_data = broadcast.parts_by_index.find(part.part_index);
  if (part_data == broadcast.parts_by_index.end()) {
    return false;
  }
  auto wire = create_serialize_tl_object<ton_api::overlay_broadcastPlumtreePayload>(
      static_cast<std::int32_t>(broadcast.flags), part.timestamp, part.source_key.tl(),
      part.certificate ? part.certificate->tl() : Certificate::empty_tl(), broadcast.data_hash,
      static_cast<std::int32_t>(broadcast.data_size), static_cast<std::int32_t>(part.part_index),
      static_cast<std::int32_t>(part.tree_index), part_data->second.clone(), part.signature.clone());
  auto wire_size = wire.size();
  td::actor::send_closure(overlay->overlay_manager(), &Overlays::send_message_via, dst, overlay->local_id(),
                          overlay->overlay_id(), std::move(wire), sender_);
  if (auto *s = slot(part.tree_index)) {
    expire_pending_feedback(*s);
    s->pending_feedback[dst] = td::Timestamp::now();
  }
  part.full_sent_to.insert(dst);
  ++part.full_sends;
  overlay->get_broadcasts_limiter(part.source, part.certificate.get()).register_out_traffic(wire_size);
  return true;
}

void BroadcastsPlumtree::Impl::send_ihave_to(OverlayImpl *overlay, PlumtreePartState &part,
                                             const td::Bits256 &broadcast_id, const adnl::AdnlNodeIdShort &dst) {
  if (dst.is_zero() || dst == overlay->local_id() || part.full_sent_to.contains(dst) ||
      part.advertised_to.contains(dst)) {
    return;
  }
  if (part.full_sends >= options_.eager_limit_) {
    return;
  }
  if (send_control(overlay, dst,
                   create_tl_object<ton_api::overlay_broadcastPlumtreeIHave>(
                       broadcast_id, td::Clocks::system(), static_cast<std::int32_t>(part.part_index),
                       static_cast<std::int32_t>(part.tree_index)))) {
    part.advertised_to.insert(dst);
  }
}

void BroadcastsPlumtree::Impl::send_prune(OverlayImpl *overlay, const adnl::AdnlNodeIdShort &dst,
                                          const PlumtreePartState &part, const td::Bits256 &broadcast_id) {
  send_control(overlay, dst,
               create_tl_object<ton_api::overlay_broadcastPlumtreePrune>(broadcast_id, td::Clocks::system(),
                                                                         static_cast<std::int32_t>(part.part_index),
                                                                         static_cast<std::int32_t>(part.tree_index)));
}

void BroadcastsPlumtree::Impl::send_useful(OverlayImpl *overlay, const adnl::AdnlNodeIdShort &dst,
                                           const PlumtreePartState &part, const td::Bits256 &broadcast_id) {
  send_control(overlay, dst,
               create_tl_object<ton_api::overlay_broadcastPlumtreeUseful>(broadcast_id, td::Clocks::system(),
                                                                          static_cast<std::int32_t>(part.part_index),
                                                                          static_cast<std::int32_t>(part.tree_index)));
}

void BroadcastsPlumtree::Impl::forward_payload(OverlayImpl *overlay, PlumtreeBroadcastState &broadcast,
                                               PlumtreePartState &part, adnl::AdnlNodeIdShort from) {
  auto *s = slot(part.tree_index);
  if (!s) {
    return;
  }

  auto active = overlay->get_neighbours(options_.active_neighbours_);
  active.erase(std::remove_if(active.begin(), active.end(),
                              [&](const auto &peer) { return peer == overlay->local_id() || peer == from; }),
               active.end());
  std::set<adnl::AdnlNodeIdShort> full_sent;
  if (s->initialized) {
    trim_eager_to_capacity(*s);
    for (const auto &peer : s->eager) {
      if (peer != from && send_payload_to(overlay, broadcast, part, peer)) {
        full_sent.insert(peer);
      }
    }
  } else {
    for (const auto &peer : active) {
      if (full_sent.size() >= options_.eager_limit_) {
        break;
      }
      if (send_payload_to(overlay, broadcast, part, peer)) {
        full_sent.insert(peer);
      }
    }
    s->initialized = true;
  }

  for (const auto &peer : active) {
    if (peer != from && !full_sent.contains(peer) && !s->eager.contains(peer)) {
      send_ihave_to(overlay, part, broadcast.broadcast_id, peer);
    }
  }
}

void BroadcastsPlumtree::Impl::trim_missing_parts() {
  if (missing_parts_.size() <= MAX_SPECULATIVE_REPAIR_PARTS) {
    return;
  }
  while (missing_parts_.size() > MAX_SPECULATIVE_REPAIR_PARTS) {
    auto oldest = missing_parts_.begin();
    for (auto it = missing_parts_.begin(); it != missing_parts_.end(); ++it) {
      if (it->second.created_at.at() < oldest->second.created_at.at()) {
        oldest = it;
      }
    }
    missing_parts_.erase(oldest);
  }
}

void BroadcastsPlumtree::Impl::send(OverlayImpl *overlay, PublicKeyHash send_as, td::uint32 flags,
                                    td::BufferSlice data, td::uint32 local_validator_index,
                                    td::uint32 validator_count) {
  if (data.empty() || data.size() > Overlays::max_fec_broadcast_size()) {
    VLOG(PLUMTREE_WARNING) << overlay << ": invalid Plumtree payload size " << data.size();
    return;
  }
  if (send_as.is_zero()) {
    VLOG(PLUMTREE_INFO) << overlay << ": no local Plumtree source";
    return;
  }
  if (validator_count == 0 || local_validator_index >= validator_count) {
    VLOG(PLUMTREE_WARNING) << overlay << ": invalid Plumtree validator group index local_index="
                           << local_validator_index << " validator_count=" << validator_count;
    return;
  }

  auto data_size = static_cast<td::uint32>(data.size());
  auto part_size = static_cast<td::uint32>((data.size() + options_.k_ - 1) / options_.k_);
  if (part_size == 0) {
    VLOG(PLUMTREE_WARNING) << overlay << ": invalid Plumtree part size";
    return;
  }
  auto data_hash = td::sha256_bits256(data.as_slice());
  auto broadcast_id = compute_broadcast_id(flags, data_hash, data_size, part_size);
  auto cert = overlay->get_certificate(send_as);
  if (overlay->check_source_eligible(send_as, cert.get(), data_size, true) == BroadcastCheckResult::Forbidden) {
    VLOG(PLUMTREE_WARNING) << overlay << ": Plumtree source is no longer eligible";
    return;
  }

  auto assigned_parts_r =
      schedule(overlay)->assigned_parts(broadcast_id, send_as, local_validator_index, validator_count);
  if (assigned_parts_r.is_error()) {
    VLOG(PLUMTREE_WARNING) << overlay << ": cannot build Plumtree schedule: " << assigned_parts_r.move_as_error();
    return;
  }
  auto assigned_parts = assigned_parts_r.move_as_ok();
  if (assigned_parts.empty()) {
    VLOG(PLUMTREE_INFO) << overlay << ": no Plumtree parts assigned to local source for broadcast_id="
                        << broadcast_id.to_hex();
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
    td::BufferSlice part(part_size);
    auto status = encoder->gen_symbol(part_index, part.as_slice());
    if (status.is_error()) {
      VLOG(PLUMTREE_WARNING) << overlay << ": cannot generate Plumtree symbol: " << status;
      continue;
    }

    PlumtreeOutboundPayload payload;
    payload.broadcast_id = broadcast_id;
    payload.flags = flags;
    payload.timestamp = td::Clocks::system();
    payload.source = send_as;
    payload.data_hash = data_hash;
    payload.data_size = data_size;
    payload.part_index = part_index;
    payload.tree_index = tree_index;
    payload.part = std::move(part);

    auto to_sign = make_payload_to_sign(payload.broadcast_id, payload.timestamp, payload.part_index, payload.tree_index,
                                        payload.part);
    auto promise = td::PromiseCreator::lambda([overlay_id = actor_id(overlay), payload = std::move(payload)](
                                                  td::Result<std::pair<td::BufferSlice, PublicKey>> R) mutable {
      td::actor::send_closure(overlay_id, &OverlayImpl::broadcast_plumtree_signed_payload, std::move(payload),
                              std::move(R));
    });
    td::actor::send_closure(overlay->keyring(), &keyring::Keyring::sign_add_get_public_key, send_as, std::move(to_sign),
                            std::move(promise));
    ++signed_parts;
  }

  VLOG(PLUMTREE_INFO) << overlay << ": Plumtree START sender broadcast_id=" << broadcast_id.to_hex()
                      << " data_hash=" << data_hash.to_hex() << " data_size=" << data_size
                      << " scheduled_parts=" << options_.parts_ << " signed_parts=" << signed_parts;
}

void BroadcastsPlumtree::Impl::signed_payload(OverlayImpl *overlay, PlumtreeOutboundPayload &&payload,
                                              td::Result<std::pair<td::BufferSlice, PublicKey>> &&R) {
  if (R.is_error()) {
    auto status = R.move_as_error();
    LOG_IF(WARNING, status.code() != ErrorCode::notready) << "failed to sign Plumtree payload: " << status;
    return;
  }
  auto signed_part = R.move_as_ok();
  if (signed_part.second.compute_short_id() != payload.source) {
    VLOG(PLUMTREE_WARNING) << overlay << ": keyring signed Plumtree payload with unexpected source";
    return;
  }
  if (payload.part_index >= options_.parts_ || payload.tree_index >= options_.tree_slots_) {
    VLOG(PLUMTREE_WARNING) << overlay << ": Plumtree signed payload is outside current option bounds";
    return;
  }
  auto expected_part_size =
      static_cast<td::uint32>((static_cast<td::uint64>(payload.data_size) + options_.k_ - 1) / options_.k_);
  if (payload.data_size == 0 || payload.data_size > Overlays::max_fec_broadcast_size() || payload.part.empty() ||
      payload.part.size() != expected_part_size) {
    VLOG(PLUMTREE_WARNING) << overlay << ": Plumtree signed payload has invalid FEC fields";
    return;
  }

  auto cert = overlay->get_certificate(payload.source);
  if (overlay->check_source_eligible(payload.source, cert.get(), payload.data_size, true) ==
      BroadcastCheckResult::Forbidden) {
    VLOG(PLUMTREE_WARNING) << overlay << ": Plumtree source became ineligible before signed payload send";
    return;
  }
  auto source_key = signed_part.second;

  auto it = broadcasts_.find(payload.broadcast_id);
  if (it == broadcasts_.end()) {
    auto state = std::make_unique<PlumtreeBroadcastState>();
    state->broadcast_id = payload.broadcast_id;
    state->first_valid_source = payload.source;
    state->data_hash = payload.data_hash;
    state->first_seen_at = td::Timestamp::now();
    state->flags = payload.flags;
    state->data_size = payload.data_size;
    state->part_size = static_cast<td::uint32>(payload.part.size());
    lru_.put(state.get());
    it = broadcasts_.emplace(payload.broadcast_id, std::move(state)).first;
  } else {
    auto &state = *it->second;
    if (state.flags != payload.flags || state.data_hash != payload.data_hash || state.data_size != payload.data_size ||
        state.part_size != payload.part.size()) {
      VLOG(PLUMTREE_WARNING) << overlay << ": Plumtree signed payload conflicts with existing broadcast state";
      return;
    }
  }

  auto &broadcast = *it->second;
  if (get_part(payload.broadcast_id, payload.part_index, payload.tree_index)) {
    return;
  }

  if (broadcast.parts_by_index.find(payload.part_index) == broadcast.parts_by_index.end()) {
    broadcast.parts_by_index.emplace(payload.part_index, std::move(payload.part));
  }

  PlumtreePartState part;
  part.part_index = payload.part_index;
  part.tree_index = payload.tree_index;
  part.timestamp = payload.timestamp;
  part.source_key = std::move(source_key);
  part.source = payload.source;
  part.certificate = cert;
  part.signature = std::move(signed_part.first);
  auto [part_it, _] =
      broadcast.tree_parts.emplace(TreePartKey{payload.part_index, payload.tree_index}, std::move(part));

  forward_payload(overlay, broadcast, part_it->second, adnl::AdnlNodeIdShort::zero());

  if (!broadcast.overlay_limiter_registered) {
    overlay->get_broadcasts_limiter(payload.source, cert.get()).register_broadcast(payload.data_size);
    broadcast.overlay_limiter_registered = true;
  }
  VLOG(PLUMTREE_INFO) << overlay << ": Plumtree SEND_PAYLOAD sender broadcast_id=" << payload.broadcast_id.to_hex()
                      << " part_index=" << payload.part_index << " tree_index=" << payload.tree_index
                      << " full_sent=" << part_it->second.full_sends;
}

td::actor::Task<> BroadcastsPlumtree::Impl::process_payload(
    OverlayImpl *overlay, adnl::AdnlNodeIdShort from, tl_object_ptr<ton_api::overlay_broadcastPlumtreePayload> msg) {
  if (from.is_zero()) {
    co_return td::Status::Error(ErrorCode::protoviolation, "missing Plumtree immediate sender");
  }
  auto flags = static_cast<td::uint32>(msg->flags_);
  auto timestamp = msg->timestamp_;
  auto data_size = static_cast<td::uint32>(msg->data_size_);
  auto part_index = static_cast<td::uint32>(msg->part_index_);
  auto tree_index = static_cast<td::uint32>(msg->tree_index_);
  CO_TRY(check_timestamp(timestamp));
  if (data_size == 0 || data_size > Overlays::max_fec_broadcast_size() || msg->part_.empty()) {
    co_return td::Status::Error(ErrorCode::protoviolation, "invalid Plumtree FEC fields");
  }
  auto part_size = static_cast<td::uint32>(msg->part_.size());
  auto expected_part_size =
      static_cast<td::uint32>((static_cast<td::uint64>(data_size) + options_.k_ - 1) / options_.k_);
  if (part_size != expected_part_size) {
    co_return td::Status::Error(ErrorCode::protoviolation, "invalid Plumtree part size");
  }
  if (part_index >= options_.parts_ || tree_index >= options_.tree_slots_) {
    co_return td::Status::Error(ErrorCode::protoviolation, "invalid Plumtree part or tree index");
  }

  auto broadcast_id = compute_broadcast_id(flags, msg->data_hash_, data_size, part_size);
  PublicKey source_key(msg->src_);
  auto source_hash = source_key.compute_short_id();
  auto it = broadcasts_.find(broadcast_id);
  if (it == broadcasts_.end() && overlay->is_delivered(broadcast_id)) {
    co_return td::Status::Error(ErrorCode::notready, "known Plumtree broadcast");
  }
  bool new_broadcast = it == broadcasts_.end();
  if (!new_broadcast) {
    auto &state = *it->second;
    if (state.flags != flags || state.data_hash != msg->data_hash_ || state.data_size != data_size ||
        state.part_size != part_size) {
      co_return td::Status::Error(ErrorCode::protoviolation, "Plumtree broadcast id collision");
    }
  }

  auto cert = CO_TRY(Certificate::create(msg->certificate_));
  auto check = overlay->check_source_eligible(source_hash, cert.get(), data_size, true, from);
  if (check != BroadcastCheckResult::Allowed) {
    co_return td::Status::Error(ErrorCode::protoviolation, "Plumtree source is not allowed");
  }

  if (new_broadcast) {
    CO_TRY(overlay->get_broadcasts_limiter(source_hash, cert.get()).precheck_new_broadcast(data_size));
    co_await overlay->precheck_broadcast(source_hash, broadcast_id, {}, false).trace("precheck Plumtree broadcast");
  }

  auto to_sign = make_payload_to_sign(broadcast_id, timestamp, part_index, tree_index, msg->part_);
  {
    TD_PERF_COUNTER(check_signature_overlay_broadcast_plumtree_payload);
    CO_TRY(overlay->check_signature_from_peer(source_key, to_sign, msg->signature_, from));
  }

  if (new_broadcast) {
    co_await overlay->precheck_broadcast(source_hash, broadcast_id, {}, true).trace("precheck Plumtree broadcast");
    CO_TRY(check_timestamp(timestamp));
    it = broadcasts_.find(broadcast_id);
    if (it == broadcasts_.end()) {
      CO_TRY(overlay->get_broadcasts_limiter(source_hash, cert.get()).try_register_broadcast(data_size));
      auto state = std::make_unique<PlumtreeBroadcastState>();
      state->broadcast_id = broadcast_id;
      state->first_valid_source = source_hash;
      state->data_hash = msg->data_hash_;
      state->first_seen_at = td::Timestamp::now();
      state->flags = flags;
      state->data_size = data_size;
      state->part_size = part_size;
      CO_TRY(ensure_decoder(*state));
      lru_.put(state.get());
      it = broadcasts_.emplace(broadcast_id, std::move(state)).first;
      VLOG(PLUMTREE_INFO) << overlay << ": Plumtree START receiver broadcast_id=" << broadcast_id.to_hex()
                          << " data_hash=" << msg->data_hash_.to_hex() << " data_size=" << data_size
                          << " from=" << from;
    } else {
      auto &state = *it->second;
      if (state.flags != flags || state.data_hash != msg->data_hash_ || state.data_size != data_size ||
          state.part_size != part_size) {
        co_return td::Status::Error(ErrorCode::protoviolation, "Plumtree broadcast id collision");
      }
    }
  }

  auto &broadcast = *it->second;
  if (auto *known = get_part(broadcast_id, part_index, tree_index)) {
    if (auto *s = slot(tree_index)) {
      remove_eager(*s, from);
    }
    send_prune(overlay, from, *known, broadcast_id);
    co_return td::Status::Error(ErrorCode::notready, "duplicate Plumtree part");
  }

  CO_TRY(ensure_decoder(broadcast));
  auto part_data_it = broadcast.parts_by_index.find(part_index);
  if (part_data_it == broadcast.parts_by_index.end()) {
    part_data_it = broadcast.parts_by_index.emplace(part_index, msg->part_.clone()).first;
  }
  PlumtreePartState part;
  part.part_index = part_index;
  part.tree_index = tree_index;
  part.timestamp = timestamp;
  part.source_key = std::move(source_key);
  part.source = source_hash;
  part.certificate = cert;
  part.signature = std::move(msg->signature_);
  auto [part_it, _] =
      broadcast.tree_parts.emplace(TreePartKey{part_index, tree_index}, std::move(part));
  missing_parts_.erase(MissingPartKey{broadcast_id, part_index, tree_index});

  auto *s = slot(tree_index);
  if (!s) {
    co_return td::Status::Error(ErrorCode::protoviolation, "invalid Plumtree slot");
  }
  promote_eager(*s, from, true);
  send_useful(overlay, from, part_it->second, broadcast_id);
  forward_payload(overlay, broadcast, part_it->second, from);

  if (!broadcast.delivered && broadcast.decoder && !broadcast.decoder_parts.contains(part_index)) {
    CO_TRY(broadcast.decoder->add_symbol({part_index, part_data_it->second.clone()}));
    broadcast.decoder_parts.insert(part_index);
    if (broadcast.decoder->may_try_decode()) {
      auto decoded = CO_TRY(broadcast.decoder->try_decode(false));
      if (msg->data_hash_ != td::sha256_bits256(decoded.data.as_slice())) {
        co_return td::Status::Error(ErrorCode::protoviolation, "Plumtree decoded data hash mismatch");
      }
      broadcast.delivered = true;
      broadcast.decoder = {};
      overlay->register_delivered_broadcast(broadcast_id);
      VLOG(PLUMTREE_INFO) << overlay << ": Plumtree FINISH receiver broadcast_id=" << broadcast_id.to_hex()
                          << " data_size=" << data_size << " decoded=true";
      overlay->deliver_broadcast(broadcast.first_valid_source, std::move(decoded.data), {});
    }
  }
  co_return td::Unit{};
}

td::actor::Task<> BroadcastsPlumtree::Impl::process_ihave(OverlayImpl *overlay, adnl::AdnlNodeIdShort from,
                                                          tl_object_ptr<ton_api::overlay_broadcastPlumtreeIHave> msg) {
  if (from.is_zero()) {
    co_return td::Status::Error(ErrorCode::protoviolation, "missing Plumtree immediate sender");
  }
  auto control = get_control_fields(*msg);
  CO_TRY(check_timestamp(control.timestamp));
  CO_TRY(validate_control_fields(control.broadcast_id, control.part_index, control.tree_index));
  auto part_index = control.part_index;
  auto tree_index = control.tree_index;

  if (!get_state(control.broadcast_id) && overlay->is_delivered(control.broadcast_id)) {
    co_return td::Unit{};
  }
  if (get_part(control.broadcast_id, part_index, tree_index)) {
    co_return td::Unit{};
  }
  auto &missing = missing_parts_[MissingPartKey{control.broadcast_id, part_index, tree_index}];
  if (missing.repair_sent) {
    co_return td::Unit{};
  }
  if (std::find(missing.repair_targets.begin(), missing.repair_targets.end(), from) == missing.repair_targets.end() &&
      missing.repair_targets.size() < options_.max_repair_targets_) {
    missing.repair_targets.push_back(from);
  }
  if (!missing.repair_at) {
    missing.repair_at = td::Timestamp::in(options_.repair_timeout_ms_ / 1000.0);
    overlay->relax_plumtree_alarm(missing.repair_at);
  }
  trim_missing_parts();
  co_return td::Unit{};
}

td::actor::Task<> BroadcastsPlumtree::Impl::process_repair(
    OverlayImpl *overlay, adnl::AdnlNodeIdShort from, tl_object_ptr<ton_api::overlay_broadcastPlumtreeRepair> msg) {
  if (from.is_zero()) {
    co_return td::Status::Error(ErrorCode::protoviolation, "missing Plumtree immediate sender");
  }
  auto control = get_control_fields(*msg);
  CO_TRY(check_timestamp(control.timestamp));
  CO_TRY(validate_control_fields(control.broadcast_id, control.part_index, control.tree_index));
  auto part_index = control.part_index;
  auto tree_index = control.tree_index;

  auto *state = get_state(control.broadcast_id);
  if (!state && overlay->is_delivered(control.broadcast_id)) {
    co_return td::Unit{};
  }
  auto *part = get_part(control.broadcast_id, part_index, tree_index);
  auto *s = slot(tree_index);
  if (!state || !part || !s || !part->advertised_to.contains(from) || part->full_sent_to.contains(from)) {
    co_return td::Unit{};
  }
  expire_pending_feedback(*s);
  if (slot_load(*s) >= options_.eager_limit_ || part->full_sends >= options_.eager_limit_) {
    co_return td::Status::Error(ErrorCode::notready, "Plumtree REPAIR cannot be served");
  }
  send_payload_to(overlay, *state, *part, from);
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

  if (!get_state(control.broadcast_id) && overlay->is_delivered(control.broadcast_id)) {
    co_return td::Unit{};
  }
  auto *part = get_part(control.broadcast_id, part_index, tree_index);
  auto *s = slot(tree_index);
  if (!part || !s) {
    co_return td::Unit{};
  }
  bool had_record = s->pending_feedback.contains(from) || s->eager.contains(from) || part->full_sent_to.contains(from);
  if (!had_record) {
    co_return td::Unit{};
  }
  remove_eager(*s, from);
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

  if (!get_state(control.broadcast_id) && overlay->is_delivered(control.broadcast_id)) {
    co_return td::Unit{};
  }
  auto *part = get_part(control.broadcast_id, part_index, tree_index);
  auto *s = slot(tree_index);
  if (!part || !s || (!s->pending_feedback.contains(from) && !s->eager.contains(from))) {
    co_return td::Unit{};
  }
  bool was_pending = s->pending_feedback.erase(from) > 0;
  if (was_pending) {
    promote_eager(*s, from, false);
  }
  co_return td::Unit{};
}

void BroadcastsPlumtree::Impl::alarm(OverlayImpl *overlay) {
  expire_pending_feedback();
  auto now = td::Timestamp::now();
  auto missing_expired_before =
      now - PLUMTREE_MISSING_PART_TTL_REPAIR_TIMEOUTS * options_.repair_timeout_ms_ / 1000.0;
  std::vector<MissingPartKey> erase;
  for (auto &[key, missing] : missing_parts_) {
    const auto &[broadcast_id, part_index, tree_index] = key;
    if (get_part(broadcast_id, part_index, tree_index)) {
      erase.push_back(key);
      continue;
    }
    if (missing.created_at <= missing_expired_before) {
      erase.push_back(key);
      continue;
    }
    if (!missing.repair_sent && missing.repair_at && missing.repair_at.is_in_past(now)) {
      for (const auto &dst : missing.repair_targets) {
        send_control(overlay, dst,
                     create_tl_object<ton_api::overlay_broadcastPlumtreeRepair>(broadcast_id, td::Clocks::system(),
                                                                                static_cast<std::int32_t>(part_index),
                                                                                static_cast<std::int32_t>(tree_index)));
      }
      missing.repair_targets.clear();
      missing.repair_sent = true;
    }
  }
  for (const auto &key : erase) {
    missing_parts_.erase(key);
  }
}

td::Timestamp BroadcastsPlumtree::Impl::next_alarm_at() const {
  auto result = td::Timestamp::never();
  for (const auto &[_, missing] : missing_parts_) {
    if (!missing.repair_sent && missing.repair_at) {
      result.relax(missing.repair_at);
    }
  }
  return result;
}

void BroadcastsPlumtree::Impl::gc(OverlayImpl *overlay) {
  expire_pending_feedback();
  auto now = td::Timestamp::now();
  auto broadcast_expired_before = now - PLUMTREE_BROADCAST_TTL;
  while (!broadcasts_.empty()) {
    auto *bcast = static_cast<PlumtreeBroadcastState *>(lru_.prev);
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

  std::vector<MissingPartKey> erase;
  auto missing_expired_before =
      now - PLUMTREE_MISSING_PART_TTL_REPAIR_TIMEOUTS * options_.repair_timeout_ms_ / 1000.0;
  for (const auto &[key, missing] : missing_parts_) {
    if (missing.created_at <= missing_expired_before) {
      erase.push_back(key);
    }
  }
  for (const auto &key : erase) {
    missing_parts_.erase(key);
  }
}

BroadcastsPlumtree::BroadcastsPlumtree(PlumtreeFecOptions options) : impl_(std::make_unique<Impl>(options)) {
}

BroadcastsPlumtree::~BroadcastsPlumtree() = default;

void BroadcastsPlumtree::init_sender(td::actor::ActorId<adnl::AdnlSenderInterface> sender) {
  impl_->init_sender(sender);
}

void BroadcastsPlumtree::send(OverlayImpl *overlay, PublicKeyHash send_as, td::uint32 flags, td::BufferSlice data,
                              td::uint32 local_validator_index, td::uint32 validator_count) {
  impl_->send(overlay, send_as, flags, std::move(data), local_validator_index, validator_count);
}

void BroadcastsPlumtree::signed_payload(OverlayImpl *overlay, PlumtreeOutboundPayload &&payload,
                                        td::Result<std::pair<td::BufferSlice, PublicKey>> &&R) {
  impl_->signed_payload(overlay, std::move(payload), std::move(R));
}

td::actor::Task<> BroadcastsPlumtree::process_payload(OverlayImpl *overlay, adnl::AdnlNodeIdShort from,
                                                      tl_object_ptr<ton_api::overlay_broadcastPlumtreePayload> msg) {
  co_await impl_->process_payload(overlay, from, std::move(msg));
  co_return td::Unit{};
}

td::actor::Task<> BroadcastsPlumtree::process_ihave(OverlayImpl *overlay, adnl::AdnlNodeIdShort from,
                                                    tl_object_ptr<ton_api::overlay_broadcastPlumtreeIHave> msg) {
  co_await impl_->process_ihave(overlay, from, std::move(msg));
  co_return td::Unit{};
}

td::actor::Task<> BroadcastsPlumtree::process_repair(OverlayImpl *overlay, adnl::AdnlNodeIdShort from,
                                                     tl_object_ptr<ton_api::overlay_broadcastPlumtreeRepair> msg) {
  co_await impl_->process_repair(overlay, from, std::move(msg));
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

td::Timestamp BroadcastsPlumtree::next_alarm_at() const {
  return impl_->next_alarm_at();
}

void BroadcastsPlumtree::gc(OverlayImpl *overlay) {
  impl_->gc(overlay);
}

}  // namespace overlay

}  // namespace ton
