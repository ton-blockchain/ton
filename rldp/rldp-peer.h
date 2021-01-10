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
#include "fec/fec.h"

namespace ton {

namespace rldp {

class RldpImpl;

using TransferId = td::Bits256;

class RldpTransferSender : public td::actor::Actor {
 public:
  virtual ~RldpTransferSender() = default;

  virtual void confirm(td::uint32 part, td::uint32 seqno) = 0;
  virtual void complete(td::uint32 part) = 0;

  static td::actor::ActorOwn<RldpTransferSender> create(TransferId transfer_id, adnl::AdnlNodeIdShort local_id,
                                                        adnl::AdnlNodeIdShort peer_id, td::BufferSlice data,
                                                        td::Timestamp timeout, td::actor::ActorId<RldpImpl> rldp,
                                                        td::actor::ActorId<adnl::Adnl> adnl);
};

class RldpTransferReceiver : public td::actor::Actor {
 public:
  virtual ~RldpTransferReceiver() = default;

  virtual void receive_part(fec::FecType fec_type, td::uint32 part, td::uint64 total_size, td::uint32 seqno,
                            td::BufferSlice data) = 0;

  static td::actor::ActorOwn<RldpTransferReceiver> create(TransferId transfer_id, adnl::AdnlNodeIdShort local_id,
                                                          adnl::AdnlNodeIdShort peer_id, td::uint64 total_size,
                                                          td::Timestamp timeout, td::actor::ActorId<RldpImpl> rldp,
                                                          td::actor::ActorId<adnl::Adnl> adnl,
                                                          td::Promise<td::BufferSlice> promise);
};

}  // namespace rldp

}  // namespace ton
