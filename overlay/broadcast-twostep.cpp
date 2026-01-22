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

#include <cstddef>
#include <map>
#include <memory>
#include <set>
#include <utility>
#include <vector>

#include "adnl/adnl-node-id.hpp"
#include "auto/tl/ton_api.h"
#include "crypto/common/bitstring.h"
#include "keys/keys.hpp"
#include "td/actor/actor.h"
#include "td/fec/raptorq/Decoder.h"
#include "td/fec/raptorq/Encoder.h"
#include "td/utils/List.h"
#include "td/utils/Status.h"
#include "td/utils/buffer.h"
#include "td/utils/common.h"
#include "td/utils/int_types.h"
#include "td/utils/port/Clocks.h"

#include "broadcast-twostep.hpp"
#include "overlay.hpp"

namespace ton {

namespace overlay {

constexpr int VERBOSITY_NAME(TWOSTEP_WARNING) = verbosity_WARNING;
constexpr int VERBOSITY_NAME(TWOSTEP_INFO) = verbosity_INFO;
constexpr int VERBOSITY_NAME(TWOSTEP_DEBUG) = verbosity_DEBUG;

static constexpr std::size_t FEC_MIN_BYTES = 513;
static constexpr std::size_t FEC_MIN_OTHER_NODES = 4;

static constexpr std::size_t fec_k(std::size_t other_nodes) {
  LOG_CHECK(other_nodes > 2) << "other_nodes=" << other_nodes;
  return (other_nodes * 2 - 2) / 3;
}

struct BroadcastTwostepDebugInfo {
  adnl::AdnlNodeIdShort src_adnl_id;
  td::Bits256 data_hash;
  td::uint32 data_size{0};
  td::uint32 symbols_received{0};
  td::uint32 symbols_needed{0};
  td::Timestamp timestamp;
  std::set<adnl::AdnlNodeIdShort> chunk_senders;

  void print_senders(td::StringBuilder &sb) const {
    sb << "senders=" << chunk_senders;
  }

  double elapsed() const {
    return td::Timestamp::now().at() - timestamp.at();
  }
};

td::StringBuilder &operator<<(td::StringBuilder &sb, const BroadcastTwostepDebugInfo &d) {
  sb << "src=" << d.src_adnl_id << " data_hash=" << d.data_hash.to_hex() << " data_size=" << d.data_size;
  if (d.symbols_needed > 0) {
    sb << " symbols=" << d.symbols_received << "/" << d.symbols_needed;
  }
  if (!d.chunk_senders.empty()) {
    sb << " unique_senders=" << d.chunk_senders.size();
  }
  return sb;
}

struct BroadcastTwostep : td::ListNode {
  Overlay::BroadcastHash broadcast_id;
  td::uint32 date;
  std::unique_ptr<td::raptorq::Decoder> decoder;
  BroadcastTwostepDebugInfo debug;
};

td::StringBuilder &operator<<(td::StringBuilder &sb, const BroadcastTwostep &b) {
  return sb << "broadcast_id=" << b.broadcast_id.to_hex() << " " << b.debug;
}

struct BroadcastTwostepDataSimple {
  td::Bits256 broadcast_id;
  td::uint32 flags;
  td::uint32 date;
  adnl::AdnlNodeIdShort src;
  std::vector<adnl::AdnlNodeIdShort> dsts;
  td::BufferSlice data;
};

struct BroadcastTwostepDataFec {
  td::Bits256 broadcast_id;
  td::uint32 flags;
  td::uint32 date;
  adnl::AdnlNodeIdShort src;
  adnl::AdnlNodeIdShort dst;
  td::Bits256 data_hash;
  td::uint32 data_size;
  td::uint32 seqno;
  td::BufferSlice part;
};

BroadcastsTwostep::BroadcastsTwostep() = default;

BroadcastsTwostep::~BroadcastsTwostep() = default;

void BroadcastsTwostep::send(OverlayImpl *overlay, PublicKeyHash send_as, td::BufferSlice data, td::uint32 flags) {
  std::size_t data_size = data.size();
  td::Bits256 data_hash = sha256_bits256(data.as_slice());
  td::uint32 date = static_cast<td::uint32>(td::Clocks::system());
  std::vector<adnl::AdnlNodeIdShort> other_nodes;
  overlay->iterate_all_peers([&](const adnl::AdnlNodeIdShort &peer_id, OverlayPeer &peer) {
    if (overlay->is_persistent_node(peer_id) && peer_id != overlay->local_id()) {
      other_nodes.push_back(peer_id);
    }
  });
  td::Bits256 broadcast_id;
  bool use_fec = data_size >= FEC_MIN_BYTES && other_nodes.size() >= FEC_MIN_OTHER_NODES;
  if (use_fec) {
    std::size_t k = fec_k(other_nodes.size());
    std::size_t part_size = (data_size + k - 1) / k;
    CHECK(part_size < data_size);
    broadcast_id = get_tl_object_sha_bits256(create_tl_object<ton_api::overlay_broadcastTwostep_id>(
        flags, date, send_as.bits256_value(), overlay->local_id().bits256_value(), data_hash,
        static_cast<std::int32_t>(part_size)));
    VLOG(TWOSTEP_INFO) << "twostep START sender broadcast_id=" << broadcast_id.to_hex()
                       << " data_hash=" << data_hash.to_hex() << " data_size=" << data_size
                       << " recipients=" << other_nodes.size() << " mode=FEC";
    auto R = td::raptorq::Encoder::create(part_size, data.clone());
    if (R.is_error()) {
      VLOG(TWOSTEP_WARNING) << "cannot create FEC encoder: " << R.move_as_error();
      return;
    }
    auto encoder = R.move_as_ok();
    encoder->precalc();
    for (std::size_t i = 0; i < other_nodes.size(); i++) {
      td::uint32 seqno = static_cast<std::uint32_t>(i);
      td::BufferSlice part(part_size);
      td::Status S = encoder->gen_symbol(seqno, part.as_slice());
      if (S.is_error()) {
        VLOG(TWOSTEP_WARNING) << "cannot generate symbol: " << S;
        continue;
      }
      td::BufferSlice to_sign = create_serialize_tl_object<ton_api::overlay_broadcastTwostepFec_toSign>(
          broadcast_id, static_cast<std::int32_t>(data_size), static_cast<std::int32_t>(seqno), part.clone());
      BroadcastTwostepDataFec passdata{.broadcast_id = broadcast_id,
                                       .flags = flags,
                                       .date = date,
                                       .src = adnl::AdnlNodeIdShort(send_as),
                                       .dst = other_nodes[i],
                                       .data_hash = data_hash,
                                       .data_size = static_cast<td::uint32>(data_size),
                                       .seqno = seqno,
                                       .part = std::move(part)};
      auto P = td::PromiseCreator::lambda([overlay = actor_id(overlay), data = std::move(passdata)](
                                              td::Result<std::pair<td::BufferSlice, PublicKey>> R) mutable {
        td::actor::send_closure(overlay, &OverlayImpl::broadcast_twostep_signed_fec, std::move(data), std::move(R));
      });
      td::actor::send_closure(overlay->keyring(), &keyring::Keyring::sign_add_get_public_key, send_as,
                              std::move(to_sign), std::move(P));
    }
  } else {
    broadcast_id = get_tl_object_sha_bits256(create_tl_object<ton_api::overlay_broadcastTwostep_id>(
        flags, date, send_as.bits256_value(), overlay->local_id().bits256_value(), data_hash,
        static_cast<std::int32_t>(data_size)));
    VLOG(TWOSTEP_INFO) << "twostep START sender broadcast_id=" << broadcast_id.to_hex()
                       << " data_hash=" << data_hash.to_hex() << " data_size=" << data_size
                       << " recipients=" << other_nodes.size() << " mode=simple";
    td::BufferSlice to_sign =
        create_serialize_tl_object<ton_api::overlay_broadcastTwostepSimple_toSign>(broadcast_id, data.clone());
    BroadcastTwostepDataSimple passdata{.broadcast_id = broadcast_id,
                                        .flags = flags,
                                        .date = date,
                                        .src = adnl::AdnlNodeIdShort(send_as),
                                        .dsts = std::move(other_nodes),
                                        .data = data.clone()};
    auto P = td::PromiseCreator::lambda([overlay = actor_id(overlay), data = std::move(passdata)](
                                            td::Result<std::pair<td::BufferSlice, PublicKey>> R) mutable {
      td::actor::send_closure(overlay, &OverlayImpl::broadcast_twostep_signed_simple, std::move(data), std::move(R));
    });
    td::actor::send_closure(overlay->keyring(), &keyring::Keyring::sign_add_get_public_key, send_as, std::move(to_sign),
                            std::move(P));
  }
  if (!overlay->is_delivered(broadcast_id)) {
    overlay->register_delivered_broadcast(broadcast_id);
    overlay->deliver_broadcast(send_as, std::move(data));
  }
}

static bool handle_error(const td::Result<std::pair<td::BufferSlice, PublicKey>> &R) {
  if (R.is_error()) {
    if (R.error().code() == ErrorCode::notready) {
      LOG(DEBUG) << "failed to send twostep broadcast: " << R.error();
    } else {
      LOG(WARNING) << "failed to send twostep broadcast: " << R.error();
    }
    return true;
  }
  return false;
}

void BroadcastsTwostep::signed_simple(OverlayImpl *overlay, BroadcastTwostepDataSimple &&data,
                                      td::Result<std::pair<td::BufferSlice, PublicKey>> &&R) {
  if (handle_error(R)) {
    return;
  }
  auto V = R.move_as_ok();
  VLOG(TWOSTEP_INFO) << "twostep SEND_SIMPLE sender broadcast_id=" << data.broadcast_id.to_hex()
                     << " data_size=" << data.data.size() << " recipients=" << data.dsts.size();
  auto cert = overlay->get_certificate(data.src.pubkey_hash());
  td::BufferSlice broadcast = create_serialize_tl_object<ton_api::overlay_broadcastTwostepSimple>(
      data.flags, data.date, V.second.tl(), overlay->local_id().bits256_value(),
      cert ? cert->tl() : Certificate::empty_tl(), std::move(data.data), std::move(V.first));
  for (auto &dst : data.dsts) {
    td::actor::send_closure(overlay->overlay_manager(), &Overlays::send_message_via, dst, overlay->local_id(),
                            overlay->overlay_id(), broadcast.clone(), sender_);
  }
}

void BroadcastsTwostep::signed_fec(OverlayImpl *overlay, BroadcastTwostepDataFec &&data,
                                   td::Result<std::pair<td::BufferSlice, PublicKey>> &&R) {
  if (handle_error(R)) {
    return;
  }
  auto V = R.move_as_ok();
  VLOG(TWOSTEP_INFO) << "twostep SEND_CHUNK sender broadcast_id=" << data.broadcast_id.to_hex()
                     << " data_hash=" << data.data_hash.to_hex() << " data_size=" << data.data_size
                     << " seqno=" << data.seqno << " part_size=" << data.part.size() << " to=" << data.dst;
  auto cert = overlay->get_certificate(data.src.pubkey_hash());
  td::BufferSlice broadcast = create_serialize_tl_object<ton_api::overlay_broadcastTwostepFec>(
      data.flags, data.date, V.second.tl(), overlay->local_id().bits256_value(),
      cert ? cert->tl() : Certificate::empty_tl(), data.data_hash, data.data_size, data.seqno, std::move(data.part),
      std::move(V.first));
  td::actor::send_closure(overlay->overlay_manager(), &Overlays::send_message_via, data.dst, overlay->local_id(),
                          overlay->overlay_id(), std::move(broadcast), sender_);
}

static td::Result<BroadcastCheckResult> check_signature_and_certificate(
    OverlayImpl *overlay, const PublicKey &src_key, const PublicKeyHash &src_keyhash, const td::BufferSlice &to_sign,
    const td::BufferSlice &signature, const tl_object_ptr<ton_api::overlay_Certificate> &certificate,
    td::uint32 data_size) {
  TRY_RESULT(encryptor, overlay->get_encryptor(src_key));
  TRY_STATUS(encryptor->check_signature(to_sign.as_slice(), signature.as_slice()));
  TRY_RESULT(cert, Certificate::create(certificate));
  auto r = overlay->check_source_eligible(src_keyhash, cert.get(), data_size, true);
  if (r == BroadcastCheckResult::Forbidden) {
    return td::Status::Error(ErrorCode::error, "broadcast is forbidden");
  }
  return r;
}

void BroadcastsTwostep::rebroadcast(OverlayImpl *overlay, const adnl::AdnlNodeIdShort &bcast_src_adnl_id,
                                    const td::BufferSlice &data) {
  overlay->iterate_all_peers([&](const adnl::AdnlNodeIdShort &peer_id, OverlayPeer &) {
    if (peer_id != bcast_src_adnl_id && peer_id != overlay->local_id()) {
      td::actor::send_closure(overlay->overlay_manager(), &Overlays::send_message_via, peer_id, overlay->local_id(),
                              overlay->overlay_id(), data.clone(), sender_);
    }
  });
}

static void check_and_deliver(OverlayImpl *overlay, PublicKeyHash src, BroadcastCheckResult check_result,
                              td::BufferSlice data) {
  if (check_result == BroadcastCheckResult::Allowed) {
    overlay->deliver_broadcast(src, std::move(data));
  } else {
    auto P = td::PromiseCreator::lambda(
        [overlay = actor_id(overlay), src, data = data.clone()](td::Result<td::Unit> R) mutable {
          td::actor::send_closure(overlay, &OverlayImpl::broadcast_twostep_checked, std::move(src), std::move(data),
                                  std::move(R));
        });
    overlay->check_broadcast(src, std::move(data), std::move(P));
  }
}

td::Status BroadcastsTwostep::process_broadcast(OverlayImpl *overlay, adnl::AdnlNodeIdShort src_peer_id,
                                                tl_object_ptr<ton_api::overlay_broadcastTwostepSimple> broadcast) {
  TRY_STATUS(overlay->check_date(broadcast->date_));
  PublicKey src_key(broadcast->src_);
  PublicKeyHash src_keyhash(src_key.compute_short_id());
  adnl::AdnlNodeIdShort bcast_src_adnl_id{broadcast->src_adnl_id_};
  td::Bits256 data_hash = sha256_bits256(broadcast->data_.as_slice());
  td::Bits256 broadcast_id = get_tl_object_sha_bits256(create_tl_object<ton_api::overlay_broadcastTwostep_id>(
      broadcast->flags_, broadcast->date_, src_keyhash.bits256_value(), bcast_src_adnl_id.bits256_value(), data_hash,
      static_cast<std::int32_t>(broadcast->data_.size())));
  bool will_rebroadcast = src_peer_id == bcast_src_adnl_id;
  VLOG(TWOSTEP_INFO) << "twostep RECV_SIMPLE receiver broadcast_id=" << broadcast_id.to_hex()
                     << " data_hash=" << data_hash.to_hex() << " data_size=" << broadcast->data_.size()
                     << " from=" << src_peer_id << " will_rebroadcast=" << will_rebroadcast;
  td::BufferSlice to_sign = create_serialize_tl_object<ton_api::overlay_broadcastTwostepSimple_toSign>(
      broadcast_id, broadcast->data_.clone());
  TRY_RESULT(check_result, check_signature_and_certificate(overlay, src_key, src_keyhash, to_sign,
                                                           broadcast->signature_, broadcast->certificate_,
                                                           static_cast<td::uint32>(broadcast->data_.size())));
  if (will_rebroadcast) {
    rebroadcast(overlay, bcast_src_adnl_id, serialize_tl_object(broadcast, true));
  }
  if (overlay->is_delivered(broadcast_id)) {
    VLOG(TWOSTEP_DEBUG) << "twostep DUPLICATE receiver broadcast_id=" << broadcast_id.to_hex();
    return td::Status::Error(ErrorCode::notready, "duplicate broadcast");
  }
  VLOG(TWOSTEP_INFO) << "twostep FINISH receiver broadcast_id=" << broadcast_id.to_hex()
                     << " data_hash=" << data_hash.to_hex() << " data_size=" << broadcast->data_.size()
                     << " decoded=true";
  overlay->register_delivered_broadcast(broadcast_id);
  check_and_deliver(overlay, src_keyhash, check_result, std::move(broadcast->data_));
  return td::Status::OK();
}

td::Status BroadcastsTwostep::process_broadcast(OverlayImpl *overlay, adnl::AdnlNodeIdShort src_peer_id,
                                                tl_object_ptr<ton_api::overlay_broadcastTwostepFec> broadcast) {
  td::uint32 date = static_cast<td::uint32>(broadcast->date_);
  TRY_STATUS(overlay->check_date(date));
  PublicKey src_key(broadcast->src_);
  PublicKeyHash src_keyhash(src_key.compute_short_id());
  adnl::AdnlNodeIdShort bcast_src_adnl_id{broadcast->src_adnl_id_};
  std::size_t data_size = static_cast<std::size_t>(static_cast<td::uint32>(broadcast->data_size_));
  std::size_t part_size = broadcast->part_.size();
  td::Bits256 broadcast_id = get_tl_object_sha_bits256(create_tl_object<ton_api::overlay_broadcastTwostep_id>(
      broadcast->flags_, broadcast->date_, src_keyhash.bits256_value(), bcast_src_adnl_id.bits256_value(),
      broadcast->data_hash_, static_cast<td::int32>(part_size)));
  td::BufferSlice to_sign = create_serialize_tl_object<ton_api::overlay_broadcastTwostepFec_toSign>(
      broadcast_id, broadcast->data_size_, broadcast->seqno_, broadcast->part_.clone());
  TRY_RESULT(check_result,
             check_signature_and_certificate(overlay, src_key, src_keyhash, to_sign, broadcast->signature_,
                                             broadcast->certificate_, static_cast<td::uint32>(data_size)));
  bool will_rebroadcast = src_peer_id == bcast_src_adnl_id;
  if (will_rebroadcast) {
    rebroadcast(overlay, bcast_src_adnl_id, serialize_tl_object(broadcast, true));
  }
  auto it = broadcasts_.find(broadcast_id);
  bool is_new = (it == broadcasts_.end());
  if (is_new) {
    if (overlay->is_delivered(broadcast_id)) {
      VLOG(TWOSTEP_DEBUG) << "twostep DUPLICATE receiver broadcast_id=" << broadcast_id.to_hex()
                          << " seqno=" << broadcast->seqno_;
      return td::Status::Error(ErrorCode::notready, "duplicate broadcast");
    }
    td::Result<std::unique_ptr<td::raptorq::Decoder>> R;
    if (part_size == 0 ||
        (R = td::raptorq::Decoder::create({(data_size + part_size - 1) / part_size, part_size, data_size}))
            .is_error()) {
      return td::Status::Error(ErrorCode::protoviolation, "invalid FEC parameters");
    }
    td::uint32 symbols_needed = static_cast<td::uint32>((data_size + part_size - 1) / part_size);
    std::unique_ptr<BroadcastTwostep> bcast(
        new BroadcastTwostep{.broadcast_id = broadcast_id,
                             .date = date,
                             .decoder = R.move_as_ok(),
                             .debug = {.src_adnl_id = bcast_src_adnl_id,
                                       .data_hash = broadcast->data_hash_,
                                       .data_size = static_cast<td::uint32>(data_size),
                                       .symbols_received = 0,
                                       .symbols_needed = symbols_needed,
                                       .timestamp = td::Timestamp::now(),
                                       .chunk_senders = {}}});
    lru_.put(bcast.get());
    it = broadcasts_.emplace(broadcast_id, std::move(bcast)).first;
    VLOG(TWOSTEP_INFO) << "twostep START receiver " << *it->second << " from=" << src_peer_id;
  }
  auto bcast = it->second.get();
  bcast->debug.chunk_senders.insert(src_peer_id);
  TRY_STATUS(bcast->decoder->add_symbol({static_cast<std::uint32_t>(broadcast->seqno_), broadcast->part_.as_slice()}));
  bcast->debug.symbols_received++;
  VLOG(TWOSTEP_INFO) << "twostep RECV_CHUNK receiver " << *bcast << " seqno=" << broadcast->seqno_
                     << " from=" << src_peer_id << " will_rebroadcast=" << will_rebroadcast;
  if (bcast->decoder->may_try_decode()) {
    TRY_RESULT(R, bcast->decoder->try_decode(false));
    VLOG(TWOSTEP_INFO) << "twostep FINISH receiver " << *bcast << " decoded=true elapsed=" << bcast->debug.elapsed();
    broadcasts_.erase(it);
    overlay->register_delivered_broadcast(broadcast_id);
    check_and_deliver(overlay, src_keyhash, check_result, std::move(R.data));
  }
  return td::Status::OK();
}

void BroadcastsTwostep::checked(OverlayImpl *overlay, PublicKeyHash &&src, td::BufferSlice &&data,
                                td::Result<td::Unit> &&R) {
  if (R.is_ok()) {
    overlay->deliver_broadcast(src, std::move(data));
  }
}

void BroadcastsTwostep::gc(OverlayImpl *overlay) {
  while (!broadcasts_.empty()) {
    auto bcast = static_cast<BroadcastTwostep *>(lru_.prev);
    CHECK(bcast);
    if (bcast->date > td::Clocks::system() - 60) {
      break;
    }
    auto broadcast_id = bcast->broadcast_id;

    FLOG(INFO) {
      sb << "twostep GC_INCOMPLETE receiver " << *bcast << " decoded=false elapsed=" << bcast->debug.elapsed() << " ";
      bcast->debug.print_senders(sb);
    };
    CHECK(broadcasts_.erase(broadcast_id));
    overlay->register_delivered_broadcast(broadcast_id);
  }
}

}  // namespace overlay

}  // namespace ton
