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
#include "adnl/adnl-message.h"

namespace ton {

namespace adnl {

/*
  from:flags.0?PublicKey 
  from_short:flags.1?adnl.id.short
  message:flags.2?adnl.Message 
  messages:flags.3?(vector adnl.Message)
  address:flags.4?adnl.addressList 
  priority_address:flags.5?adnl.addressList
  seqno:flags.6?long 
  confirm_seqno:flags.7?long 
  recv_addr_list_version:flags.8?int
  recv_priority_addr_list_version:flags.9?int
  reinit_date:flags.10?int 
  dst_reinit_date:flags.10?int
  signature:flags.11?bytes 
  */

// total packet length:
//   for full packet:
//     32 (dst) + 64 (encryption overhead) + 4 (magic) + 36 (pubkey) + 4 + M (sum of messages) +
//              + A1 + A2 + 8 + 8 + 4 + 4 + 4 + 4 + 68 (signature) + 16 (r1) + 16 (r2) =
//              = 272 + M + A1 + A2
//   for channel:
//     32 (channel id) + 32 (encryption overhead) + 4 (magic) + 4 + M (sum of messages) +
//              + A1 + A2 + 8 + 8 + 4 + 4 + 16(r1) + 16(r2) = 128 + M + A1 + A2

class AdnlPacket {
 private:
  enum Flags : td::uint32 {
    f_from = 0x1,
    f_from_short = 0x2,
    f_one_message = 0x4,
    f_mult_messages = 0x8,
    f_address = 0x10,
    f_priority_address = 0x20,
    f_seqno = 0x40,
    f_confirm_seqno = 0x80,
    f_recv_addr_version = 0x100,
    f_recv_priority_addr_version = 0x200,
    f_reinit_date = 0x400,
    f_signature = 0x800,
    f_priority = 0x1000,
    f_all = 0x1fff
  };

 public:
  AdnlPacket() {
  }
  static td::Result<AdnlPacket> create(tl_object_ptr<ton_api::adnl_packetContents> packet);
  tl_object_ptr<ton_api::adnl_packetContents> tl() const;
  td::BufferSlice to_sign() const;

  td::Status run_basic_checks() const;

  auto flags() const {
    return flags_;
  }
  bool priority() const {
    return flags_ & f_priority;
  }
  bool inited_from_short() const {
    return flags_ & (Flags::f_from | Flags::f_from_short);
  }
  bool inited_from() const {
    return flags_ & Flags::f_from;
  }
  auto from() const {
    return from_;
  }
  auto from_short() const {
    return from_short_;
  }
  const auto &messages() const {
    return messages_;
  }
  auto &messages() {
    return messages_;
  }
  bool inited_addr_list() const {
    return flags_ & Flags::f_address;
  }
  auto addr_list() const {
    return addr_;
  }
  auto priority_addr_list() const {
    return priority_addr_;
  }
  auto seqno() const {
    return seqno_;
  }
  auto confirm_seqno() const {
    return confirm_seqno_;
  }
  auto recv_addr_list_version() const {
    return recv_addr_list_version_;
  }
  auto recv_priority_addr_list_version() const {
    return recv_priority_addr_list_version_;
  }
  auto reinit_date() const {
    return reinit_date_;
  }
  auto dst_reinit_date() const {
    return dst_reinit_date_;
  }
  auto signature() const {
    return signature_.clone();
  }
  auto remote_addr() const {
    return remote_addr_;
  }

  void init_random();

  void set_signature(td::BufferSlice signature) {
    signature_ = std::move(signature);
    flags_ |= Flags::f_signature;
  }
  void set_source(AdnlNodeIdFull src) {
    from_ = src;
    from_short_ = src.compute_short_id();
    flags_ = (flags_ | Flags::f_from) & ~Flags::f_from_short;
  }
  void set_source(AdnlNodeIdShort src) {
    if (!(flags_ & Flags::f_from)) {
      from_short_ = src;
      flags_ |= Flags::f_from_short;
    }
  }
  void add_message(AdnlMessage message) {
    messages_.push_back(std::move(message));
    if (messages_.size() == 1) {
      flags_ = (flags_ | Flags::f_one_message) & ~Flags::f_mult_messages;
    } else {
      flags_ = (flags_ | Flags::f_mult_messages) & ~Flags::f_one_message;
    }
  }
  void set_addr_list(AdnlAddressList addr_list) {
    addr_ = std::move(addr_list);
    flags_ |= Flags::f_address;
  }
  void set_priority_addr_list(AdnlAddressList addr_list) {
    priority_addr_ = std::move(addr_list);
    flags_ |= Flags::f_priority_address;
  }
  void set_seqno(td::uint64 seqno) {
    seqno_ = seqno;
    flags_ |= Flags::f_seqno;
  }
  void set_confirm_seqno(td::uint64 seqno) {
    confirm_seqno_ = seqno;
    flags_ |= Flags::f_confirm_seqno;
  }
  void set_received_addr_list_version(td::int32 version) {
    recv_addr_list_version_ = version;
    flags_ |= Flags::f_recv_addr_version;
  }
  void set_received_priority_addr_list_version(td::int32 version) {
    recv_priority_addr_list_version_ = version;
    flags_ |= Flags::f_recv_priority_addr_version;
  }
  void set_reinit_date(td::int32 date, td::int32 dst_reinit_date) {
    reinit_date_ = date;
    dst_reinit_date_ = dst_reinit_date;
    flags_ |= Flags::f_reinit_date;
  }

  void set_remote_addr(td::IPAddress addr) {
    remote_addr_ = addr;
  }

 private:
  td::BufferSlice rand1_;
  td::uint32 flags_{0};
  AdnlNodeIdFull from_;
  AdnlNodeIdShort from_short_;
  AdnlMessageList messages_;
  AdnlAddressList addr_;
  AdnlAddressList priority_addr_;
  td::uint64 seqno_{0};
  td::uint64 confirm_seqno_{0};
  td::int32 recv_addr_list_version_{0};
  td::int32 recv_priority_addr_list_version_{0};
  td::int32 reinit_date_{0};
  td::int32 dst_reinit_date_{0};
  td::BufferSlice signature_;
  td::BufferSlice rand2_;

  td::IPAddress remote_addr_;
};

}  // namespace adnl

}  // namespace ton
