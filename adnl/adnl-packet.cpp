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
#include "adnl-packet.h"
#include "td/utils/Random.h"

namespace ton {

namespace adnl {

/*adnl.packetContents rand1:bytes flags:# from:flags.0?PublicKey from_short:flags.1?adnl.id.short
                    message:flags.2?adnl.Message messages:flags.3?(vector adnl.Message)
                    address:flags.6?adnl.addressList seqno:flags.8?long recv_addr_list_version:flags.9?int
                    confirm_seqno:flags.10?long reinit_date:flags.11?int dst_reinit_date:flags.11?int
                    signature:flags.7?bytes rand2:bytes = adnl.PacketContents;*/

td::Result<AdnlPacket> AdnlPacket::create(tl_object_ptr<ton_api::adnl_packetContents> packet) {
  AdnlPacket R;
  R.rand1_ = std::move(packet->rand1_);
  R.flags_ = packet->flags_;
  if (R.flags_ & Flags::f_from) {
    TRY_RESULT(F, AdnlNodeIdFull::create(packet->from_));
    R.from_ = std::move(F);
  }
  if (R.flags_ & Flags::f_from_short) {
    R.from_short_ = AdnlNodeIdShort{packet->from_short_->id_};
  } else if (packet->flags_ & Flags::f_from) {
    R.from_short_ = R.from_.compute_short_id();
  }
  if (R.flags_ & Flags::f_one_message) {
    R.messages_ = AdnlMessageList{std::move(packet->message_)};
  }
  if (R.flags_ & Flags::f_mult_messages) {
    // may override messages_ if (flags & 0x4)
    // but this message will fail in run_basic_checks()
    // so it doesn't matter
    R.messages_ = AdnlMessageList{std::move(packet->messages_)};
  }
  if (R.flags_ & Flags::f_address) {
    TRY_RESULT(addr_list, AdnlAddressList::create(std::move(packet->address_)));
    R.addr_ = std::move(addr_list);
  }
  if (R.flags_ & Flags::f_priority_address) {
    TRY_RESULT(addr_list, AdnlAddressList::create(std::move(packet->address_)));
    R.priority_addr_ = std::move(addr_list);
  }
  if (R.flags_ & Flags::f_seqno) {
    R.seqno_ = packet->seqno_;
  }
  if (R.flags_ & Flags::f_confirm_seqno) {
    R.confirm_seqno_ = packet->confirm_seqno_;
  }
  if (R.flags_ & Flags::f_recv_addr_version) {
    R.recv_addr_list_version_ = packet->recv_addr_list_version_;
  }
  if (R.flags_ & Flags::f_recv_priority_addr_version) {
    R.recv_priority_addr_list_version_ = packet->recv_priority_addr_list_version_;
  }
  if (R.flags_ & Flags::f_reinit_date) {
    R.reinit_date_ = packet->reinit_date_;
    R.dst_reinit_date_ = packet->dst_reinit_date_;
  }
  if (R.flags_ & Flags::f_signature) {
    R.signature_ = std::move(packet->signature_);
  }
  R.rand2_ = std::move(packet->rand2_);

  TRY_STATUS(R.run_basic_checks());
  return std::move(R);
}

td::Status AdnlPacket::run_basic_checks() const {
  if ((flags_ & Flags::f_all) != flags_) {
    return td::Status::Error(ErrorCode::protoviolation, "bad flags");
  }
  if ((flags_ & Flags::f_one_message) && (flags_ & Flags::f_mult_messages)) {
    return td::Status::Error(ErrorCode::protoviolation, "both flags 0x4 and 0x8 set");
  }
  if ((flags_ & Flags::f_from) && (flags_ & Flags::f_from_short) && from_.compute_short_id() != from_short_) {
    return td::Status::Error(ErrorCode::protoviolation, "source and short source mismatch");
  }
  if ((flags_ & Flags::f_address) && addr_.empty()) {
    return td::Status::Error(ErrorCode::protoviolation, "bad addr list");
  }
  if ((flags_ & Flags::f_priority_address) && priority_addr_.empty()) {
    return td::Status::Error(ErrorCode::protoviolation, "bad addr list");
  }
  return td::Status::OK();
}

tl_object_ptr<ton_api::adnl_packetContents> AdnlPacket::tl() const {
  return create_tl_object<ton_api::adnl_packetContents>(
      rand1_.clone(), flags_ & ~Flags::f_priority, (flags_ & Flags::f_from) ? from_.tl() : nullptr,
      (flags_ & Flags::f_from_short) ? from_short_.tl() : nullptr,
      (flags_ & Flags::f_one_message) ? messages_.one_message() : nullptr,
      (flags_ & Flags::f_mult_messages) ? messages_.mult_messages() : messages_.empty_vector(),
      (flags_ & Flags::f_address) ? addr_.tl() : nullptr,
      (flags_ & Flags::f_priority_address) ? priority_addr_.tl() : nullptr, seqno_, confirm_seqno_,
      recv_addr_list_version_, recv_priority_addr_list_version_, reinit_date_, dst_reinit_date_, signature_.clone(),
      rand2_.clone());
}

td::BufferSlice AdnlPacket::to_sign() const {
  auto obj = tl();
  obj->signature_.clear();
  obj->flags_ &= ~Flags::f_signature;
  CHECK(obj->signature_.size() == 0);
  return serialize_tl_object(obj, true);
}

void AdnlPacket::init_random() {
  rand1_ = td::BufferSlice{(td::Random::fast_uint32() & 1) ? 7u : 15u};
  rand2_ = td::BufferSlice{(td::Random::fast_uint32() & 1) ? 7u : 15u};
  td::Random::secure_bytes(rand1_.as_slice());
  td::Random::secure_bytes(rand2_.as_slice());
}

}  // namespace adnl

}  // namespace ton
