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
#include "overlay-broadcast.hpp"
#include "adnl/adnl-node-id.hpp"
#include "common/util.h"
#include "overlay.hpp"
#include "keys/encryptor.h"
#include "td/actor/PromiseFuture.h"
#include "td/actor/actor.h"
#include "td/utils/Status.h"
#include "td/utils/port/Stat.h"

namespace ton {

namespace overlay {

td::Status BroadcastSimple::check_time() {
  return overlay_->check_date(date_);
}

td::Status BroadcastSimple::check_duplicate() {
  return overlay_->check_delivered(broadcast_hash_);
}

td::Status BroadcastSimple::check_source() {
  auto r = overlay_->check_source_eligible(source_, cert_.get(), data_size(), false);
  if (r == BroadcastCheckResult::Forbidden) {
    return td::Status::Error(ErrorCode::error, "broadcast is forbidden");
  }

  is_valid_ = r == BroadcastCheckResult::Allowed;
  return td::Status::OK();
}

td::BufferSlice BroadcastSimple::to_sign() {
  return create_serialize_tl_object<ton_api::overlay_broadcast_toSign>(broadcast_hash_, date_);
}

td::Status BroadcastSimple::check_signature() {
  TRY_RESULT(encryptor, overlay_->get_encryptor(source_));

  return encryptor->check_signature(to_sign().as_slice(), signature_.as_slice());
}

td::Status BroadcastSimple::run_checks() {
  TRY_STATUS(check_time());
  TRY_STATUS(check_duplicate());
  TRY_STATUS(check_source());
  TRY_STATUS(check_signature());
  return td::Status::OK();
}

td::Status BroadcastSimple::distribute() {
  auto B = serialize();
  auto nodes = overlay_->get_neighbours(3);

  auto manager = overlay_->overlay_manager();
  for (auto &n : nodes) {
    td::actor::send_closure(manager, &OverlayManager::send_message, n, overlay_->local_id(), overlay_->overlay_id(),
                            B.clone());
  }
  return td::Status::OK();
}

void BroadcastSimple::broadcast_checked(td::Result<td::Unit> R) {
  if (R.is_error()) {
    return;
  }
  is_valid_ = true;
  run_continue().ignore();
}

tl_object_ptr<ton_api::overlay_broadcast> BroadcastSimple::tl() const {
  return create_tl_object<ton_api::overlay_broadcast>(source_.tl(), cert_ ? cert_->tl() : Certificate::empty_tl(),
                                                      flags_, data_.clone(), date_, signature_.clone());
}

td::BufferSlice BroadcastSimple::serialize() {
  return serialize_tl_object(tl(), true);
}

td::Status BroadcastSimple::run_continue() {
  TRY_STATUS(distribute());
  deliver();
  return td::Status::OK();
}

td::Status BroadcastSimple::run() {
  TRY_STATUS(run_checks());
  if (!is_valid_) {
    auto P = td::PromiseCreator::lambda(
        [id = broadcast_hash_, overlay_id = actor_id(overlay_)](td::Result<td::Unit> R) mutable {
          td::actor::send_closure(std::move(overlay_id), &OverlayImpl::broadcast_checked, id, std::move(R));
        });
    overlay_->check_broadcast(source_.compute_short_id(), data_.clone(), std::move(P));
    return td::Status::OK();
  }
  return run_continue();
}

td::Status BroadcastSimple::create(OverlayImpl *overlay, tl_object_ptr<ton_api::overlay_broadcast> broadcast) {
  auto src = PublicKey{broadcast->src_};
  auto data_hash = sha256_bits256(broadcast->data_.as_slice());
  auto broadcast_hash = compute_broadcast_id(src, data_hash, broadcast->flags_);

  TRY_STATUS(overlay->check_date(broadcast->date_));
  TRY_STATUS(overlay->check_delivered(broadcast_hash));
  TRY_RESULT(cert, Certificate::create(std::move(broadcast->certificate_)));

  auto B = std::make_unique<BroadcastSimple>(broadcast_hash, src, std::move(cert), broadcast->flags_,
                                             std::move(broadcast->data_), broadcast->date_,
                                             std::move(broadcast->signature_), false, overlay);
  TRY_STATUS(B->run());
  overlay->register_simple_broadcast(std::move(B));
  return td::Status::OK();
}

td::Status BroadcastSimple::create_new(td::actor::ActorId<OverlayImpl> overlay,
                                       td::actor::ActorId<keyring::Keyring> keyring, PublicKeyHash local_id,
                                       td::BufferSlice data, td::uint32 flags) {
  auto data_hash = sha256_bits256(data.as_slice());
  auto broadcast_hash = compute_broadcast_id(local_id, data_hash, flags);
  auto date = static_cast<td::uint32>(td::Clocks::system());

  auto B = std::make_unique<BroadcastSimple>(broadcast_hash, PublicKey{}, nullptr, flags, std::move(data), date,
                                             td::BufferSlice{}, false, nullptr);

  auto to_sign = B->to_sign();
  auto P = td::PromiseCreator::lambda(
      [id = overlay, B = std::move(B)](td::Result<std::pair<td::BufferSlice, PublicKey>> R) mutable {
        if (R.is_error()) {
          td::actor::send_closure(id, &OverlayImpl::failed_to_create_simple_broadcast, R.move_as_error());
          return;
        }
        auto V = R.move_as_ok();
        auto pub_id = V.second;
        B->update_source(pub_id);
        B->update_signature(std::move(V.first));
        td::actor::send_closure(id, &OverlayImpl::created_simple_broadcast, std::move(B));
      });
  td::actor::send_closure(keyring, &keyring::Keyring::sign_add_get_public_key, local_id, std::move(to_sign),
                          std::move(P));
  return td::Status::OK();
}

Overlay::BroadcastHash BroadcastSimple::compute_broadcast_id(PublicKeyHash source, Overlay::BroadcastDataHash data_hash,
                                                             td::uint32 flags) {
  auto obj = create_tl_object<ton_api::overlay_broadcast_id>(
      (flags & Overlays::BroadcastFlagAnySender()) ? PublicKeyHash::zero().tl() : source.tl(), data_hash, flags);
  return get_tl_object_sha_bits256(obj);
}

Overlay::BroadcastHash BroadcastSimple::compute_broadcast_id(PublicKey source, Overlay::BroadcastDataHash data_hash,
                                                             td::uint32 flags) {
  return compute_broadcast_id(source.compute_short_id(), data_hash, flags);
}

void BroadcastSimple::update_overlay(OverlayImpl *overlay) {
  if (overlay_) {
    return;
  }
  overlay_ = overlay;
  cert_ = overlay->get_certificate(source_.compute_short_id());
}

void BroadcastSimple::deliver() {
  overlay_->deliver_broadcast(source_.compute_short_id(), data_.clone());
}

}  // namespace overlay

}  // namespace ton
