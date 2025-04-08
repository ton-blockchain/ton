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
#include "adnl-peer.h"
#include "adnl-peer.hpp"
#include "adnl-local-id.h"

#include "utils.hpp"

#include "td/actor/PromiseFuture.h"
#include "td/utils/base64.h"
#include "td/utils/Random.h"
#include "auto/tl/ton_api.h"
#include "td/utils/overloaded.h"

namespace ton {

namespace adnl {

static_assert(AdnlPeerPairImpl::get_mtu() + AdnlPeerPairImpl::packet_header_max_size() <= AdnlNetworkManager::get_mtu(),
              "wrong mtu configuration");

void AdnlPeerPairImpl::start_up() {
  auto P1 = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<AdnlDbItem> R) {
    td::actor::send_closure(SelfId, &AdnlPeerPairImpl::got_data_from_db, std::move(R));
  });
  td::actor::send_closure(peer_table_, &AdnlPeerTable::get_addr_list_from_db, local_id_, peer_id_short_, std::move(P1));
  auto P2 = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<AdnlNode> R) {
    td::actor::send_closure(SelfId, &AdnlPeerPairImpl::got_data_from_static_nodes, std::move(R));
  });
  td::actor::send_closure(peer_table_, &AdnlPeerTable::get_static_node, peer_id_short_, std::move(P2));

  if (!dht_node_.empty()) {
    discover();
  }
}

void AdnlPeerPairImpl::alarm() {
  if (!disable_dht_query_) {
    disable_dht_query_ = true;
    if (next_dht_query_at_ && next_dht_query_at_.is_in_past()) {
      next_dht_query_at_ = td::Timestamp::never();
      discover();
    }
    alarm_timestamp().relax(next_dht_query_at_);
  }
  if (next_db_update_at_ && next_db_update_at_.is_in_past()) {
    if (received_from_db_ && received_from_static_nodes_ && !peer_id_.empty()) {
      AdnlDbItem item;
      item.id = peer_id_;
      item.addr_list = addr_list_;
      item.priority_addr_list = priority_addr_list_;

      td::actor::send_closure(peer_table_, &AdnlPeerTable::write_new_addr_list_to_db, local_id_, peer_id_short_,
                              std::move(item), [](td::Unit) {});
    }
    next_db_update_at_ = td::Timestamp::in(td::Random::fast(60.0, 120.0));
  }
  if (retry_send_at_ && retry_send_at_.is_in_past()) {
    retry_send_at_ = td::Timestamp::never();
    send_messages_from_queue();
  }
  alarm_timestamp().relax(next_db_update_at_);
  alarm_timestamp().relax(retry_send_at_);
}

void AdnlPeerPairImpl::discover() {
  CHECK(!dht_query_active_);
  CHECK(!dht_node_.empty());
  dht_query_active_ = true;

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), id = print_id(),
                                       peer_id = peer_id_short_](td::Result<dht::DhtValue> kv) {
    if (kv.is_error()) {
      td::actor::send_closure(SelfId, &AdnlPeerPairImpl::got_data_from_dht,
                              kv.move_as_error_prefix("failed to get from dht: "));
      return;
    }
    auto k = kv.move_as_ok();
    auto pub = AdnlNodeIdFull{k.key().public_key()};
    CHECK(pub.compute_short_id() == peer_id);

    auto addr_list = fetch_tl_object<ton_api::adnl_addressList>(k.value().clone(), true);
    if (addr_list.is_error()) {
      td::actor::send_closure(SelfId, &AdnlPeerPairImpl::got_data_from_dht,
                              addr_list.move_as_error_prefix("bad dht value: "));
      return;
    }

    auto F = AdnlAddressList::create(addr_list.move_as_ok());
    if (F.is_error()) {
      td::actor::send_closure(SelfId, &AdnlPeerPairImpl::got_data_from_dht, F.move_as_error_prefix("bad dht value: "));
      return;
    }

    AdnlNode node{pub, F.move_as_ok()};
    td::actor::send_closure(SelfId, &AdnlPeerPairImpl::got_data_from_dht, std::move(node));
  });

  td::actor::send_closure(dht_node_, &dht::Dht::get_value, dht::DhtKey{peer_id_short_.pubkey_hash(), "address", 0},
                          std::move(P));
}

void AdnlPeerPairImpl::receive_packet_checked(AdnlPacket packet) {
  last_received_packet_ = td::Timestamp::now();
  try_reinit_at_ = td::Timestamp::never();
  drop_addr_list_at_ = td::Timestamp::never();
  request_reverse_ping_after_ = td::Timestamp::in(15.0);
  auto d = Adnl::adnl_start_time();
  if (packet.dst_reinit_date() > d) {
    VLOG(ADNL_WARNING) << this << ": dropping IN message: too new our reinit date " << packet.dst_reinit_date();
    return;
  }
  if (packet.reinit_date() > td::Clocks::system() + 60) {
    VLOG(ADNL_NOTICE) << this << ": dropping IN message: too new peer reinit date " << packet.reinit_date();
    return;
  }
  if (packet.reinit_date() > reinit_date_) {
    reinit(packet.reinit_date());
  }
  if (packet.reinit_date() > 0 && packet.reinit_date() < reinit_date_) {
    VLOG(ADNL_NOTICE) << this << ": dropping IN message: old peer reinit date " << packet.reinit_date();
    return;
  }
  if (packet.dst_reinit_date() > 0 && packet.dst_reinit_date() < d) {
    if (!packet.addr_list().empty()) {
      auto addr_list = packet.addr_list();
      if (packet.remote_addr().is_valid() && addr_list.size() == 0) {
        VLOG(ADNL_DEBUG) << "adding implicit address " << packet.remote_addr();
        addr_list.add_udp_address(packet.remote_addr());
      }
      update_addr_list(std::move(addr_list));
    }
    if (!packet.priority_addr_list().empty()) {
      update_addr_list(packet.priority_addr_list());
    }
    VLOG(ADNL_NOTICE) << this << ": dropping IN message old our reinit date " << packet.dst_reinit_date()
                      << " date=" << d;
    auto M = OutboundAdnlMessage{adnlmessage::AdnlMessageNop{}, 0};
    send_message(std::move(M));
    return;
  }
  if (packet.seqno() > 0) {
    if (received_packet(packet.seqno())) {
      VLOG(ADNL_INFO) << this << ": dropping IN message: old seqno: " << packet.seqno() << " (current max " << in_seqno_
                      << ")";
      return;
    }
  }
  if (packet.confirm_seqno() > 0) {
    if (packet.confirm_seqno() > out_seqno_) {
      VLOG(ADNL_WARNING) << this << ": dropping IN message: new ack seqno: " << packet.confirm_seqno()
                         << " (current max sent " << out_seqno_ << ")";
      return;
    }
  }

  // accepted
  // delivering

  if (packet.seqno() > 0) {
    add_received_packet(packet.seqno());
  }

  if (packet.confirm_seqno() > ack_seqno_) {
    ack_seqno_ = packet.confirm_seqno();
  }

  if (packet.recv_addr_list_version() > peer_recv_addr_list_version_) {
    peer_recv_addr_list_version_ = packet.recv_addr_list_version();
  }

  if (packet.recv_priority_addr_list_version() > peer_recv_priority_addr_list_version_) {
    peer_recv_priority_addr_list_version_ = packet.recv_priority_addr_list_version();
  }

  if (!packet.addr_list().empty()) {
    auto addr_list = packet.addr_list();
    if (packet.remote_addr().is_valid() && addr_list.size() == 0) {
      VLOG(ADNL_DEBUG) << "adding implicit address " << packet.remote_addr();
      addr_list.add_udp_address(packet.remote_addr());
    }
    update_addr_list(std::move(addr_list));
  }
  if (!packet.priority_addr_list().empty()) {
    update_addr_list(packet.priority_addr_list());
  }

  received_messages_++;
  if (received_messages_ % 64 == 0) {
    VLOG(ADNL_INFO) << this << ": received " << received_messages_ << " messages";
  }
  for (auto &M : packet.messages().vector()) {
    deliver_message(std::move(M));
  }
}

void AdnlPeerPairImpl::receive_packet_from_channel(AdnlChannelIdShort id, AdnlPacket packet,
                                                   td::uint64 serialized_size) {
  add_packet_stats(serialized_size, /* in = */ true, /* channel = */ true);
  if (id != channel_in_id_) {
    VLOG(ADNL_NOTICE) << this << ": dropping IN message: outdated channel id" << id;
    return;
  }
  if (channel_inited_ && !channel_ready_) {
    channel_ready_ = true;
    if (!out_messages_queue_.empty()) {
      td::actor::send_closure(actor_id(this), &AdnlPeerPairImpl::send_messages_from_queue);
    }
  }
  receive_packet_checked(std::move(packet));
}

void AdnlPeerPairImpl::receive_packet(AdnlPacket packet, td::uint64 serialized_size) {
  add_packet_stats(serialized_size, /* in = */ true, /* channel = */ false);
  packet.run_basic_checks().ensure();

  if (!encryptor_) {
    VLOG(ADNL_NOTICE) << this << "dropping IN message: unitialized id";
    return;
  }

  auto S = encryptor_->check_signature(packet.to_sign().as_slice(), packet.signature().as_slice());
  if (S.is_error()) {
    VLOG(ADNL_NOTICE) << this << "dropping IN message: bad signature: " << S;
    return;
  }

  receive_packet_checked(std::move(packet));
}

void AdnlPeerPairImpl::deliver_message(AdnlMessage message) {
  message.visit([&](const auto &obj) { this->process_message(obj); });
}

void AdnlPeerPairImpl::send_messages_from_queue() {
  while (!out_messages_queue_.empty() && out_messages_queue_.front().second.is_in_past()) {
    out_messages_queue_total_size_ -= out_messages_queue_.front().first.size();
    add_expired_msg_stats(out_messages_queue_.front().first.size());
    out_messages_queue_.pop();
    VLOG(ADNL_NOTICE) << this << ": dropping OUT message: message in queue expired";
  }
  if (out_messages_queue_.empty()) {
    return;
  }

  auto connR = get_conn();
  if (connR.is_error()) {
    disable_dht_query_ = false;
    retry_send_at_.relax(td::Timestamp::in(message_in_queue_ttl_ - 1.0));
    alarm_timestamp().relax(retry_send_at_);
    VLOG(ADNL_INFO) << this << ": delaying OUT messages: cannot get conn: " << connR.move_as_error();
    return;
  }
  disable_dht_query_ = true;
  auto C = connR.move_as_ok();
  auto conn = std::move(C.first);
  bool is_direct = C.second;

  bool first = !skip_init_packet_;
  while (!out_messages_queue_.empty()) {
    bool try_reinit = try_reinit_at_ && try_reinit_at_.is_in_past();
    bool via_channel = channel_ready_ && !try_reinit;
    if (!via_channel && !nochannel_rate_limiter_.take()) {
      alarm_timestamp().relax(retry_send_at_ = nochannel_rate_limiter_.ready_at());
      return;
    }
    if (try_reinit) {
      try_reinit_at_ = td::Timestamp::in(td::Random::fast(0.5, 1.5));
    }
    respond_with_nop_after_ = td::Timestamp::in(td::Random::fast(1.0, 2.0));

    size_t s = (via_channel ? channel_packet_header_max_size() : packet_header_max_size());
    if (first) {
      s += 2 * addr_list_max_size();
    }

    AdnlPacket packet;
    packet.set_seqno(++out_seqno_);
    packet.set_confirm_seqno(in_seqno_);

    if (first) {
      if (!channel_inited_) {
        auto M = adnlmessage::AdnlMessageCreateChannel{channel_pub_, channel_pk_date_};
        s += M.size();
        packet.add_message(std::move(M));
      } else if (!channel_ready_) {
        auto M = adnlmessage::AdnlMessageConfirmChannel{channel_pub_, peer_channel_pub_, channel_pk_date_};
        s += M.size();
        packet.add_message(std::move(M));
      }
    }

    if (!addr_list_.empty()) {
      packet.set_received_addr_list_version(addr_list_.version());
    }
    if (!priority_addr_list_.empty()) {
      packet.set_received_priority_addr_list_version(priority_addr_list_.version());
    }

    skip_init_packet_ = true;
    while (!out_messages_queue_.empty()) {
      auto &M = out_messages_queue_.front().first;
      if (!is_direct && (M.flags() & Adnl::SendFlags::direct_only)) {
        out_messages_queue_total_size_ -= M.size();
        out_messages_queue_.pop();
        continue;
      }
      CHECK(M.size() <= get_mtu());
      if (s + M.size() <= AdnlNetworkManager::get_mtu()) {
        s += M.size();
        out_messages_queue_total_size_ -= M.size();
        packet.add_message(M.release());
        out_messages_queue_.pop();
        skip_init_packet_ = false;
      } else {
        break;
      }
    }

    if (!via_channel) {
      packet.set_reinit_date(Adnl::adnl_start_time(), reinit_date_);
      packet.set_source(local_id_);
    }

    if (!first) {
      if (!channel_inited_) {
        auto M = adnlmessage::AdnlMessageCreateChannel{channel_pub_, channel_pk_date_};
        if (s + M.size() <= AdnlNetworkManager::get_mtu()) {
          s += M.size();
          packet.add_message(std::move(M));
        }
      } else if (!channel_ready_) {
        auto M = adnlmessage::AdnlMessageConfirmChannel{channel_pub_, peer_channel_pub_, channel_pk_date_};
        if (s + M.size() <= AdnlNetworkManager::get_mtu()) {
          s += M.size();
          packet.add_message(std::move(M));
        }
      }
    }

    packet.run_basic_checks().ensure();
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), conn, id = print_id(),
                                         via_channel](td::Result<AdnlPacket> res) {
      if (res.is_error()) {
        LOG(ERROR) << id << ": dropping OUT message: error while creating packet: " << res.move_as_error();
      } else {
        td::actor::send_closure(SelfId, &AdnlPeerPairImpl::send_packet_continue, res.move_as_ok(), conn, via_channel);
      }
    });

    td::actor::send_closure(local_actor_, &AdnlLocalId::update_packet, std::move(packet),
                            (!channel_ready_ && ack_seqno_ == 0 && in_seqno_ == 0) || try_reinit, !via_channel,
                            (first || s + addr_list_max_size() <= AdnlNetworkManager::get_mtu())
                                ? (try_reinit ? 0 : peer_recv_addr_list_version_)
                                : 0x7fffffff,
                            (first || s + 2 * addr_list_max_size() <= AdnlNetworkManager::get_mtu())
                                ? peer_recv_priority_addr_list_version_
                                : 0x7fffffff,
                            std::move(P));
    first = false;
  }
}

void AdnlPeerPairImpl::send_messages(std::vector<OutboundAdnlMessage> messages) {
  std::vector<OutboundAdnlMessage> new_vec;
  for (auto &M : messages) {
    if (M.size() <= get_mtu()) {
      new_vec.push_back(std::move(M));
    } else {
      auto B = serialize_tl_object(M.tl(), true);
      CHECK(B.size() <= huge_packet_max_size());

      auto hash = sha256_bits256(B.as_slice());

      auto size = static_cast<td::uint32>(B.size());
      td::uint32 offset = 0;
      td::uint32 part_size = Adnl::get_mtu();
      while (offset < size) {
        auto data = B.clone();
        if (data.size() > part_size) {
          data.truncate(part_size);
        }
        B.confirm_read(data.size());

        new_vec.push_back(
            OutboundAdnlMessage{adnlmessage::AdnlMessagePart{hash, size, offset, std::move(data)}, M.flags()});
        offset += part_size;
      }
    }
  }
  for (auto &m : new_vec) {
    out_messages_queue_total_size_ += m.size();
    out_messages_queue_.emplace(std::move(m), td::Timestamp::in(message_in_queue_ttl_));
  }
  send_messages_from_queue();
}

void AdnlPeerPairImpl::send_packet_continue(AdnlPacket packet, td::actor::ActorId<AdnlNetworkConnection> conn,
                                            bool via_channel) {
  if (!try_reinit_at_ && last_received_packet_ < td::Timestamp::in(-5.0)) {
    try_reinit_at_ = td::Timestamp::in(10.0);
  }
  if (!drop_addr_list_at_ && last_received_packet_ < td::Timestamp::in(-60.0 * 9.0)) {
    drop_addr_list_at_ = td::Timestamp::in(60.0);
  }
  packet.run_basic_checks().ensure();
  auto B = serialize_tl_object(packet.tl(), true);
  if (via_channel) {
    if (channel_ready_) {
      add_packet_stats(B.size(), /* in = */ false, /* channel = */ true);
      td::actor::send_closure(channel_, &AdnlChannel::send_message, priority_, conn, std::move(B));
    } else {
      VLOG(ADNL_WARNING) << this << ": dropping OUT message [" << local_id_ << "->" << peer_id_short_
                         << "]: channel destroyed in process";
    }
    return;
  }

  if (!encryptor_) {
    VLOG(ADNL_INFO) << this << ": dropping OUT message [" << local_id_ << "->" << peer_id_short_
                    << "]: empty encryptor";
    return;
  }

  auto res = encryptor_->encrypt(B.as_slice());
  if (res.is_error()) {
    VLOG(ADNL_WARNING) << this << ": dropping OUT message [" << local_id_ << "->" << peer_id_short_
                       << "]: failed to encrypt: " << res.move_as_error();
    return;
  }
  auto X = res.move_as_ok();
  auto enc = td::BufferSlice(X.size() + 32);
  td::MutableSlice S = enc.as_slice();
  S.copy_from(peer_id_short_.as_slice());
  S.remove_prefix(32);
  S.copy_from(X.as_slice());

  add_packet_stats(B.size(), /* in = */ false, /* channel = */ false);
  td::actor::send_closure(conn, &AdnlNetworkConnection::send, local_id_, peer_id_short_, priority_, std::move(enc));
}

void AdnlPeerPairImpl::send_query(std::string name, td::Promise<td::BufferSlice> promise, td::Timestamp timeout,
                                  td::BufferSlice data, td::uint32 flags) {
  AdnlQueryId id = AdnlQuery::random_query_id();
  CHECK(out_queries_.count(id) == 0);

  auto P = [SelfId = actor_id(this)](AdnlQueryId id) {
    td::actor::send_closure(SelfId, &AdnlPeerPairImpl::delete_query, id);
  };

  out_queries_[id] = AdnlQuery::create(std::move(promise), std::move(P), name, timeout, id);

  send_message(OutboundAdnlMessage{adnlmessage::AdnlMessageQuery{id, std::move(data)}, flags});
}

void AdnlPeerPairImpl::alarm_query(AdnlQueryId id) {
  out_queries_.erase(id);
}

AdnlPeerPairImpl::AdnlPeerPairImpl(td::actor::ActorId<AdnlNetworkManager> network_manager,
                                   td::actor::ActorId<AdnlPeerTable> peer_table, td::uint32 local_mode,
                                   td::actor::ActorId<AdnlLocalId> local_actor, td::actor::ActorId<AdnlPeer> peer,
                                   td::actor::ActorId<dht::Dht> dht_node, AdnlNodeIdShort local_id,
                                   AdnlNodeIdShort peer_id) {
  network_manager_ = network_manager;
  peer_table_ = peer_table;
  local_actor_ = local_actor;
  peer_ = peer;
  dht_node_ = dht_node;
  mode_ = local_mode;

  local_id_ = local_id;
  peer_id_short_ = peer_id;

  channel_pk_ = privkeys::Ed25519::random();
  channel_pub_ = channel_pk_.pub();
  channel_pk_date_ = static_cast<td::int32>(td::Clocks::system());
}

void AdnlPeerPairImpl::create_channel(pubkeys::Ed25519 pub, td::uint32 date) {
  if (channel_inited_ && peer_channel_pub_ == pub) {
    return;
  }
  if (channel_inited_ && date <= peer_channel_date_) {
    return;
  }
  if (channel_inited_) {
    td::actor::send_closure(peer_table_, &AdnlPeerTable::unregister_channel, channel_in_id_);
    channel_.reset();
    channel_inited_ = false;
    channel_ready_ = false;
  }
  CHECK(!channel_ready_);

  peer_channel_pub_ = pub;
  peer_channel_date_ = date;

  auto R = AdnlChannel::create(channel_pk_, peer_channel_pub_, local_id_, peer_id_short_, channel_out_id_,
                               channel_in_id_, actor_id(this));
  if (R.is_ok()) {
    channel_ = R.move_as_ok();
    channel_inited_ = true;

    td::actor::send_closure_later(peer_table_, &AdnlPeerTable::register_channel, channel_in_id_, local_id_,
                                  channel_.get());
  } else {
    VLOG(ADNL_WARNING) << this << ": failed to create channel: " << R.move_as_error();
  }
}

void AdnlPeerPairImpl::process_message(const adnlmessage::AdnlMessageCreateChannel &message) {
  create_channel(message.key(), message.date());
}

void AdnlPeerPairImpl::process_message(const adnlmessage::AdnlMessageConfirmChannel &message) {
  if (message.peer_key() != channel_pub_) {
    VLOG(ADNL_NOTICE) << this << ": received adnl.message.confirmChannel with bad peer_key";
    return;
  }
  create_channel(message.key(), message.date());
  if (!channel_inited_ || peer_channel_pub_ != message.key()) {
    VLOG(ADNL_NOTICE) << this << ": received adnl.message.confirmChannel with old key";
    return;
  }
  if (!channel_ready_) {
    channel_ready_ = true;
    send_messages_from_queue();
  }
}

void AdnlPeerPairImpl::process_message(const adnlmessage::AdnlMessageCustom &message) {
  respond_with_nop();
  td::actor::send_closure(local_actor_, &AdnlLocalId::deliver, peer_id_short_, message.data());
}

void AdnlPeerPairImpl::process_message(const adnlmessage::AdnlMessageNop &message) {
  // nop
}

void AdnlPeerPairImpl::process_message(const adnlmessage::AdnlMessageReinit &message) {
  reinit(message.date());
}

void AdnlPeerPairImpl::process_message(const adnlmessage::AdnlMessageQuery &message) {
  respond_with_nop();
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), query_id = message.query_id(),
                                       flags = static_cast<td::uint32>(0)](td::Result<td::BufferSlice> R) {
    if (R.is_error()) {
      LOG(WARNING) << "failed to answer query: " << R.move_as_error();
    } else {
      auto data = R.move_as_ok();
      if (data.size() > Adnl::huge_packet_max_size()) {
        LOG(WARNING) << "dropping too big answer query: size=" << data.size();
      } else {
        td::actor::send_closure(SelfId, &AdnlPeerPairImpl::send_message,
                                OutboundAdnlMessage{adnlmessage::AdnlMessageAnswer{query_id, std::move(data)}, flags});
      }
    }
  });
  td::actor::send_closure(local_actor_, &AdnlLocalId::deliver_query, peer_id_short_, message.data(), std::move(P));
}

void AdnlPeerPairImpl::process_message(const adnlmessage::AdnlMessageAnswer &message) {
  respond_with_nop();
  auto Q = out_queries_.find(message.query_id());

  if (Q == out_queries_.end()) {
    VLOG(ADNL_NOTICE) << this << ": dropping IN answer: unknown query id " << message.query_id();
    return;
  }

  if (message.data().size() > Adnl::huge_packet_max_size()) {
    VLOG(ADNL_NOTICE) << this << ": dropping IN answer: too big answer size";
    return;
  }

  td::actor::send_closure_later(Q->second, &AdnlQuery::result, message.data());
  out_queries_.erase(Q);
}

void AdnlPeerPairImpl::process_message(const adnlmessage::AdnlMessagePart &message) {
  respond_with_nop();
  auto size = message.total_size();
  if (size > huge_packet_max_size()) {
    VLOG(ADNL_INFO) << this << ": dropping too big huge message: size=" << size;
    return;
  }
  if (message.hash().is_zero()) {
    VLOG(ADNL_INFO) << this << ": dropping huge message with zero hash";
    return;
  }
  if (message.hash() != huge_message_hash_) {
    huge_message_hash_.set_zero();
    huge_message_.clear();
    huge_message_offset_ = 0;
    if (message.offset() == 0) {
      huge_message_hash_ = message.hash();
      huge_message_ = td::BufferSlice{size};
    } else {
      return;
    }
  }
  auto data = message.data();
  if (data.size() + message.offset() > size) {
    VLOG(ADNL_WARNING) << this << ": dropping huge message with bad part";
    return;
  }
  if (size != huge_message_.size()) {
    VLOG(ADNL_WARNING) << this << ": dropping huge message part with inconsistent size";
    return;
  }
  if (message.offset() == huge_message_offset_) {
    auto S = huge_message_.as_slice();
    S.remove_prefix(huge_message_offset_);
    S.copy_from(data.as_slice());
    huge_message_offset_ += static_cast<td::uint32>(data.size());

    if (huge_message_offset_ == huge_message_.size()) {
      //td::actor::send_closure(local_actor_, &AdnlLocalId::deliver, peer_id_short_, std::move(huge_message_));
      if (sha256_bits256(huge_message_.as_slice()) != huge_message_hash_) {
        VLOG(ADNL_WARNING) << this << ": dropping huge message: hash mismatch";
        return;
      }
      huge_message_hash_.set_zero();
      huge_message_offset_ = 0;
      auto MR = fetch_tl_object<ton_api::adnl_Message>(std::move(huge_message_), true);
      if (MR.is_error()) {
        VLOG(ADNL_WARNING) << this << ": dropping huge message part with bad data";
        return;
      }
      auto M = AdnlMessage{MR.move_as_ok()};
      deliver_message(std::move(M));
    }
  }
}

void AdnlPeerPairImpl::delete_query(AdnlQueryId id) {
  auto Q = out_queries_.find(id);

  if (Q != out_queries_.end()) {
    out_queries_.erase(Q);
  }
}

void AdnlPeerPairImpl::respond_with_nop() {
  if (respond_with_nop_after_.is_in_past()) {
    std::vector<OutboundAdnlMessage> messages;
    messages.emplace_back(adnlmessage::AdnlMessageNop{}, 0);
    send_messages(std::move(messages));
  }
}

void AdnlPeerPairImpl::reinit(td::int32 date) {
  if (reinit_date_ == 0) {
    reinit_date_ = date;
  }
  if (reinit_date_ < date) {
    if (channel_inited_) {
      td::actor::send_closure(peer_table_, &AdnlPeerTable::unregister_channel, channel_in_id_);
    }

    in_seqno_ = 0;
    out_seqno_ = 0;
    ack_seqno_ = 0;
    recv_seqno_mask_ = 0;

    channel_ready_ = false;
    channel_inited_ = false;

    peer_recv_addr_list_version_ = 0;

    huge_message_offset_ = 0;
    huge_message_hash_.set_zero();
    huge_message_.clear();

    channel_.release();

    reinit_date_ = date;
  }
}

td::Result<std::pair<td::actor::ActorId<AdnlNetworkConnection>, bool>> AdnlPeerPairImpl::get_conn() {
  if (drop_addr_list_at_ && drop_addr_list_at_.is_in_past()) {
    drop_addr_list_at_ = td::Timestamp::never();
    priority_addr_list_ = AdnlAddressList{};
    priority_conns_.clear();
    addr_list_ = AdnlAddressList{};
    conns_.clear();
    has_reverse_addr_ = false;
    return td::Status::Error(ErrorCode::notready, "no active connections");
  }

  if (!priority_addr_list_.empty() && priority_addr_list_.expire_at() < td::Clocks::system()) {
    priority_addr_list_ = AdnlAddressList{};
    priority_conns_.clear();
  }

  if (conns_.size() == 0 && priority_conns_.size() == 0) {
    if (has_reverse_addr_) {
      request_reverse_ping();
      return td::Status::Error(ErrorCode::notready, "waiting for reverse ping");
    } else {
      return td::Status::Error(ErrorCode::notready, PSTRING()
                                                        << "empty network information: version=" << addr_list_.version()
                                                        << " reinit_date=" << addr_list_.reinit_date()
                                                        << " real_reinit_date=" << reinit_date_);
    }
  }

  for (int direct_only = 1; direct_only >= 0; --direct_only) {
    for (auto &conn : priority_conns_) {
      if (conn.ready() && (!direct_only || conn.is_direct())) {
        return std::make_pair(conn.conn.get(), conn.is_direct());
      }
    }
  }
  for (int direct_only = 1; direct_only >= 0; --direct_only) {
    for (auto &conn : conns_) {
      if (conn.ready() && (!direct_only || conn.is_direct())) {
        return std::make_pair(conn.conn.get(), conn.is_direct());
      }
    }
  }
  return td::Status::Error(ErrorCode::notready, "no active connections");
}

void AdnlPeerPairImpl::update_addr_list(AdnlAddressList addr_list) {
  if (addr_list.empty()) {
    return;
  }
  //CHECK(addr_list.size() > 0);

  if (addr_list.reinit_date() > td::Clocks::system() + 60) {
    VLOG(ADNL_WARNING) << "dropping addr list with too new reinit date";
    return;
  }

  if (addr_list.reinit_date() > reinit_date_) {
    reinit(addr_list.reinit_date());
  } else if (addr_list.reinit_date() < reinit_date_) {
    return;
  }

  bool priority = addr_list.priority() > 0;

  if ((priority ? priority_addr_list_ : addr_list_).version() >= addr_list.version()) {
    if (priority && priority_addr_list_.version() == addr_list.version()) {
      auto expire_at = addr_list.expire_at();
      if (expire_at > priority_addr_list_.expire_at()) {
        priority_addr_list_.set_expire_at(expire_at);
      }
    }
    return;
  }

  VLOG(ADNL_INFO) << this << ": updating addr list to version " << addr_list.version() << " size=" << addr_list.size();

  const auto addrs = addr_list.addrs();
  has_reverse_addr_ = addr_list.has_reverse();
  if (has_reverse_addr_ && addrs.empty()) {
    return;
  }
  std::vector<Conn> conns;
  auto &old_conns = priority ? priority_conns_ : conns_;

  size_t idx = 0;
  for (const auto &addr : addrs) {
    if (addr->is_reverse()) {
      continue;
    }
    if ((mode_ & static_cast<td::uint32>(AdnlLocalIdMode::direct_only)) && !addr->is_public()) {
      continue;
    }
    auto hash = addr->get_hash();
    if (idx < old_conns.size() && old_conns[idx].addr->get_hash() == hash) {
      conns.push_back(std::move(old_conns[idx]));
    } else {
      conns.push_back(Conn{addr, actor_id(this), network_manager_, peer_table_});
    }
    idx++;
  }

  old_conns = std::move(conns);
  (priority ? priority_addr_list_ : addr_list_) = addr_list;
}

void AdnlPeerPairImpl::get_conn_ip_str(td::Promise<td::string> promise) {
  if (conns_.size() == 0 && priority_conns_.size() == 0) {
    promise.set_value("undefined");
    return;
  }

  for (auto &conn : priority_conns_) {
    if (conn.ready()) {
      td::actor::send_closure(conn.conn, &AdnlNetworkConnection::get_ip_str, std::move(promise));
      return;
    }
  }
  for (auto &conn : conns_) {
    if (conn.ready()) {
      td::actor::send_closure(conn.conn, &AdnlNetworkConnection::get_ip_str, std::move(promise));
      return;
    }
  }

  promise.set_value("undefined");
}

void AdnlPeerPairImpl::get_stats(bool all, td::Promise<tl_object_ptr<ton_api::adnl_stats_peerPair>> promise) {
  if (!all) {
    double threshold = td::Clocks::system() - 600.0;
    if (last_in_packet_ts_ < threshold && last_out_packet_ts_ < threshold) {
      promise.set_value(nullptr);
      return;
    }
  }

  auto stats = create_tl_object<ton_api::adnl_stats_peerPair>();
  stats->local_id_ = local_id_.bits256_value();
  stats->peer_id_ = peer_id_short_.bits256_value();
  for (const AdnlAddress &addr : addr_list_.addrs()) {
    ton_api::downcast_call(*addr->tl(), td::overloaded(
                                            [&](const ton_api::adnl_address_udp &obj) {
                                              stats->ip_str_ = PSTRING() << td::IPAddress::ipv4_to_str(obj.ip_) << ":"
                                                                         << obj.port_;
                                            },
                                            [&](const auto &) {}));
    if (!stats->ip_str_.empty()) {
      break;
    }
  }

  prepare_packet_stats();
  stats->last_in_packet_ts_ = last_in_packet_ts_;
  stats->last_out_packet_ts_ = last_out_packet_ts_;
  stats->packets_total_ = packet_stats_total_.tl();
  stats->packets_total_->ts_start_ = started_ts_;
  stats->packets_total_->ts_end_ = td::Clocks::system();
  stats->packets_recent_ = packet_stats_prev_.tl();

  if (channel_ready_) {
    stats->channel_status_ = 2;
  } else if (channel_inited_) {
    stats->channel_status_ = 1;
  } else {
    stats->channel_status_ = 0;
  }
  stats->try_reinit_at_ = (try_reinit_at_ ? try_reinit_at_.at_unix() : 0.0);
  stats->connection_ready_ =
      std::any_of(conns_.begin(), conns_.end(), [](const Conn &conn) { return conn.ready(); }) ||
      std::any_of(priority_conns_.begin(), priority_conns_.end(), [](const Conn &conn) { return conn.ready(); });
  stats->out_queue_messages_ = out_messages_queue_.size();
  stats->out_queue_bytes_ = out_messages_queue_total_size_;

  promise.set_result(std::move(stats));
}

void AdnlPeerImpl::update_id(AdnlNodeIdFull id) {
  CHECK(id.compute_short_id() == peer_id_short_);
  if (!peer_id_.empty()) {
    return;
  }

  peer_id_ = std::move(id);

  for (auto &it : peer_pairs_) {
    td::actor::send_closure(it.second.get(), &AdnlPeerPair::update_peer_id, peer_id_);
  }
}

void AdnlPeerPairImpl::Conn::create_conn(td::actor::ActorId<AdnlPeerPairImpl> peer,
                                         td::actor::ActorId<AdnlNetworkManager> network_manager,
                                         td::actor::ActorId<Adnl> adnl) {
  auto id = addr->get_hash();

  conn = addr->create_connection(network_manager, adnl, std::make_unique<ConnCallback>(peer, id));
}

void AdnlPeerPairImpl::conn_change_state(AdnlConnectionIdShort id, bool ready) {
  if (ready) {
    if (out_messages_queue_.empty()) {
      send_messages_from_queue();
    }
  }
}

td::actor::ActorOwn<AdnlPeerPair> AdnlPeerPair::create(
    td::actor::ActorId<AdnlNetworkManager> network_manager, td::actor::ActorId<AdnlPeerTable> peer_table,
    td::uint32 local_mode, td::actor::ActorId<AdnlLocalId> local_actor, td::actor::ActorId<AdnlPeer> peer_actor,
    td::actor::ActorId<dht::Dht> dht_node, AdnlNodeIdShort local_id, AdnlNodeIdShort peer_id) {
  auto X = td::actor::create_actor<AdnlPeerPairImpl>("peerpair", network_manager, peer_table, local_mode, local_actor,
                                                     peer_actor, dht_node, local_id, peer_id);
  return td::actor::ActorOwn<AdnlPeerPair>(std::move(X));
}

td::actor::ActorOwn<AdnlPeer> AdnlPeer::create(td::actor::ActorId<AdnlNetworkManager> network_manager,
                                               td::actor::ActorId<AdnlPeerTable> peer_table,
                                               td::actor::ActorId<dht::Dht> dht_node, AdnlNodeIdShort peer_id) {
  auto X = td::actor::create_actor<AdnlPeerImpl>("peer", network_manager, peer_table, dht_node, peer_id);
  return td::actor::ActorOwn<AdnlPeer>(std::move(X));
}

void AdnlPeerImpl::receive_packet(AdnlNodeIdShort dst, td::uint32 dst_mode, td::actor::ActorId<AdnlLocalId> dst_actor,
                                  AdnlPacket packet, td::uint64 serialized_size) {
  if (packet.inited_from()) {
    update_id(packet.from());
  }

  auto it = peer_pairs_.find(dst);
  if (it == peer_pairs_.end()) {
    auto X = AdnlPeerPair::create(network_manager_, peer_table_, dst_mode, dst_actor, actor_id(this), dht_node_, dst,
                                  peer_id_short_);
    peer_pairs_.emplace(dst, std::move(X));
    it = peer_pairs_.find(dst);
    CHECK(it != peer_pairs_.end());

    if (!peer_id_.empty()) {
      td::actor::send_closure(it->second.get(), &AdnlPeerPair::update_peer_id, peer_id_);
    }
  }

  td::actor::send_closure(it->second.get(), &AdnlPeerPair::receive_packet, std::move(packet), serialized_size);
}

void AdnlPeerImpl::send_messages(AdnlNodeIdShort src, td::uint32 src_mode, td::actor::ActorId<AdnlLocalId> src_actor,
                                 std::vector<OutboundAdnlMessage> messages) {
  auto it = peer_pairs_.find(src);
  if (it == peer_pairs_.end()) {
    auto X = AdnlPeerPair::create(network_manager_, peer_table_, src_mode, src_actor, actor_id(this), dht_node_, src,
                                  peer_id_short_);
    peer_pairs_.emplace(src, std::move(X));
    it = peer_pairs_.find(src);
    CHECK(it != peer_pairs_.end());

    if (!peer_id_.empty()) {
      td::actor::send_closure(it->second.get(), &AdnlPeerPair::update_peer_id, peer_id_);
    }
  }

  td::actor::send_closure(it->second, &AdnlPeerPair::send_messages, std::move(messages));
}

void AdnlPeerImpl::send_query(AdnlNodeIdShort src, td::uint32 src_mode, td::actor::ActorId<AdnlLocalId> src_actor,
                              std::string name, td::Promise<td::BufferSlice> promise, td::Timestamp timeout,
                              td::BufferSlice data, td::uint32 flags) {
  auto it = peer_pairs_.find(src);
  if (it == peer_pairs_.end()) {
    auto X = AdnlPeerPair::create(network_manager_, peer_table_, src_mode, src_actor, actor_id(this), dht_node_, src,
                                  peer_id_short_);
    peer_pairs_.emplace(src, std::move(X));
    it = peer_pairs_.find(src);
    CHECK(it != peer_pairs_.end());

    if (!peer_id_.empty()) {
      td::actor::send_closure(it->second.get(), &AdnlPeerPair::update_peer_id, peer_id_);
    }
  }

  td::actor::send_closure(it->second, &AdnlPeerPair::send_query, name, std::move(promise), timeout, std::move(data),
                          flags);
}

void AdnlPeerImpl::del_local_id(AdnlNodeIdShort local_id) {
  peer_pairs_.erase(local_id);
}

void AdnlPeerImpl::update_dht_node(td::actor::ActorId<dht::Dht> dht_node) {
  dht_node_ = dht_node;
  for (auto it = peer_pairs_.begin(); it != peer_pairs_.end(); it++) {
    td::actor::send_closure(it->second, &AdnlPeerPair::update_dht_node, dht_node_);
  }
}

void AdnlPeerImpl::get_conn_ip_str(AdnlNodeIdShort l_id, td::Promise<td::string> promise) {
  auto it = peer_pairs_.find(l_id);
  if (it == peer_pairs_.end()) {
    promise.set_value("undefined");
    return;
  }

  td::actor::send_closure(it->second, &AdnlPeerPair::get_conn_ip_str, std::move(promise));
}

void AdnlPeerImpl::update_addr_list(AdnlNodeIdShort local_id, td::uint32 local_mode,
                                    td::actor::ActorId<AdnlLocalId> local_actor, AdnlAddressList addr_list) {
  auto it = peer_pairs_.find(local_id);
  if (it == peer_pairs_.end()) {
    auto X = AdnlPeerPair::create(network_manager_, peer_table_, local_mode, local_actor, actor_id(this), dht_node_,
                                  local_id, peer_id_short_);
    peer_pairs_.emplace(local_id, std::move(X));
    it = peer_pairs_.find(local_id);
    CHECK(it != peer_pairs_.end());

    if (!peer_id_.empty()) {
      td::actor::send_closure(it->second.get(), &AdnlPeerPair::update_peer_id, peer_id_);
    }
  }

  td::actor::send_closure(it->second, &AdnlPeerPair::update_addr_list, std::move(addr_list));
}

void AdnlPeerImpl::get_stats(bool all, td::Promise<std::vector<tl_object_ptr<ton_api::adnl_stats_peerPair>>> promise) {
  class Cb : public td::actor::Actor {
   public:
    explicit Cb(td::Promise<std::vector<tl_object_ptr<ton_api::adnl_stats_peerPair>>> promise)
        : promise_(std::move(promise)) {
    }

    void got_peer_pair_stats(tl_object_ptr<ton_api::adnl_stats_peerPair> peer_pair) {
      if (peer_pair) {
        result_.push_back(std::move(peer_pair));
      }
      dec_pending();
    }

    void inc_pending() {
      ++pending_;
    }

    void dec_pending() {
      CHECK(pending_ > 0);
      --pending_;
      if (pending_ == 0) {
        promise_.set_result(std::move(result_));
        stop();
      }
    }

   private:
    td::Promise<std::vector<tl_object_ptr<ton_api::adnl_stats_peerPair>>> promise_;
    size_t pending_ = 1;
    std::vector<tl_object_ptr<ton_api::adnl_stats_peerPair>> result_;
  };
  auto callback = td::actor::create_actor<Cb>("adnlpeerstats", std::move(promise)).release();

  for (auto &[local_id, peer_pair] : peer_pairs_) {
    td::actor::send_closure(callback, &Cb::inc_pending);
    td::actor::send_closure(peer_pair, &AdnlPeerPair::get_stats, all,
                            [local_id = local_id, peer_id = peer_id_short_,
                             callback](td::Result<tl_object_ptr<ton_api::adnl_stats_peerPair>> R) {
                              if (R.is_error()) {
                                VLOG(ADNL_NOTICE) << "failed to get stats for peer pair " << peer_id << "->" << local_id
                                                  << " : " << R.move_as_error();
                                td::actor::send_closure(callback, &Cb::dec_pending);
                              } else {
                                td::actor::send_closure(callback, &Cb::got_peer_pair_stats, R.move_as_ok());
                              }
                            });
  }
  td::actor::send_closure(callback, &Cb::dec_pending);
}


void AdnlPeerPairImpl::got_data_from_db(td::Result<AdnlDbItem> R) {
  received_from_db_ = false;
  if (R.is_error()) {
    return;
  }
  auto value = R.move_as_ok();
  if (!value.id.empty()) {
    update_peer_id(value.id);
  }
  update_addr_list(value.addr_list);
  update_addr_list(value.priority_addr_list);
}

void AdnlPeerPairImpl::got_data_from_static_nodes(td::Result<AdnlNode> R) {
  received_from_static_nodes_ = false;
  if (R.is_error()) {
    return;
  }
  auto value = R.move_as_ok();
  if (!value.pub_id().empty()) {
    update_peer_id(value.pub_id());
  }
  update_addr_list(value.addr_list());
}

void AdnlPeerPairImpl::got_data_from_dht(td::Result<AdnlNode> R) {
  CHECK(dht_query_active_);
  dht_query_active_ = false;
  next_dht_query_at_ = td::Timestamp::in(td::Random::fast(60.0, 120.0));
  if (R.is_error()) {
    VLOG(ADNL_INFO) << this << ": dht query failed: " << R.move_as_error();
    return;
  }
  auto value = R.move_as_ok();
  if (!value.pub_id().empty()) {
    update_peer_id(value.pub_id());
  }
  update_addr_list(value.addr_list());
}

void AdnlPeerPairImpl::update_peer_id(AdnlNodeIdFull id) {
  if (peer_id_.empty()) {
    peer_id_ = std::move(id);
    auto R = peer_id_.pubkey().create_encryptor();
    if (R.is_ok()) {
      encryptor_ = R.move_as_ok();
    } else {
      VLOG(ADNL_WARNING) << this << ": failed to create encryptor: " << R.move_as_error();
    }
  }
  CHECK(!peer_id_.empty());
}

void AdnlPeerPairImpl::request_reverse_ping() {
  if (request_reverse_ping_active_ || !request_reverse_ping_after_.is_in_past()) {
    return;
  }
  VLOG(ADNL_INFO) << this << ": requesting reverse ping";
  request_reverse_ping_after_ = td::Timestamp::in(15.0);
  request_reverse_ping_active_ = true;
  td::actor::send_closure(
      local_actor_, &AdnlLocalId::get_self_node,
      [SelfId = actor_id(this), peer = peer_id_short_, dht = dht_node_](td::Result<AdnlNode> R) {
        if (R.is_error()) {
          td::actor::send_closure(SelfId, &AdnlPeerPairImpl::request_reverse_ping_result, R.move_as_error());
          return;
        }
        td::actor::send_closure(
            dht, &dht::Dht::request_reverse_ping, R.move_as_ok(), peer, [SelfId](td::Result<td::Unit> R) {
              td::actor::send_closure(SelfId, &AdnlPeerPairImpl::request_reverse_ping_result, std::move(R));
            });
      });
}

void AdnlPeerPairImpl::request_reverse_ping_result(td::Result<td::Unit> R) {
  request_reverse_ping_active_ = false;
  if (R.is_ok()) {
    VLOG(ADNL_INFO) << this << ": reverse ping requested";
  } else {
    VLOG(ADNL_INFO) << this << ": failed to request reverse ping: " << R.move_as_error();
  }
}

void AdnlPeerPairImpl::add_packet_stats(td::uint64 bytes, bool in, bool channel) {
  prepare_packet_stats();
  auto add_stats = [&](PacketStats &stats) {
    if (in) {
      ++stats.in_packets;
      stats.in_bytes += bytes;
      if (channel) {
        ++stats.in_packets_channel;
        stats.in_bytes_channel += bytes;
      }
    } else {
      ++stats.out_packets;
      stats.out_bytes += bytes;
      if (channel) {
        ++stats.out_packets_channel;
        stats.out_bytes_channel += bytes;
      }
    }
  };
  add_stats(packet_stats_cur_);
  add_stats(packet_stats_total_);
  if (in) {
    last_in_packet_ts_ = td::Clocks::system();
  } else {
    last_out_packet_ts_ = td::Clocks::system();
  }
}

void AdnlPeerPairImpl::add_expired_msg_stats(td::uint64 bytes) {
  prepare_packet_stats();
  auto add_stats = [&](PacketStats &stats) {
    ++stats.out_expired_messages;
    stats.out_expired_bytes += bytes;
  };
  add_stats(packet_stats_cur_);
  add_stats(packet_stats_total_);
}

void AdnlPeerPairImpl::prepare_packet_stats() {
  double now = td::Clocks::system();
  if (now >= packet_stats_cur_.ts_end) {
    packet_stats_prev_ = std::move(packet_stats_cur_);
    packet_stats_cur_ = {};
    auto now_int = (int)now;
    packet_stats_cur_.ts_start = (double)(now_int / 60 * 60);
    packet_stats_cur_.ts_end = packet_stats_cur_.ts_start + 60.0;
    if (packet_stats_prev_.ts_end < now - 60.0) {
      packet_stats_prev_ = {};
      packet_stats_prev_.ts_end = packet_stats_cur_.ts_start;
      packet_stats_prev_.ts_start = packet_stats_prev_.ts_end - 60.0;
    }
  }
}

tl_object_ptr<ton_api::adnl_stats_packets> AdnlPeerPairImpl::PacketStats::tl() const {
  return create_tl_object<ton_api::adnl_stats_packets>(ts_start, ts_end, in_packets, in_bytes, in_packets_channel,
                                                       in_bytes_channel, out_packets, out_bytes, out_packets_channel,
                                                       out_bytes_channel, out_expired_messages, out_expired_bytes);
}

}  // namespace adnl

}  // namespace ton
