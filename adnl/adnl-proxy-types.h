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

#include "td/utils/buffer.h"
#include "auto/tl/ton_api.h"

namespace ton {

namespace adnl {

class AdnlProxy {
 public:
  struct Packet {
    td::uint32 flags;
    td::uint32 ip;
    td::uint16 port;
    td::int32 adnl_start_time;
    td::int64 seqno;
    td::int32 date{0};
    td::BufferSlice data;
  };
  virtual ~AdnlProxy() = default;
  virtual td::BufferSlice encrypt(Packet packet) const = 0;
  virtual td::Result<Packet> decrypt(td::BufferSlice packet) const = 0;
  virtual tl_object_ptr<ton_api::adnl_Proxy> tl() const = 0;
  virtual const td::Bits256 &id() const = 0;

  static td::Result<std::shared_ptr<AdnlProxy>> create(const ton_api::adnl_Proxy &proxy_type);
};

}  // namespace adnl

}  // namespace ton
