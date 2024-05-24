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

#include "adnl/adnl.h"

namespace ton {

namespace rldp {

class Rldp : public adnl::AdnlSenderInterface {
 public:
  virtual ~Rldp() = default;

  virtual void add_id(adnl::AdnlNodeIdShort local_id) = 0;

  virtual void send_message_ex(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::Timestamp timeout,
                               td::BufferSlice data) = 0;

  virtual void set_default_mtu(td::uint64 mtu) = 0;

  static td::actor::ActorOwn<Rldp> create(td::actor::ActorId<adnl::Adnl> adnl);
};

}  // namespace rldp

}  // namespace ton
