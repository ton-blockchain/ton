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

#include "overlay-manager.h"
#include "fec/fec.h"
#include "overlay.h"

namespace ton {

namespace overlay {

class OverlayImpl;

class OverlayOutboundFecBroadcast : public td::actor::Actor {
 private:
  const td::uint32 symbol_size_ = 768;
  td::uint32 to_send_;

  td::uint32 seqno_ = 0;
  PublicKeyHash local_id_;
  Overlay::BroadcastDataHash data_hash_;
  td::uint32 flags_ = 0;
  td::int32 date_;
  std::unique_ptr<td::fec::Encoder> encoder_;
  td::actor::ActorId<OverlayImpl> overlay_;
  fec::FecType fec_type_;

 public:
  static td::actor::ActorId<OverlayOutboundFecBroadcast> create(td::BufferSlice data, td::uint32 flags,
                                                                td::actor::ActorId<OverlayImpl> overlay,
                                                                PublicKeyHash local_id);
  OverlayOutboundFecBroadcast(td::BufferSlice data, td::uint32 flags, td::actor::ActorId<OverlayImpl> overlay,
                              PublicKeyHash local_id);

  void alarm() override;
  void start_up() override;
};

}  // namespace overlay

}  // namespace ton
