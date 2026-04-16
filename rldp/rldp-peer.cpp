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
#include "adnl/utils.hpp"
#include "auto/tl/ton_api.hpp"
#include "common/io.hpp"
#include "td/utils/Random.h"
#include "td/utils/overloaded.h"

#include "rldp-peer.hpp"

namespace ton {

namespace rldp {

void RldpTransferSenderImpl::finish() {
  td::actor::send_closure(rldp_, &RldpImpl::transfer_completed, transfer_id_);
  stop();
}

void RldpTransferSenderImpl::create_encoder() {
  if (part_ * slice_size() >= data_.size()) {
    finish();
    return;
  }
  td::BufferSlice D = data_.clone();
  D.confirm_read(td::narrow_cast<std::size_t>(part_ * slice_size()));
  if (D.size() > slice_size()) {
    D.truncate(td::narrow_cast<std::size_t>(slice_size()));
  }
  fec_type_ = td::fec::RaptorQEncoder::Parameters{D.size(), symbol_size(), 0};
  auto E = fec_type_.create_encoder(std::move(D));
  E.ensure();
  encoder_ = E.move_as_ok();
  seqno_ = 0;
  confirmed_seqno_ = 0;
}

void RldpTransferSenderImpl::start_up() {
  create_encoder();
  alarm();
}

void RldpTransferSenderImpl::alarm() {
  CHECK(confirmed_seqno_ <= seqno_);
  if (timeout_.is_in_past()) {
    finish();
    return;
  }
  alarm_timestamp() = td::Timestamp::in(0.01);
  send_part();
}

void RldpTransferSenderImpl::send_part() {
  for (td::uint32 cnt = 0; cnt < 10; cnt++) {
    if (seqno_ - confirmed_seqno_ <= window_size()) {
      send_one_part(seqno_++);
    } else {
      send_one_part(seqno_);
      break;
    }
  }
}

void RldpTransferSenderImpl::send_one_part(td::uint32 seqno) {
  if (encoder_->get_info().ready_symbol_count <= seqno) {
    encoder_->prepare_more_symbols();
  }
  auto symbol = encoder_->gen_symbol(seqno);
  auto obj = create_tl_object<ton_api::rldp_messagePart>(transfer_id_, fec_type_.tl(), part_, data_.size(), seqno,
                                                         std::move(symbol.data));
  auto serialized = serialize_tl_object(obj, true);
  if (metrics_) {
    metrics_->parts_sent_to_adnl_part.fetch_add(1, std::memory_order_relaxed);
    metrics_->bytes_sent_to_adnl_part.fetch_add(serialized.size(), std::memory_order_relaxed);
  }
  td::actor::send_closure(adnl_, &adnl::Adnl::send_message, local_id_, peer_id_, std::move(serialized));
}

void RldpTransferSenderImpl::confirm(td::uint32 part, td::uint32 seqno) {
  if (part == part_) {
    if (seqno >= confirmed_seqno_ && seqno <= seqno_) {
      confirmed_seqno_ = seqno;
    }
  }
}

void RldpTransferSenderImpl::complete(td::uint32 part) {
  if (part == part_) {
    part_++;
    create_encoder();
  }
}

td::actor::ActorOwn<RldpTransferSender> RldpTransferSender::create(
    TransferId transfer_id, adnl::AdnlNodeIdShort local_id, adnl::AdnlNodeIdShort peer_id, td::BufferSlice data,
    td::Timestamp timeout, td::actor::ActorId<RldpImpl> rldp, td::actor::ActorId<adnl::Adnl> adnl,
    std::shared_ptr<RldpMetrics> metrics) {
  return td::actor::create_actor<RldpTransferSenderImpl>("sender", transfer_id, local_id, peer_id, std::move(data),
                                                         timeout, rldp, adnl, std::move(metrics));
}

void RldpTransferReceiverImpl::receive_part(fec::FecType fec_type, td::uint32 part, td::uint64 total_size,
                                            td::uint32 seqno, td::BufferSlice data) {
  if (part < part_) {
    auto obj = create_tl_object<ton_api::rldp_complete>(transfer_id_, part);
    auto serialized = serialize_tl_object(obj, true);
    if (metrics_) {
      metrics_->parts_sent_to_adnl_complete.fetch_add(1, std::memory_order_relaxed);
      metrics_->bytes_sent_to_adnl_complete.fetch_add(serialized.size(), std::memory_order_relaxed);
    }
    td::actor::send_closure(adnl_, &adnl::Adnl::send_message, local_id_, peer_id_, std::move(serialized));
    return;
  }
  if (part > part_) {
    return;
  }
  cnt_++;

  if (seqno > max_seqno_) {
    max_seqno_ = seqno;
  }

  if (!decoder_) {
    if (offset_ + fec_type.size() > total_size_) {
      VLOG(RLDP_NOTICE) << "failed to create decoder: data size in fec type is too big";
      return;
    }
    auto D = fec_type.create_decoder();
    if (D.is_error()) {
      VLOG(RLDP_WARNING) << "failed to create decoder: " << D.move_as_error();
      return;
    }
    decoder_ = D.move_as_ok();
  }
  decoder_->add_symbol(td::fec::Symbol{seqno, std::move(data)});
  if (decoder_->may_try_decode()) {
    auto D = decoder_->try_decode(false);
    if (D.is_ok()) {
      auto data = D.move_as_ok();
      if (data.data.size() + offset_ > total_size_) {
        abort(td::Status::Error(ErrorCode::protoviolation,
                                PSTRING() << "too big part: offset=" << offset_ << " total_size=" << total_size_
                                          << " data_size=" << data.data.size() << " part=" << part_));
        return;
      }
      offset_ += data.data.size();
      data_parts_.push_back(std::move(data.data));
      auto obj = create_tl_object<ton_api::rldp_complete>(transfer_id_, part_);
      auto serialized = serialize_tl_object(obj, true);
      if (metrics_) {
        metrics_->parts_sent_to_adnl_complete.fetch_add(1, std::memory_order_relaxed);
        metrics_->bytes_sent_to_adnl_complete.fetch_add(serialized.size(), std::memory_order_relaxed);
      }
      td::actor::send_closure(adnl_, &adnl::Adnl::send_message, local_id_, peer_id_, std::move(serialized));
      part_++;
      cnt_ = 0;
      max_seqno_ = 0;
      decoder_ = nullptr;
      if (offset_ == total_size_) {
        finish();
        return;
      }
    }
  }

  if (cnt_ >= 10) {
    auto obj = create_tl_object<ton_api::rldp_confirm>(transfer_id_, part_, max_seqno_);
    auto serialized = serialize_tl_object(obj, true);
    if (metrics_) {
      metrics_->parts_sent_to_adnl_confirm.fetch_add(1, std::memory_order_relaxed);
      metrics_->bytes_sent_to_adnl_confirm.fetch_add(serialized.size(), std::memory_order_relaxed);
    }
    td::actor::send_closure(adnl_, &adnl::Adnl::send_message, local_id_, peer_id_, std::move(serialized));
    cnt_ = 0;
  }
}

void RldpTransferReceiverImpl::abort(td::Status reason) {
  VLOG(RLDP_NOTICE) << "aborted transfer receive: " << reason;
  promise_.set_error(reason.move_as_error_prefix(PSTRING() << "rldptransfer " << transfer_id_ << ": "));
  stop();
}

void RldpTransferReceiverImpl::finish() {
  if (data_parts_.size() == 1) {
    promise_.set_value(data_parts_[0].clone());
  } else {
    td::BufferSlice data(total_size_);
    td::MutableSlice s = data.as_slice();
    for (const auto& part : data_parts_) {
      s.copy_from(part);
      s.remove_prefix(part.size());
    }
    CHECK(s.empty());
    promise_.set_value(std::move(data));
  }
  stop();
}

void RldpTransferReceiverImpl::alarm() {
  abort(td::Status::Error(ErrorCode::timeout, "timeout"));
}

td::actor::ActorOwn<RldpTransferReceiver> RldpTransferReceiver::create(
    TransferId transfer_id, adnl::AdnlNodeIdShort local_id, adnl::AdnlNodeIdShort peer_id, td::uint64 total_size,
    td::Timestamp timeout, td::actor::ActorId<RldpImpl> rldp, td::actor::ActorId<adnl::Adnl> adnl,
    td::Promise<td::BufferSlice> promise, std::shared_ptr<RldpMetrics> metrics) {
  return td::actor::create_actor<RldpTransferReceiverImpl>("receiver", transfer_id, local_id, peer_id, total_size,
                                                           timeout, rldp, adnl, std::move(promise), std::move(metrics));
}

}  // namespace rldp

}  // namespace ton
