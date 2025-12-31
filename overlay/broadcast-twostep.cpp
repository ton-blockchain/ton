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

static constexpr std::size_t FEC_MIN_BYTES = 513;
static constexpr std::size_t FEC_MIN_OTHER_NODES = 4;

static constexpr std::size_t fec_k(std::size_t other_nodes) {
  return (other_nodes * 2 - 2) / 3;
}

struct BroadcastTwostep : td::ListNode {
  Overlay::BroadcastHash broadcast_id;
  td::uint32 date;
  std::unique_ptr<td::raptorq::Decoder> decoder;
};

struct BroadcastTwostepDataSimple {
  td::uint32 flags;
  td::uint32 date;
  adnl::AdnlNodeIdShort src;
  std::vector<adnl::AdnlNodeIdShort> dsts;
  td::BufferSlice data;
};

struct BroadcastTwostepDataFec {
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
  overlay->iterate_all_peers([&](const adnl::AdnlNodeIdShort &key, OverlayPeer &peer) {
    if (overlay->is_persistent_node(key) && key.pubkey_hash() != send_as) {
      other_nodes.push_back(key);
    }
  });
  td::Bits256 broadcast_id;
  if (data_size >= FEC_MIN_BYTES && other_nodes.size() >= FEC_MIN_OTHER_NODES) {
    std::size_t k = fec_k(other_nodes.size());
    std::size_t part_size = (data_size + k - 1) / k;
    CHECK(part_size < data_size);
    broadcast_id = get_tl_object_sha_bits256(create_tl_object<ton_api::overlay_broadcastTwostep_id>(
        flags, date, send_as.bits256_value(), overlay->local_id().bits256_value(), data_hash,
        static_cast<std::int32_t>(part_size)));
    auto R = td::raptorq::Encoder::create(part_size, data.clone());
    if (R.is_error()) {
      VLOG(OVERLAY_WARNING) << overlay << ": cannot create FEC encoder: " << R.move_as_error();
      return;
    }
    auto encoder = R.move_as_ok();
    encoder->precalc();
    for (std::size_t i = 0; i < other_nodes.size(); i++) {
      td::uint32 seqno = static_cast<std::uint32_t>(i);
      td::BufferSlice part(part_size);
      td::Status S = encoder->gen_symbol(seqno, part.as_slice());
      if (S.is_error()) {
        VLOG(OVERLAY_WARNING) << overlay << ": cannot generate symbol: " << S;
        continue;
      }
      td::BufferSlice to_sign = create_serialize_tl_object<ton_api::overlay_broadcastTwostepFec_toSign>(
          broadcast_id, static_cast<std::int32_t>(data_size), static_cast<std::int32_t>(seqno), part.clone());
      BroadcastTwostepDataFec passdata{flags,          date,           adnl::AdnlNodeIdShort(send_as),
                                       other_nodes[i], data_hash,      static_cast<td::uint32>(data_size),
                                       seqno,          std::move(part)};
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
    td::BufferSlice to_sign =
        create_serialize_tl_object<ton_api::overlay_broadcastTwostepSimple_toSign>(broadcast_id, data.clone());
    BroadcastTwostepDataSimple passdata{flags, date, adnl::AdnlNodeIdShort(send_as), std::move(other_nodes),
                                        data.clone()};
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
  td::Bits256 broadcast_id = get_tl_object_sha_bits256(create_tl_object<ton_api::overlay_broadcastTwostep_id>(
      broadcast->flags_, broadcast->date_, src_keyhash.bits256_value(), bcast_src_adnl_id.bits256_value(),
      sha256_bits256(broadcast->data_.as_slice()), static_cast<std::int32_t>(broadcast->data_.size())));
  td::BufferSlice to_sign = create_serialize_tl_object<ton_api::overlay_broadcastTwostepSimple_toSign>(
      broadcast_id, broadcast->data_.clone());
  TRY_RESULT(check_result, check_signature_and_certificate(overlay, src_key, src_keyhash, to_sign,
                                                           broadcast->signature_, broadcast->certificate_,
                                                           static_cast<td::uint32>(broadcast->data_.size())));
  if (src_peer_id == bcast_src_adnl_id) {
    rebroadcast(overlay, bcast_src_adnl_id, serialize_tl_object(broadcast, true));
  }
  if (overlay->is_delivered(broadcast_id)) {
    return td::Status::Error(ErrorCode::notready, "duplicate broadcast");
  }
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
  if (src_peer_id == bcast_src_adnl_id) {
    rebroadcast(overlay, bcast_src_adnl_id, serialize_tl_object(broadcast, true));
  }
  auto it = broadcasts_.find(broadcast_id);
  if (it == broadcasts_.end()) {
    if (overlay->is_delivered(broadcast_id)) {
      return td::Status::Error(ErrorCode::notready, "duplicate broadcast");
    }
    td::Result<std::unique_ptr<td::raptorq::Decoder>> R;
    if (part_size == 0 ||
        (R = td::raptorq::Decoder::create({(data_size + part_size - 1) / part_size, part_size, data_size}))
            .is_error()) {
      return td::Status::Error(ErrorCode::protoviolation, "invalid FEC parameters");
    }
    std::unique_ptr<BroadcastTwostep> bcast(new BroadcastTwostep{{}, broadcast_id, date, R.move_as_ok()});
    lru_.put(bcast.get());
    it = broadcasts_.emplace(broadcast_id, std::move(bcast)).first;
  }
  auto bcast = it->second.get();
  TRY_STATUS(bcast->decoder->add_symbol({static_cast<std::uint32_t>(broadcast->seqno_), broadcast->part_.as_slice()}));
  if (bcast->decoder->may_try_decode()) {
    TRY_RESULT(R, bcast->decoder->try_decode(false));
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
    auto bcast = static_cast<BroadcastTwostep *>(lru_.get());
    if (bcast->date > td::Clocks::system() - 60) {
      break;
    }
    broadcasts_.erase(bcast->broadcast_id);
    overlay->register_delivered_broadcast(bcast->broadcast_id);
  }
}

}  // namespace overlay

}  // namespace ton
