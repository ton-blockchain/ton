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
#include "rldp-in.hpp"
#include "auto/tl/ton_api.h"
#include "auto/tl/ton_api.hpp"
#include "td/utils/Random.h"
#include "fec/fec.h"

namespace ton {

namespace rldp {

TransferId RldpIn::transfer(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::Timestamp timeout,
                            td::BufferSlice data, TransferId t) {
  TransferId transfer_id;
  if (!t.is_zero()) {
    transfer_id = t;
  } else {
    td::Random::secure_bytes(transfer_id.as_slice());
  }

  senders_.emplace(transfer_id,
                   RldpTransferSender::create(transfer_id, src, dst, std::move(data), timeout, actor_id(this), adnl_));
  return transfer_id;
}

void RldpIn::send_message(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data) {
  td::Bits256 id;
  td::Random::secure_bytes(id.as_slice());

  auto B = serialize_tl_object(create_tl_object<ton_api::rldp_message>(id, std::move(data)), true);

  transfer(src, dst, td::Timestamp::in(10.0), std::move(B));
}

void RldpIn::send_message_ex(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::Timestamp timeout,
                             td::BufferSlice data) {
  td::Bits256 id;
  td::Random::secure_bytes(id.as_slice());

  auto B = serialize_tl_object(create_tl_object<ton_api::rldp_message>(id, std::move(data)), true);

  transfer(src, dst, timeout, std::move(B));
}

void RldpIn::send_query_ex(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, std::string name,
                           td::Promise<td::BufferSlice> promise, td::Timestamp timeout, td::BufferSlice data,
                           td::uint64 max_answer_size) {
  auto query_id = adnl::AdnlQuery::random_query_id();

  auto date = static_cast<td::uint32>(timeout.at_unix()) + 1;
  auto B = serialize_tl_object(create_tl_object<ton_api::rldp_query>(query_id, max_answer_size, date, std::move(data)),
                               true);

  auto transfer_id = transfer(src, dst, timeout, std::move(B));
  max_size_[transfer_id ^ TransferId::ones()] = max_answer_size;

  auto Q = adnl::AdnlQuery::create(
      std::move(promise),
      [SelfId = actor_id(this), transfer_id](adnl::AdnlQueryId query_id) {
        td::actor::send_closure(SelfId, &RldpIn::alarm_query, query_id, transfer_id);
      },
      name, timeout, query_id);
  queries_.emplace(query_id, std::move(Q));
}

void RldpIn::answer_query(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::Timestamp timeout,
                          adnl::AdnlQueryId query_id, TransferId transfer_id, td::BufferSlice data) {
  auto B = serialize_tl_object(create_tl_object<ton_api::rldp_answer>(query_id, std::move(data)), true);

  transfer(src, dst, timeout, std::move(B), transfer_id);
}

void RldpIn::alarm_query(adnl::AdnlQueryId query_id, TransferId transfer_id) {
  queries_.erase(query_id);
  max_size_.erase(transfer_id);
}

void RldpIn::receive_message_part(adnl::AdnlNodeIdShort source, adnl::AdnlNodeIdShort local_id, td::BufferSlice data) {
  auto F = fetch_tl_object<ton_api::rldp_MessagePart>(std::move(data), true);
  if (F.is_error()) {
    VLOG(RLDP_INFO) << "failed to parse rldp packet [" << source << "->" << local_id << "]: " << F.move_as_error();
    return;
  }

  ton_api::downcast_call(*F.move_as_ok().get(), [&](auto &obj) { this->process_message_part(source, local_id, obj); });
}

void RldpIn::process_message_part(adnl::AdnlNodeIdShort source, adnl::AdnlNodeIdShort local_id,
                                  ton_api::rldp_messagePart &part) {
  auto it = receivers_.find(part.transfer_id_);
  if (it == receivers_.end()) {
    if (part.part_ != 0) {
      VLOG(RLDP_INFO) << "dropping new part";
      return;
    }
    if (static_cast<td::uint64>(part.total_size_) > mtu()) {
      VLOG(RLDP_NOTICE) << "dropping too big rldp packet of size=" << part.total_size_ << " mtu=" << mtu();
      return;
    }
    auto ite = max_size_.find(part.transfer_id_);
    if (ite == max_size_.end()) {
      if (static_cast<td::uint64>(part.total_size_) > default_mtu()) {
        VLOG(RLDP_NOTICE) << "dropping too big rldp packet of size=" << part.total_size_
                          << " default_mtu=" << default_mtu();
        return;
      }
    } else {
      if (static_cast<td::uint64>(part.total_size_) > ite->second) {
        VLOG(RLDP_NOTICE) << "dropping too big rldp packet of size=" << part.total_size_ << " allowed=" << ite->second;
        return;
      }
    }
    if (lru_set_.count(part.transfer_id_) == 1) {
      auto obj = create_tl_object<ton_api::rldp_complete>(part.transfer_id_, part.part_);
      td::actor::send_closure(adnl_, &adnl::Adnl::send_message, local_id, source, serialize_tl_object(obj, true));
      return;
    }
    auto P = td::PromiseCreator::lambda(
        [SelfId = actor_id(this), source, local_id, transfer_id = part.transfer_id_](td::Result<td::BufferSlice> R) {
          if (R.is_error()) {
            VLOG(RLDP_INFO) << "failed to receive: " << R.move_as_error();
            return;
          }
          td::actor::send_closure(SelfId, &RldpIn::in_transfer_completed, transfer_id);
          td::actor::send_closure(SelfId, &RldpIn::receive_message, source, local_id, transfer_id, R.move_as_ok());
        });

    receivers_.emplace(part.transfer_id_,
                       RldpTransferReceiver::create(part.transfer_id_, local_id, source, part.total_size_,
                                                    td::Timestamp::in(60.0), actor_id(this), adnl_, std::move(P)));
    it = receivers_.find(part.transfer_id_);
  }
  auto F = fec::FecType::create(std::move(part.fec_type_));
  if (F.is_ok()) {
    td::actor::send_closure(it->second, &RldpTransferReceiver::receive_part, F.move_as_ok(), part.part_,
                            part.total_size_, part.seqno_, std::move(part.data_));
  } else {
    VLOG(RLDP_NOTICE) << "received bad fec type: " << F.move_as_error();
  }
}

void RldpIn::process_message_part(adnl::AdnlNodeIdShort source, adnl::AdnlNodeIdShort local_id,
                                  ton_api::rldp_confirm &part) {
  auto it = senders_.find(part.transfer_id_);
  if (it != senders_.end()) {
    td::actor::send_closure(it->second, &RldpTransferSender::confirm, part.part_, part.seqno_);
  }
}

void RldpIn::process_message_part(adnl::AdnlNodeIdShort source, adnl::AdnlNodeIdShort local_id,
                                  ton_api::rldp_complete &part) {
  auto it = senders_.find(part.transfer_id_);
  if (it != senders_.end()) {
    td::actor::send_closure(it->second, &RldpTransferSender::complete, part.part_);
  }
}

void RldpIn::receive_message(adnl::AdnlNodeIdShort source, adnl::AdnlNodeIdShort local_id, TransferId transfer_id,
                             td::BufferSlice data) {
  auto F = fetch_tl_object<ton_api::rldp_Message>(std::move(data), true);
  if (F.is_error()) {
    VLOG(RLDP_INFO) << "failed to parse rldp packet [" << source << "->" << local_id << "]: " << F.move_as_error();
    return;
  }

  ton_api::downcast_call(*F.move_as_ok().get(),
                         [&](auto &obj) { this->process_message(source, local_id, transfer_id, obj); });
}

void RldpIn::process_message(adnl::AdnlNodeIdShort source, adnl::AdnlNodeIdShort local_id, TransferId transfer_id,
                             ton_api::rldp_message &message) {
  td::actor::send_closure(adnl_, &adnl::AdnlPeerTable::deliver, source, local_id, std::move(message.data_));
}

void RldpIn::process_message(adnl::AdnlNodeIdShort source, adnl::AdnlNodeIdShort local_id, TransferId transfer_id,
                             ton_api::rldp_query &message) {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), source, local_id,
                                       timeout = td::Timestamp::at_unix(message.timeout_), query_id = message.query_id_,
                                       max_answer_size = static_cast<td::uint64>(message.max_answer_size_),
                                       transfer_id](td::Result<td::BufferSlice> R) {
    if (R.is_ok()) {
      auto data = R.move_as_ok();
      if (data.size() > max_answer_size) {
        VLOG(RLDP_NOTICE) << "rldp query failed: answer too big";
      } else {
        td::actor::send_closure(SelfId, &RldpIn::answer_query, local_id, source, timeout, query_id,
                                transfer_id ^ TransferId::ones(), std::move(data));
      }
    } else {
      VLOG(RLDP_NOTICE) << "rldp query failed: " << R.move_as_error();
    }
  });
  VLOG(RLDP_DEBUG) << "delivering rldp query";
  td::actor::send_closure(adnl_, &adnl::AdnlPeerTable::deliver_query, source, local_id, std::move(message.data_),
                          std::move(P));
}

void RldpIn::process_message(adnl::AdnlNodeIdShort source, adnl::AdnlNodeIdShort local_id, TransferId transfer_id,
                             ton_api::rldp_answer &message) {
  auto it = queries_.find(message.query_id_);
  if (it != queries_.end()) {
    td::actor::send_closure(it->second, &adnl::AdnlQuery::result, std::move(message.data_));
    queries_.erase(it);
  } else {
    VLOG(RLDP_INFO) << "received answer to unknown query " << message.query_id_;
  }
}

void RldpIn::transfer_completed(TransferId transfer_id) {
  senders_.erase(transfer_id);
  VLOG(RLDP_DEBUG) << "rldp: completed transfer " << transfer_id << "; " << senders_.size() << " out transfer pending ";
}

void RldpIn::in_transfer_completed(TransferId transfer_id) {
  if (lru_set_.count(transfer_id) == 1) {
    return;
  }
  while (lru_size_ >= lru_size()) {
    auto N = RldpLru::from_list_node(lru_.get());
    lru_set_.erase(N->transfer_id());
    lru_size_--;
    delete N;
  }
  lru_set_.insert(transfer_id);
  auto N = new RldpLru{transfer_id};
  lru_.put(N);
  lru_size_++;
}

void RldpIn::add_id(adnl::AdnlNodeIdShort local_id) {
  if (local_ids_.count(local_id) == 1) {
    return;
  }

  std::vector<std::string> X{adnl::Adnl::int_to_bytestring(ton_api::rldp_messagePart::ID),
                             adnl::Adnl::int_to_bytestring(ton_api::rldp_confirm::ID),
                             adnl::Adnl::int_to_bytestring(ton_api::rldp_complete::ID)};
  for (auto &x : X) {
    td::actor::send_closure(adnl_, &adnl::Adnl::subscribe, local_id, x, make_adnl_callback());
  }

  local_ids_.insert(local_id);
}

void RldpIn::get_conn_ip_str(adnl::AdnlNodeIdShort l_id, adnl::AdnlNodeIdShort p_id, td::Promise<td::string> promise) {  
  td::actor::send_closure(adnl_, &adnl::AdnlPeerTable::get_conn_ip_str, l_id, p_id, std::move(promise));
}

std::unique_ptr<adnl::Adnl::Callback> RldpIn::make_adnl_callback() {
  class Callback : public adnl::Adnl::Callback {
   private:
    td::actor::ActorId<RldpIn> id_;

   public:
    Callback(td::actor::ActorId<RldpIn> id) : id_(id) {
    }
    void receive_message(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data) override {
      td::actor::send_closure(id_, &RldpIn::receive_message_part, src, dst, std::move(data));
    }
    void receive_query(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data,
                       td::Promise<td::BufferSlice> promise) override {
      promise.set_error(td::Status::Error(ErrorCode::notready, "rldp does not support queries"));
    }
  };

  return std::make_unique<Callback>(actor_id(this));
}

td::actor::ActorOwn<Rldp> Rldp::create(td::actor::ActorId<adnl::Adnl> adnl) {
  return td::actor::create_actor<RldpIn>("rldp", td::actor::actor_dynamic_cast<adnl::AdnlPeerTable>(adnl));
}

}  // namespace rldp

}  // namespace ton
