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

#include "rldp-peer.h"
#include "fec/fec.h"
#include "rldp.hpp"

#include <map>

namespace ton {

namespace rldp {

class RldpTransferSenderImpl : public RldpTransferSender {
 public:
  static constexpr td::uint64 slice_size() {
    return 2000000;
  }
  static constexpr td::uint32 symbol_size() {
    return 768;
  }
  static constexpr td::uint32 window_size() {
    return 1000;
  }

  void start_up() override;
  void alarm() override;

  void send_part();
  void send_one_part(td::uint32 seqno);
  void confirm(td::uint32 part, td::uint32 seqno) override;
  void complete(td::uint32 part) override;

  RldpTransferSenderImpl(TransferId transfer_id, adnl::AdnlNodeIdShort local_id, adnl::AdnlNodeIdShort peer_id,
                         td::BufferSlice data, td::Timestamp timeout, td::actor::ActorId<RldpImpl> rldp,
                         td::actor::ActorId<adnl::Adnl> adnl)
      : transfer_id_(transfer_id)
      , local_id_(local_id)
      , peer_id_(peer_id)
      , data_(std::move(data))
      , timeout_(timeout)
      , rldp_(rldp)
      , adnl_(adnl) {
  }

 private:
  void create_encoder();
  void finish();

  TransferId transfer_id_;

  adnl::AdnlNodeIdShort local_id_;
  adnl::AdnlNodeIdShort peer_id_;

  td::uint32 seqno_ = 0;
  td::uint32 confirmed_seqno_ = 0;
  std::unique_ptr<td::fec::Encoder> encoder_;
  fec::FecType fec_type_;
  td::BufferSlice data_;
  td::uint32 part_ = 0;

  td::Timestamp timeout_;
  td::actor::ActorId<RldpImpl> rldp_;
  td::actor::ActorId<adnl::Adnl> adnl_;
};

class RldpTransferReceiverImpl : public RldpTransferReceiver {
 public:
  void receive_part(fec::FecType fec_type, td::uint32 part, td::uint64 total_size, td::uint32 seqno,
                    td::BufferSlice data) override;
  void alarm() override;
  void start_up() override {
    data_ = td::BufferSlice(td::narrow_cast<size_t>(total_size_));
  }

  RldpTransferReceiverImpl(TransferId transfer_id, adnl::AdnlNodeIdShort local_id, adnl::AdnlNodeIdShort peer_id,
                           td::uint64 total_size, td::Timestamp timeout, td::actor::ActorId<RldpImpl> rldp,
                           td::actor::ActorId<adnl::Adnl> adnl, td::Promise<td::BufferSlice> promise)
      : transfer_id_(transfer_id)
      , local_id_(local_id)
      , peer_id_(peer_id)
      , total_size_(total_size)
      , timeout_(timeout)
      , rldp_(rldp)
      , adnl_(adnl)
      , promise_(std::move(promise)) {
  }

 private:
  void abort(td::Status reason);
  void finish();

  TransferId transfer_id_;

  adnl::AdnlNodeIdShort local_id_;
  adnl::AdnlNodeIdShort peer_id_;

  td::uint64 total_size_;
  td::uint64 offset_ = 0;
  td::uint32 part_ = 0;
  td::uint32 cnt_ = 0;
  td::uint32 max_seqno_ = 0;
  td::BufferSlice data_;

  std::unique_ptr<td::fec::Decoder> decoder_;

  td::Timestamp timeout_;
  td::actor::ActorId<RldpImpl> rldp_;
  td::actor::ActorId<adnl::Adnl> adnl_;

  td::Promise<td::BufferSlice> promise_;
};

}  // namespace rldp

}  // namespace ton
