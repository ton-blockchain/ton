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
#include "overlay-fec.hpp"
#include "overlay.hpp"
#include "adnl/utils.hpp"

namespace ton {

namespace overlay {

void OverlayOutboundFecBroadcast::alarm() {
  for (td::uint32 i = 0; i < 4; i++) {
    auto X = encoder_->gen_symbol(seqno_++);
    CHECK(X.data.size() <= 1000);
    td::actor::send_closure(overlay_, &OverlayImpl::send_new_fec_broadcast_part, local_id_, data_hash_,
                            fec_type_.size(), flags_, std::move(X.data), X.id, fec_type_, date_);
  }

  alarm_timestamp() = td::Timestamp::in(0.010);

  if (seqno_ >= to_send_) {
    stop();
  }
}

void OverlayOutboundFecBroadcast::start_up() {
  encoder_->prepare_more_symbols();
  alarm();
}

OverlayOutboundFecBroadcast::OverlayOutboundFecBroadcast(td::BufferSlice data, td::uint32 flags,
                                                         td::actor::ActorId<OverlayImpl> overlay,
                                                         PublicKeyHash local_id)
    : flags_(flags) {
  CHECK(data.size() <= (1 << 27));
  local_id_ = local_id;
  overlay_ = std::move(overlay);
  date_ = static_cast<td::int32>(td::Clocks::system());
  to_send_ = (static_cast<td::uint32>(data.size()) / symbol_size_ + 1) * 2;

  data_hash_ = td::sha256_bits256(data);

  fec_type_ = td::fec::RaptorQEncoder::Parameters{data.size(), symbol_size_, 0};
  auto E = fec_type_.create_encoder(std::move(data));
  E.ensure();
  encoder_ = E.move_as_ok();
}

td::actor::ActorId<OverlayOutboundFecBroadcast> OverlayOutboundFecBroadcast::create(
    td::BufferSlice data, td::uint32 flags, td::actor::ActorId<OverlayImpl> overlay, PublicKeyHash local_id) {
  return td::actor::create_actor<OverlayOutboundFecBroadcast>(td::actor::ActorOptions().with_name("bcast"),
                                                              std::move(data), flags, overlay, local_id)
      .release();
}

}  // namespace overlay

}  // namespace ton
