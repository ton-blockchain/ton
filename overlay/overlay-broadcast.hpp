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

#include "adnl/adnl-local-id.h"
#include "adnl/adnl-node-id.hpp"
#include "auto/tl/ton_api.h"
#include "common/refcnt.hpp"
#include "overlay/overlay.h"
#include "td/actor/PromiseFuture.h"
#include "td/utils/List.h"
#include "td/utils/Status.h"
#include "td/utils/buffer.h"
#include "td/utils/common.h"

namespace ton {

namespace overlay {

class OverlayImpl;

class BroadcastSimple : public td::ListNode {
 private:
  Overlay::BroadcastHash broadcast_hash_;

  PublicKey source_;
  std::shared_ptr<Certificate> cert_;
  td::uint32 flags_;
  td::BufferSlice data_;
  td::uint32 date_;
  td::BufferSlice signature_;
  bool is_valid_{false};

  OverlayImpl *overlay_;

  td::Status check_time();
  td::Status check_duplicate();
  td::Status check_source();
  td::Status check_signature();

  td::Status run_checks();
  td::Status distribute();
  td::BufferSlice to_sign();

 public:
  BroadcastSimple(Overlay::BroadcastHash broadcast_hash, PublicKey source, std::shared_ptr<Certificate> cert,
                  td::uint32 flags, td::BufferSlice data, td::uint32 date, td::BufferSlice signature, bool is_valid,
                  OverlayImpl *overlay)
      : broadcast_hash_(broadcast_hash)
      , source_(std::move(source))
      , cert_(std::move(cert))
      , flags_(flags)
      , data_(std::move(data))
      , date_(date)
      , signature_(std::move(signature))
      , is_valid_(is_valid)
      , overlay_(overlay) {
  }

  Overlay::BroadcastHash get_hash() const {
    return broadcast_hash_;
  }

  td::uint32 data_size() const {
    return static_cast<td::uint32>(data_.size());
  }

  void update_source(PublicKey source) {
    source_ = source;
  }
  void update_signature(td::BufferSlice signature) {
    signature_ = std::move(signature);
  }
  void deliver();

  td::Status run();
  td::Status run_continue();

  tl_object_ptr<ton_api::overlay_broadcast> tl() const;
  td::BufferSlice serialize();

  void update_overlay(OverlayImpl *overlay);
  void broadcast_checked(td::Result<td::Unit> R);

  static td::Status create(OverlayImpl *overlay, tl_object_ptr<ton_api::overlay_broadcast> broadcast);
  static td::Status create_new(td::actor::ActorId<OverlayImpl> overlay, td::actor::ActorId<keyring::Keyring> keyring,
                               PublicKeyHash local_id, td::BufferSlice data, td::uint32 flags);

  static Overlay::BroadcastHash compute_broadcast_id(PublicKey source, Overlay::BroadcastDataHash data_hash,
                                                     td::uint32 flags);
  static Overlay::BroadcastHash compute_broadcast_id(PublicKeyHash source, Overlay::BroadcastDataHash data_hash,
                                                     td::uint32 flags);

  static BroadcastSimple *from_list_node(ListNode *node) {
    return static_cast<BroadcastSimple *>(node);
  }
};

}  // namespace overlay

}  // namespace ton
