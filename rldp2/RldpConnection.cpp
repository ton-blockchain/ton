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

#include "RldpConnection.h"
#include "rldp.hpp"

#include "td/utils/overloaded.h"
#include "td/utils/Random.h"
#include "td/utils/tl_helpers.h"

#include "tl-utils/tl-utils.hpp"
#include "auto/tl/ton_api.h"
#include "auto/tl/ton_api.hpp"

#include "common/errorcode.h"

#include "td/actor//actor.h"

namespace ton {
namespace rldp2 {
void RldpConnection::add_limit(td::Timestamp timeout, Limit limit) {
  CHECK(timeout);
  auto p = limits_set_.insert(limit);
  LOG_CHECK(p.second) << limit.transfer_id.to_hex();
  limits_heap_.insert(timeout.at(), const_cast<Limit *>(&*p.first));
}

td::Timestamp RldpConnection::next_limit_expires_at() {
  if (limits_heap_.empty()) {
    return td::Timestamp::never();
  }
  return td::Timestamp::at(limits_heap_.top_key());
}

void RldpConnection::drop_limits(TransferId id) {
  Limit limit;
  limit.transfer_id = id;
  auto it = limits_set_.find(limit);
  if (it == limits_set_.end()) {
    return;
  }
  limits_heap_.erase(const_cast<td::HeapNode *>(static_cast<const td::HeapNode *>(&*it)));
  limits_set_.erase(it);
}

void RldpConnection::on_inbound_completed(TransferId transfer_id, td::Timestamp now) {
  inbound_transfers_.erase(transfer_id);
  completed_set_.insert(transfer_id);
  completed_queue_.push(CompletedId{transfer_id, now.in(20)});
  while (completed_queue_.size() > 128 && completed_queue_.front().timeout.is_in_past(now)) {
    completed_set_.erase(completed_queue_.pop().transfer_id);
  }
}

td::Timestamp RldpConnection::loop_limits(td::Timestamp now) {
  while (!limits_heap_.empty() && td::Timestamp::at(limits_heap_.top_key()).is_in_past(now)) {
    auto *limit = static_cast<Limit *>(limits_heap_.pop());
    auto error = td::Status::Error(ErrorCode::timeout, "timeout");
    if (limit->is_inbound) {
      on_inbound_completed(limit->transfer_id, now);
      to_receive_.emplace_back(limit->transfer_id, std::move(error));
    } else {
      auto it = outbound_transfers_.find(limit->transfer_id);
      if (it != outbound_transfers_.end()) {
        for (auto &part : it->second.parts(RldpSender::Config{})) {
          in_flight_count_ -= part.second.sender.get_inflight_symbols_count();
        }
        outbound_transfers_.erase(it);
        to_on_sent_.emplace_back(limit->transfer_id, std::move(error));
      } else {
        VLOG(RLDP_WARNING) << "Timeout on unknown transfer " << limit->transfer_id.to_hex();
      }
    }
    limits_set_.erase(*limit);
  }

  return next_limit_expires_at();
}

void RldpConnection::set_receive_limits(TransferId transfer_id, td::Timestamp timeout, td::uint64 max_size) {
  CHECK(timeout);
  Limit limit;
  limit.transfer_id = transfer_id;
  limit.max_size = max_size;
  limit.is_inbound = true;
  add_limit(timeout, limit);
}

RldpConnection::RldpConnection() {
  bdw_stats_.on_update(td::Timestamp::now(), 0);

  rtt_stats_.windowed_min_rtt = 0.5;
  bdw_stats_.windowed_max_bdw = 10;
}

void RldpConnection::send(TransferId transfer_id, td::BufferSlice data, td::Timestamp timeout) {
  if (transfer_id.is_zero()) {
    td::Random::secure_bytes(transfer_id.as_slice());
  } else {
    if (outbound_transfers_.find(transfer_id) != outbound_transfers_.end()) {
      VLOG(RLDP_WARNING) << "Skip resend of " << transfer_id.to_hex();
      return;
    }
  }

  if (timeout) {
    Limit limit;
    limit.transfer_id = transfer_id;
    limit.max_size = 0;
    limit.is_inbound = false;
    add_limit(timeout, limit);
  }
  outbound_transfers_.emplace(transfer_id, OutboundTransfer{std::move(data)});
}

void RldpConnection::receive_raw(td::BufferSlice packet) {
  auto F = ton::fetch_tl_object<ton::ton_api::rldp2_MessagePart>(std::move(packet), true);
  if (F.is_error()) {
    return;
  }
  downcast_call(*F.move_as_ok(), [&](auto &obj) { this->receive_raw_obj(obj); });
}

void RldpConnection::loop_bbr(td::Timestamp now) {
  bbr_.step(rtt_stats_, bdw_stats_, in_flight_count_, td::Timestamp::now());
  //LOG(ERROR) << td::format::as_time(rtt_stats_.windowed_min_rtt) << " "
  //<< td::format::as_size((td::int64)bdw_stats_.windowed_max_bdw * 768) << " " << rtt_stats_.rtt_round;
  double speed = bbr_.get_rate();
  td::uint32 congestion_window = bbr_.get_window_size();

  pacer_.set_speed(speed);
  congestion_window_ = congestion_window;
}

td::Timestamp RldpConnection::run(ConnectionCallback &callback) {
  auto now = td::Timestamp::now();
  loop_bbr(now);

  td::Timestamp alarm_timestamp;
  td::VectorQueue<std::pair<const TransferId, OutboundTransfer> *> queue;
  for (auto &outbound : outbound_transfers_) {
    queue.push(&outbound);
  }
  while (!queue.empty()) {
    auto outbound = queue.pop();
    auto o_timeout = step(outbound->first, outbound->second, now);
    if (o_timeout) {
      alarm_timestamp.relax(o_timeout.unwrap());
    } else {
      queue.push(outbound);
    }
  }

  if (in_flight_count_ > congestion_window_) {
    bdw_stats_.on_pause(now);
  }

  for (auto &inbound : inbound_transfers_) {
    alarm_timestamp.relax(run(inbound.first, inbound.second));
  }

  alarm_timestamp.relax(loop_limits(td::Timestamp::now()));

  for (auto &data : to_receive_) {
    callback.receive(data.first, std::move(data.second));
  }
  for (auto &raw : to_send_raw_) {
    callback.send_raw(std::move(raw));
  }
  to_send_raw_.clear();
  to_receive_.clear();
  for (auto &res : to_on_sent_) {
    callback.on_sent(res.first, std::move(res.second));
  }
  to_on_sent_.clear();

  return alarm_timestamp;
}

td::Timestamp RldpConnection::run(const TransferId &transfer_id, InboundTransfer &inbound) {
  td::Timestamp wakeup_at;
  bool has_actions = true;
  while (has_actions) {
    has_actions = false;
    for (auto &it : inbound.parts()) {
      auto &inbound = it.second;
      inbound.receiver.next_action(td::Timestamp::now())
          .visit(td::overloaded([&](const RldpReceiver::ActionWait &wait) { wakeup_at.relax(wait.wait_till); },
                                [&](const RldpReceiver::ActionSendAck &send) {
                                  send_packet(ton::create_serialize_tl_object<ton::ton_api::rldp2_confirm>(
                                      transfer_id, it.first, send.ack.max_seqno, send.ack.received_mask,
                                      send.ack.received_count));
                                  inbound.receiver.on_ack_sent(td::Timestamp::now());
                                  has_actions = true;
                                }));
    }
  }
  return wakeup_at;
}

td::optional<td::Timestamp> RldpConnection::step(const TransferId &transfer_id, OutboundTransfer &outbound,
                                                 td::Timestamp now) {
  bool only_probe = in_flight_count_ > congestion_window_;

  td::Timestamp wakeup_at;
  if (!pacer_.wakeup_at().is_in_past(now)) {
    wakeup_at = pacer_.wakeup_at();
    only_probe = true;
  }

  for (auto &it : outbound.parts(RldpSender::Config{})) {
    auto &part = it.second;

    Guard guard(in_flight_count_, part.sender);
    auto action = part.sender.next_action(now, only_probe);

    bool was_send = false;
    action.visit(td::overloaded(
        [&](const RldpSender::ActionSend &send) {
          auto seqno = send.seqno - 1;
          if (part.encoder->get_info().ready_symbol_count <= seqno) {
            part.encoder->prepare_more_symbols();
          }
          auto symbol = part.encoder->gen_symbol(seqno).data;
          send_packet(ton::create_serialize_tl_object<ton::ton_api::rldp2_messagePart>(
              transfer_id, part.fec_type.tl(), it.first, outbound.total_size(), seqno, std::move(symbol)));
          if (!send.is_probe) {
            pacer_.send(1, now);
          }
          part.sender.on_send(send.seqno, now, send.is_probe, rtt_stats_, bdw_stats_);
          if (send.is_probe) {
            //LOG(ERROR) << "PROBE " << it.first << " " << send.seqno;
          }
          //LOG(ERROR) << "SEND";
          was_send = true;
        },
        [&](const RldpSender::ActionWait &wait) {
          //LOG(ERROR) << "WAIT";
          wakeup_at.relax(wait.wait_till);
        }));
    if (was_send) {
      return {};
    }
  }

  return wakeup_at;
}

void RldpConnection::receive_raw_obj(ton::ton_api::rldp2_messagePart &part) {
  if (completed_set_.count(part.transfer_id_) > 0) {
    send_packet(ton::create_serialize_tl_object<ton::ton_api::rldp2_complete>(part.transfer_id_, part.part_));
    return;
  }

  auto r_total_size = td::narrow_cast_safe<std::size_t>(part.total_size_);
  if (r_total_size.is_error()) {
    return;
  }
  auto r_fec_type = ton::fec::FecType::create(std::move(part.fec_type_));
  if (r_fec_type.is_error()) {
    return;
  }

  auto total_size = r_total_size.move_as_ok();

  auto transfer_id = part.transfer_id_;

  // check total_size limits
  td::uint64 max_size = default_mtu();
  Limit key;
  key.transfer_id = transfer_id;
  auto limit_it = limits_set_.find(key);
  bool has_limit = limit_it != limits_set_.end();
  if (has_limit && limit_it->max_size != 0) {
    max_size = limit_it->max_size;
  }
  if (total_size > max_size) {
    VLOG(RLDP_INFO) << "Drop too big rldp query " << part.total_size_ << " > " << max_size;
    return;
  }

  auto it = inbound_transfers_.find(transfer_id);
  if (it == inbound_transfers_.end()) {
    if (!has_limit) {
      // set timeout even for small inbound queries
      // TODO: other party stil may ddos us with small transfers
      set_receive_limits(transfer_id, td::Timestamp::in(10), max_size);
    }
    it = inbound_transfers_.emplace(transfer_id, InboundTransfer{total_size}).first;
  }

  auto &inbound = it->second;
  auto o_res = [&]() -> td::optional<td::Result<td::BufferSlice>> {
    TRY_RESULT(in_part, inbound.get_part(part.part_, r_fec_type.move_as_ok()));
    if (!in_part) {
      if (inbound.is_part_completed(part.part_)) {
        send_packet(ton::create_serialize_tl_object<ton::ton_api::rldp2_complete>(transfer_id, part.part_));
      }
      return {};
    }
    if (in_part->receiver.on_received(part.seqno_ + 1, td::Timestamp::now())) {
      TRY_STATUS_PREFIX(in_part->decoder->add_symbol({static_cast<td::uint32>(part.seqno_), std::move(part.data_)}),
                        td::Status::Error(ErrorCode::protoviolation, "invalid symbol"));
      if (in_part->decoder->may_try_decode()) {
        auto r_data = in_part->decoder->try_decode(false);
        if (r_data.is_ok()) {
          inbound.finish_part(part.part_, r_data.move_as_ok().data);
        }
      }
    }
    return inbound.try_finish();
  }();

  if (o_res) {
    drop_limits(transfer_id);
    on_inbound_completed(transfer_id, td::Timestamp::now());
    to_receive_.emplace_back(transfer_id, o_res.unwrap());
  }
}

void RldpConnection::receive_raw_obj(ton::ton_api::rldp2_complete &complete) {
  auto transfer_id = complete.transfer_id_;
  auto it = outbound_transfers_.find(transfer_id);
  if (it == outbound_transfers_.end()) {
    return;
  }

  auto *part = it->second.get_part(complete.part_);
  if (part) {
    in_flight_count_ -= part->sender.get_inflight_symbols_count();
    it->second.drop_part(complete.part_);
  }

  if (it->second.is_done()) {
    drop_limits(it->first);
    to_on_sent_.emplace_back(it->first, td::Unit());
    outbound_transfers_.erase(it);
  }
}

void RldpConnection::receive_raw_obj(ton::ton_api::rldp2_confirm &confirm) {
  auto transfer_id = confirm.transfer_id_;
  auto it = outbound_transfers_.find(transfer_id);
  if (it == outbound_transfers_.end()) {
    return;
  }
  auto *part = it->second.get_part(confirm.part_);
  if (!part) {
    return;
  }
  Guard guard(in_flight_count_, part->sender);
  Ack ack;
  ack.max_seqno = confirm.max_seqno_;
  ack.received_count = confirm.received_count_;
  ack.received_mask = confirm.received_mask_;
  auto update = part->sender.on_ack(ack, 0, td::Timestamp::now(), rtt_stats_, bdw_stats_, loss_stats_);
  // update.new_received event
  // update.o_loss_at event
}

}  // namespace rldp2
}  // namespace ton
