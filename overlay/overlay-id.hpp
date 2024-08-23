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

#include "auto/tl/ton_api.h"
#include "adnl/adnl-node-id.hpp"
#include "overlay/overlays.h"
#include "td/utils/SharedSlice.h"
#include "td/utils/buffer.h"
#include "td/utils/overloaded.h"
#include "keys/encryptor.h"
#include "td/utils/port/StdStreams.h"
#include "td/utils/unique_ptr.h"
#include <limits>
#include <memory>

namespace ton {

namespace overlay {

class OverlayNode {
 public:
  explicit OverlayNode(adnl::AdnlNodeIdShort self_id, OverlayIdShort overlay, td::uint32 flags) {
    source_ = self_id;
    overlay_ = overlay;
    flags_ = flags;
    version_ = static_cast<td::int32>(td::Clocks::system());
  }
  static td::Result<OverlayNode> create(const tl_object_ptr<ton_api::overlay_node> &node) {
    TRY_RESULT(source, adnl::AdnlNodeIdFull::create(node->id_));
    return OverlayNode{source, OverlayIdShort{node->overlay_}, 0, node->version_, node->signature_.as_slice()};
  }
  static td::Result<OverlayNode> create(const tl_object_ptr<ton_api::overlay_nodeV2> &node) {
    TRY_RESULT(source, adnl::AdnlNodeIdFull::create(node->id_));
    auto res = OverlayNode{source, OverlayIdShort{node->overlay_}, (td::uint32)node->flags_, node->version_,
                           node->signature_.as_slice()};
    res.update_certificate(OverlayMemberCertificate(node->certificate_.get()));
    return res;
  }
  OverlayNode(td::Variant<adnl::AdnlNodeIdFull, adnl::AdnlNodeIdShort> source, OverlayIdShort overlay, td::uint32 flags,
              td::int32 version, td::Slice signature)
      : source_(std::move(source))
      , overlay_(overlay)
      , flags_(flags)
      , version_(version)
      , signature_(td::SharedSlice(signature)) {
  }
  OverlayNode(td::Variant<adnl::AdnlNodeIdFull, adnl::AdnlNodeIdShort> source, OverlayIdShort overlay,
              td::int32 version, td::SharedSlice signature)
      : source_(std::move(source)), overlay_(overlay), version_(version), signature_(std::move(signature)) {
  }
  td::Status check_signature() {
    td::Status res;
    source_.visit(td::overloaded(
        [&](const adnl::AdnlNodeIdShort &id) { res = td::Status::Error(ErrorCode::notready, "fullid not set"); },
        [&](const adnl::AdnlNodeIdFull &id) {
          auto E = id.pubkey().create_encryptor();
          if (E.is_error()) {
            res = E.move_as_error();
            return;
          }
          auto enc = E.move_as_ok();
          res = enc->check_signature(to_sign().as_slice(), signature_.as_slice());
        }));
    return res;
  }

  td::BufferSlice to_sign() const {
    if (flags_ == 0) {
      auto obj = create_tl_object<ton_api::overlay_node_toSign>(nullptr, overlay_.tl(), version_);
      source_.visit(td::overloaded([&](const adnl::AdnlNodeIdShort &id) { obj->id_ = id.tl(); },
                                   [&](const adnl::AdnlNodeIdFull &id) { obj->id_ = id.compute_short_id().tl(); }));
      return serialize_tl_object(obj, true);
    } else {
      auto obj = create_tl_object<ton_api::overlay_node_toSignEx>(nullptr, overlay_.tl(), flags_, version_);
      source_.visit(td::overloaded([&](const adnl::AdnlNodeIdShort &id) { obj->id_ = id.tl(); },
                                   [&](const adnl::AdnlNodeIdFull &id) { obj->id_ = id.compute_short_id().tl(); }));
      return serialize_tl_object(obj, true);
    }
  }
  void update_adnl_id(adnl::AdnlNodeIdFull node_id) {
    source_ = node_id;
  }
  void update_signature(td::Slice signature) {
    signature_ = td::SharedSlice(signature);
  }
  OverlayIdShort overlay_id() const {
    return overlay_;
  }
  td::int32 version() const {
    return version_;
  }
  td::uint32 flags() const {
    return flags_;
  }
  td::BufferSlice signature() const {
    return signature_.clone_as_buffer_slice();
  }
  adnl::AdnlNodeIdShort adnl_id_short() const {
    adnl::AdnlNodeIdShort res;
    source_.visit(td::overloaded([&](const adnl::AdnlNodeIdShort &id) { res = id; },
                                 [&](const adnl::AdnlNodeIdFull &id) { res = id.compute_short_id(); }));
    return res;
  };
  adnl::AdnlNodeIdFull adnl_id_full() const {
    adnl::AdnlNodeIdFull res;
    source_.visit(td::overloaded([&](const adnl::AdnlNodeIdShort &id) { UNREACHABLE(); },
                                 [&](const adnl::AdnlNodeIdFull &id) { res = id; }));
    return res;
  };
  tl_object_ptr<ton_api::overlay_node> tl() const {
    auto obj =
        create_tl_object<ton_api::overlay_node>(nullptr, overlay_.tl(), version_, signature_.clone_as_buffer_slice());
    source_.visit(td::overloaded([&](const adnl::AdnlNodeIdShort &id) { UNREACHABLE(); },
                                 [&](const adnl::AdnlNodeIdFull &id) { obj->id_ = id.tl(); }));
    return obj;
  }
  tl_object_ptr<ton_api::overlay_nodeV2> tl_v2() const {
    tl_object_ptr<ton_api::overlay_MemberCertificate> cert;
    if (cert_ && !cert_->empty()) {
      cert = cert_->tl();
    } else {
      cert = create_tl_object<ton_api::overlay_emptyMemberCertificate>();
    }
    auto obj = create_tl_object<ton_api::overlay_nodeV2>(nullptr, overlay_.tl(), flags_, version_,
                                                         signature_.clone_as_buffer_slice(), std::move(cert));
    source_.visit(td::overloaded([&](const adnl::AdnlNodeIdShort &id) { UNREACHABLE(); },
                                 [&](const adnl::AdnlNodeIdFull &id) { obj->id_ = id.tl(); }));
    return obj;
  }
  OverlayNode clone() const {
    auto res = OverlayNode{source_, overlay_, version_, signature_.clone()};
    if (cert_) {
      res.cert_ = td::make_unique<OverlayMemberCertificate>(*cert_);
    }
    return res;
  }

  const OverlayMemberCertificate *certificate() const {
    if (cert_) {
      return cert_.get();
    }
    return &empty_certificate_;
  }

  void update_certificate(OverlayMemberCertificate cert) {
    if (!cert_ || cert_->empty() || cert_->is_expired() || cert.is_newer(*cert_)) {
      cert_ = td::make_unique<OverlayMemberCertificate>(std::move(cert));
    }
  }

  void update(OverlayNode from) {
    if (version_ < from.version_) {
      source_ = from.source_;
      overlay_ = from.overlay_;
      flags_ = from.flags_;
      version_ = from.version_;
      signature_ = from.signature_.clone();
    }
    if (from.cert_ && !from.cert_->empty()) {
      update_certificate(std::move(*from.cert_));
    }
  }

  void clear_certificate() {
    cert_ = nullptr;
  }

  bool has_full_id() const {
    return source_.get_offset() == source_.offset<adnl::AdnlNodeIdFull>();
  }

 private:
  td::Variant<adnl::AdnlNodeIdFull, adnl::AdnlNodeIdShort> source_;
  OverlayIdShort overlay_;
  td::uint32 flags_;
  td::int32 version_;
  td::unique_ptr<OverlayMemberCertificate> cert_;
  td::SharedSlice signature_;
  static const OverlayMemberCertificate empty_certificate_;
};

}  // namespace overlay

}  // namespace ton
