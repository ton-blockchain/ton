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
#include "overlay-fec-broadcast.hpp"
#include "overlay.hpp"
#include "keys/encryptor.h"

namespace ton {

namespace overlay {

td::Result<std::unique_ptr<BroadcastFec>> BroadcastFec::create(Overlay::BroadcastHash hash, PublicKey src,
                                                               Overlay::BroadcastDataHash data_hash, td::uint32 flags,
                                                               td::uint32 date, fec::FecType fec_type) {
  auto F = std::make_unique<BroadcastFec>(hash, std::move(src), data_hash, flags, date, std::move(fec_type));
  TRY_STATUS(F->run_checks());
  TRY_STATUS(F->init_fec_type());
  return std::move(F);
}

td::Status BroadcastFec::run_checks() {
  if (fec_type_.size() > Overlays::max_fec_broadcast_size()) {
    return td::Status::Error(ErrorCode::protoviolation, "too big fec broadcast");
  }
  return td::Status::OK();
}

td::Status OverlayFecBroadcastPart::check_time() {
  return overlay_->check_date(date_);
}

td::Status OverlayFecBroadcastPart::check_duplicate() {
  TRY_STATUS(overlay_->check_delivered(broadcast_hash_));

  if (bcast_ && bcast_->received_part(seqno_)) {
    return td::Status::Error(ErrorCode::notready, "duplicate part");
  }
  return td::Status::OK();
}

td::Status OverlayFecBroadcastPart::check_source() {
  auto r = overlay_->check_source_eligible(source_, cert_.get(), broadcast_size_, true);
  if (r == BroadcastCheckResult::Forbidden) {
    return td::Status::Error(ErrorCode::error, "broadcast is forbidden");
  }

  if (r == BroadcastCheckResult::NeedCheck) {
    untrusted_ = true;
    return td::Status::OK();
  }

  if (bcast_) {
    TRY_STATUS(bcast_->is_eligible_sender(source_));
  }

  return td::Status::OK();
}

td::Status OverlayFecBroadcastPart::check_signature() {
  TRY_RESULT(encryptor, overlay_->get_encryptor(source_));

  return encryptor->check_signature(to_sign().as_slice(), signature_.as_slice());
}

td::Status OverlayFecBroadcastPart::run_checks() {

  TRY_STATUS(check_time());
  TRY_STATUS(check_duplicate());
  TRY_STATUS(check_source());
  TRY_STATUS(check_signature());
  return td::Status::OK();
}

void BroadcastFec::broadcast_checked(td::Result<td::Unit> R) {
  if (R.is_error()) {
    td::actor::send_closure(actor_id(overlay_), &OverlayImpl::update_peer_err_ctr, src_peer_id_, true);
    return;
  }
  overlay_->deliver_broadcast(get_source().compute_short_id(), data_.clone());
  auto manager = overlay_->overlay_manager();
  while (!parts_.empty()) {
       distribute_part(parts_.begin()->first);
  }
}

// Do we need status here??
td::Status  BroadcastFec::distribute_part(td::uint32 seqno) {
  auto i = parts_.find(seqno);
  if (i == parts_.end()) {
    // should not get here
    return td::Status::OK();
  }
  auto tls = std::move(i->second);
  parts_.erase(i);
  td::BufferSlice data_short = std::move(tls.first);
  td::BufferSlice data = std::move(tls.second);

  auto nodes = overlay_->get_neighbours(5);
  auto manager = overlay_->overlay_manager();

  for (auto &n : nodes) {
    if (neighbour_completed(n)) {
      continue;
    }
    if (neighbour_received(n)) {
      td::actor::send_closure(manager, &OverlayManager::send_message, n, overlay_->local_id(), overlay_->overlay_id(),
                              data_short.clone());
    } else {
      if (hash_.count_leading_zeroes() >= 12) {
        VLOG(OVERLAY_INFO) << "broadcast " << hash_ << ": sending part " << seqno << " to " << n;
      }
      td::actor::send_closure(manager, &OverlayManager::send_message, n, overlay_->local_id(), overlay_->overlay_id(),
                              data.clone());
    }
  }
  return td::Status::OK();
}

td::Status OverlayFecBroadcastPart::apply() {

  if (!bcast_) {
    bcast_ = overlay_->get_fec_broadcast(broadcast_hash_);
  }
  if (!bcast_) {
    if (is_short_) {
      return td::Status::Error(ErrorCode::protoviolation, "short broadcast part for incomplete broadcast");
    }
    TRY_RESULT(B, BroadcastFec::create(broadcast_hash_, source_, broadcast_data_hash_, flags_, date_, fec_type_));
    bcast_ = B.get();
    overlay_->register_fec_broadcast(std::move(B));
  }
  if (bcast_->received_part(seqno_)) {
    return td::Status::Error(ErrorCode::notready, "duplicate part");
  } else {
    bcast_->add_received_part(seqno_);
  }

  if (!bcast_->finalized() && is_short_) {
    return td::Status::Error(ErrorCode::protoviolation, "short broadcast part for incomplete broadcast");
  }

  if (!bcast_->finalized()) {
    bcast_->set_overlay(overlay_);
    bcast_->set_src_peer_id(src_peer_id_);
    TRY_STATUS(bcast_->add_part(seqno_, data_.clone(), export_serialized_short(), export_serialized()));
    auto R = bcast_->finish();
    if (R.is_error()) {
      auto S = R.move_as_error();
      if (S.code() != ErrorCode::notready) {
        return S;
      }
    } else {
      if(untrusted_) {
        auto P = td::PromiseCreator::lambda(
              [id = broadcast_hash_, overlay_id = actor_id(overlay_)](td::Result<td::Unit> RR) mutable {
                td::actor::send_closure(std::move(overlay_id), &OverlayImpl::broadcast_checked, id, std::move(RR));
              });
        overlay_->check_broadcast(bcast_->get_source().compute_short_id(), R.move_as_ok(), std::move(P));
      } else {
        overlay_->deliver_broadcast(bcast_->get_source().compute_short_id(), R.move_as_ok());
      }
    }
  }
  return td::Status::OK();
}

td::Status OverlayFecBroadcastPart::distribute() {
  TRY_STATUS(bcast_->distribute_part(seqno_));
  return td::Status::OK();
}

tl_object_ptr<ton_api::overlay_broadcastFec> OverlayFecBroadcastPart::export_tl() {
  if (data_.size() == 0) {
    data_ = bcast_->get_part(seqno_);
  }

  return create_tl_object<ton_api::overlay_broadcastFec>(
      source_.tl(), cert_ ? cert_->tl() : Certificate::empty_tl(), bcast_->get_data_hash(), bcast_->get_size(),
      bcast_->get_flags(), data_.clone(), seqno_, bcast_->get_fec_type().tl(), bcast_->get_date(), signature_.clone());
}

tl_object_ptr<ton_api::overlay_broadcastFecShort> OverlayFecBroadcastPart::export_tl_short() {
  return create_tl_object<ton_api::overlay_broadcastFecShort>(
      source_.tl(), cert_ ? cert_->tl() : Certificate::empty_tl(), broadcast_hash_, part_data_hash_, seqno_,
      signature_.clone());
}

td::BufferSlice OverlayFecBroadcastPart::export_serialized() {
  return serialize_tl_object(export_tl(), true);
}

td::BufferSlice OverlayFecBroadcastPart::export_serialized_short() {
  return serialize_tl_object(export_tl_short(), true);
}

td::BufferSlice OverlayFecBroadcastPart::to_sign() {
  auto obj = create_tl_object<ton_api::overlay_broadcast_toSign>(part_hash_, date_);
  return serialize_tl_object(obj, true);
}

td::Status OverlayFecBroadcastPart::create(OverlayImpl *overlay, adnl::AdnlNodeIdShort src_peer_id,
                                           tl_object_ptr<ton_api::overlay_broadcastFec> broadcast) {
  TRY_STATUS(overlay->check_date(broadcast->date_));
  auto source = PublicKey{broadcast->src_};
  auto part_data_hash = sha256_bits256(broadcast->data_.as_slice());

  TRY_RESULT(fec_type, fec::FecType::create(std::move(broadcast->fec_)));
  auto broadcast_hash =
      compute_broadcast_id(source, fec_type, broadcast->data_hash_, broadcast->data_size_, broadcast->flags_);
  auto part_hash = compute_broadcast_part_id(broadcast_hash, part_data_hash, broadcast->seqno_);

  if (broadcast_hash.count_leading_zeroes() >= 12) {
    VLOG(OVERLAY_INFO) << "broadcast " << broadcast_hash << ": received part " << part_hash;
  }

  TRY_STATUS(overlay->check_delivered(broadcast_hash));
  TRY_RESULT(cert, Certificate::create(std::move(broadcast->certificate_)));

  OverlayFecBroadcastPart B{broadcast_hash,
                            part_hash,
                            source,
                            std::move(cert),
                            broadcast->data_hash_,
                            static_cast<td::uint32>(broadcast->data_size_),
                            static_cast<td::uint32>(broadcast->flags_),
                            part_data_hash,
                            std::move(broadcast->data_),
                            static_cast<td::uint32>(broadcast->seqno_),
                            std::move(fec_type),
                            static_cast<td::uint32>(broadcast->date_),
                            std::move(broadcast->signature_),
                            false,
                            overlay->get_fec_broadcast(broadcast_hash),
                            overlay,
                            src_peer_id};
  TRY_STATUS(B.run());
  return td::Status::OK();
}

td::Status OverlayFecBroadcastPart::create(OverlayImpl *overlay, adnl::AdnlNodeIdShort src_peer_id,
                                           tl_object_ptr<ton_api::overlay_broadcastFecShort> broadcast) {
  auto bcast = overlay->get_fec_broadcast(broadcast->broadcast_hash_);
  if (!bcast) {
    return td::Status::Error(ErrorCode::notready, "short part of unknown broadcast");
  }
  if (!bcast->finalized()) {
    return td::Status::Error(ErrorCode::protoviolation, "short part of not finished broadcast");
  }

  TRY_STATUS(overlay->check_date(bcast->get_date()));

  auto source = PublicKey{broadcast->src_};
  auto part_data_hash = broadcast->part_data_hash_;

  auto broadcast_hash = bcast->get_hash();
  auto part_hash = compute_broadcast_part_id(broadcast_hash, part_data_hash, broadcast->seqno_);

  TRY_STATUS(overlay->check_delivered(broadcast_hash));
  TRY_RESULT(cert, Certificate::create(std::move(broadcast->certificate_)));

  OverlayFecBroadcastPart B{broadcast_hash,
                            part_hash,
                            source,
                            std::move(cert),
                            bcast->get_data_hash(),
                            bcast->get_size(),
                            bcast->get_flags(),
                            part_data_hash,
                            td::BufferSlice{},
                            static_cast<td::uint32>(broadcast->seqno_),
                            bcast->get_fec_type(),
                            bcast->get_date(),
                            std::move(broadcast->signature_),
                            true,
                            bcast,
                            overlay,
                            src_peer_id};
  TRY_STATUS(B.run());
  return td::Status::OK();
}

td::Status OverlayFecBroadcastPart::create_new(OverlayImpl *overlay, td::actor::ActorId<OverlayImpl> overlay_actor_id,
                                               PublicKeyHash local_id, Overlay::BroadcastDataHash data_hash,
                                               td::uint32 size, td::uint32 flags, td::BufferSlice part,
                                               td::uint32 seqno, fec::FecType fec_type, td::uint32 date) {
  auto broadcast_hash = compute_broadcast_id(local_id, fec_type, data_hash, size, flags);
  auto part_data_hash = sha256_bits256(part.as_slice());
  auto part_hash = compute_broadcast_part_id(broadcast_hash, part_data_hash, seqno);

  auto B = std::make_unique<OverlayFecBroadcastPart>(
      broadcast_hash, part_hash, PublicKey{}, overlay->get_certificate(local_id), data_hash, size, flags,
      part_data_hash, std::move(part), seqno, std::move(fec_type), date, td::BufferSlice{}, false, nullptr, overlay, adnl::AdnlNodeIdShort::zero());
  auto to_sign = B->to_sign();

  auto P = td::PromiseCreator::lambda(
      [id = overlay_actor_id, local_id, B = std::move(B)](td::Result<std::pair<td::BufferSlice, PublicKey>> R) mutable {
        if (R.is_error()) {
          td::actor::send_closure(id, &OverlayImpl::failed_to_create_fec_broadcast, R.move_as_error());
          return;
        }
        auto V = R.move_as_ok();
        auto pub_id = V.second;
        B->update_source(pub_id);
        B->update_signature(std::move(V.first));
        td::actor::send_closure(id, &OverlayImpl::created_fec_broadcast, local_id, std::move(B));
      });
  td::actor::send_closure(overlay->keyring(), &keyring::Keyring::sign_add_get_public_key, local_id, std::move(to_sign),
                          std::move(P));
  return td::Status::OK();
}

Overlay::BroadcastHash OverlayFecBroadcastPart::compute_broadcast_id(PublicKey source, const fec::FecType &fec_type,
                                                                     Overlay::BroadcastDataHash data_hash,
                                                                     td::uint32 size, td::uint32 flags) {
  return compute_broadcast_id(source.compute_short_id(), fec_type, data_hash, size, flags);
}

Overlay::BroadcastHash OverlayFecBroadcastPart::compute_broadcast_id(PublicKeyHash source, const fec::FecType &fec_type,
                                                                     Overlay::BroadcastDataHash data_hash,
                                                                     td::uint32 size, td::uint32 flags) {
  return get_tl_object_sha_bits256(create_tl_object<ton_api::overlay_broadcastFec_id>(
      (flags & Overlays::BroadcastFlagAnySender()) ? PublicKeyHash::zero().tl() : source.tl(),
      get_tl_object_sha_bits256(fec_type.tl()), data_hash, size, flags));
}

Overlay::BroadcastPartHash OverlayFecBroadcastPart::compute_broadcast_part_id(Overlay::BroadcastHash broadcast_hash,
                                                                              Overlay::BroadcastDataHash data_hash,
                                                                              td::uint32 seqno) {
  return get_tl_object_sha_bits256(
      create_tl_object<ton_api::overlay_broadcastFec_partId>(broadcast_hash, data_hash, seqno));
}

void OverlayFecBroadcastPart::update_overlay(OverlayImpl *overlay) {
  if (overlay_) {
    return;
  }
  overlay_ = overlay;
  cert_ = overlay_->get_certificate(source_.compute_short_id());
}

}  // namespace overlay

}  // namespace ton
