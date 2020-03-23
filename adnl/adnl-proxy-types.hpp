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

#include "adnl-proxy-types.h"

#include "common/checksum.h"

namespace ton {

namespace adnl {

class AdnlProxyNone : public AdnlProxy {
 public:
  AdnlProxyNone(td::Bits256 id) : id_(id) {
  }
  td::BufferSlice encrypt(Packet packet) const override {
    td::BufferSlice d{packet.data.size() + 32};
    d.as_slice().copy_from(id_.as_slice());
    d.as_slice().remove_prefix(32).copy_from(packet.data.as_slice());
    return d;
  }
  td::Result<Packet> decrypt(td::BufferSlice packet) const override;
  tl_object_ptr<ton_api::adnl_Proxy> tl() const override {
    return create_tl_object<ton_api::adnl_proxy_none>(id_);
  }
  const td::Bits256 &id() const override {
    return id_;
  }

 private:
  td::Bits256 id_;
};

class AdnlProxyFast : public AdnlProxy {
 public:
  AdnlProxyFast(td::Bits256 id, td::Slice shared_secret)
      : id_(id), shared_secret_(sha256_bits256(shared_secret)), shared_secret_raw_(shared_secret) {
  }
  td::BufferSlice encrypt(Packet packet) const override;
  td::Result<Packet> decrypt(td::BufferSlice packet) const override;
  tl_object_ptr<ton_api::adnl_Proxy> tl() const override {
    return create_tl_object<ton_api::adnl_proxy_fast>(id_, shared_secret_raw_.clone_as_buffer_slice());
  }
  const td::Bits256 &id() const override {
    return id_;
  }

 private:
  td::Bits256 id_;
  td::Bits256 shared_secret_;
  td::SharedSlice shared_secret_raw_;
};

}  // namespace adnl

}  // namespace ton
