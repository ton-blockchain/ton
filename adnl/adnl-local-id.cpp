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
#include "td/utils/crypto.h"
#include "td/utils/Random.h"

#include "adnl-local-id.h"
#include "keys/encryptor.h"
#include "utils.hpp"

namespace ton {

namespace adnl {

AdnlNodeIdFull AdnlLocalId::get_id() const {
  return id_;
}

AdnlNodeIdShort AdnlLocalId::get_short_id() const {
  return short_id_;
}

AdnlAddressList AdnlLocalId::get_addr_list() const {
  CHECK(!addr_list_.empty());
  return addr_list_;
}

void AdnlLocalId::receive(td::IPAddress addr, td::BufferSlice data) {
  InboundRateLimiter& rate_limiter = inbound_rate_limiter_[addr];
  if (!rate_limiter.rate_limiter.take()) {
    VLOG(ADNL_NOTICE) << this << ": dropping IN message: rate limit exceeded";
    add_dropped_packet_stats(addr);
    return;
  }
  ++rate_limiter.currently_decrypting_packets;
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), peer_table = peer_table_, dst = short_id_, addr,
                                       id = print_id(), size = data.size()](td::Result<AdnlPacket> R) {
    td::actor::send_closure(SelfId, &AdnlLocalId::decrypt_packet_done, addr);
    if (R.is_error()) {
      VLOG(ADNL_WARNING) << id << ": dropping IN message: cannot decrypt: " << R.move_as_error();
    } else {
      auto packet = R.move_as_ok();
      packet.set_remote_addr(addr);
      td::actor::send_closure(peer_table, &AdnlPeerTable::receive_decrypted_packet, dst, std::move(packet), size);
    }
  });
  decrypt(std::move(data), std::move(P));
}

void AdnlLocalId::decrypt_packet_done(td::IPAddress addr) {
  auto it = inbound_rate_limiter_.find(addr);
  CHECK(it != inbound_rate_limiter_.end());
  --it->second.currently_decrypting_packets;
  add_decrypted_packet_stats(addr);
}

void AdnlLocalId::deliver(AdnlNodeIdShort src, td::BufferSlice data) {
  auto s = std::move(data);
  for (auto &cb : cb_) {
    auto f = cb.first;
    if (f.length() <= s.length() && s.as_slice().substr(0, f.length()) == f) {
      cb.second->receive_message(src, short_id_, std::move(s));
      return;
    }
  }
  VLOG(ADNL_INFO) << this << ": dropping IN message from " << src
                  << ": no callbacks for custom message. firstint=" << td::TlParser(s.as_slice()).fetch_int();
}

void AdnlLocalId::deliver_query(AdnlNodeIdShort src, td::BufferSlice data, td::Promise<td::BufferSlice> promise) {
  auto s = std::move(data);
  for (auto &cb : cb_) {
    auto f = cb.first;
    if (f.length() <= s.length() && s.as_slice().substr(0, f.length()) == f) {
      cb.second->receive_query(src, short_id_, std::move(s), std::move(promise));
      return;
    }
  }
  VLOG(ADNL_INFO) << this << ": dropping IN message from " << src
                  << ": no callbacks for custom query. firstint=" << td::TlParser(s.as_slice()).fetch_int();
  promise.set_error(td::Status::Error(ErrorCode::warning, PSTRING() << "dropping IN message from " << src
                                                                    << ": no callbacks for custom query. firstint="
                                                                    << td::TlParser(s.as_slice()).fetch_int()));
}

void AdnlLocalId::subscribe(std::string prefix, std::unique_ptr<AdnlPeerTable::Callback> callback) {
  auto S = td::Slice(prefix);
  for (auto &cb : cb_) {
    auto G = td::Slice(cb.first);
    if (S.size() < G.size()) {
      LOG_CHECK(G.substr(0, S.size()) != S) << this << ": duplicate subscribe prefix";
    } else {
      LOG_CHECK(S.substr(0, G.size()) != G) << this << ": duplicate subscribe prefix";
    }
  }
  cb_.emplace_back(prefix, std::move(callback));
}

void AdnlLocalId::unsubscribe(std::string prefix) {
  bool deleted = false;
  for (auto it = cb_.begin(); it != cb_.end();) {
    if (it->first == prefix) {
      it = cb_.erase(it);
      deleted = true;
    } else {
      it++;
    }
  }
  LOG_CHECK(deleted) << this << ": cannot unsubscribe: prefix not found";
}

void AdnlLocalId::update_address_list(AdnlAddressList addr_list) {
  addr_list_ = std::move(addr_list);
  addr_list_.set_reinit_date(Adnl::adnl_start_time());
  addr_list_.set_version(static_cast<td::int32>(td::Clocks::system()));

  VLOG(ADNL_INFO) << this << ": updated addr list. New version set to " << addr_list_.version();

  publish_address_list();
}

void AdnlLocalId::publish_address_list() {
  if (dht_node_.empty() || addr_list_.empty() || (addr_list_.size() == 0 && !addr_list_.has_reverse())) {
    VLOG(ADNL_NOTICE) << this << ": skipping public addr list, because localid (or dht node) not fully initialized";
    return;
  }

  dht::DhtKey dht_key{short_id_.pubkey_hash(), "address", 0};
  auto dht_update_rule = dht::DhtUpdateRuleSignature::create().move_as_ok();
  dht::DhtKeyDescription dht_key_description{std::move(dht_key), id_.pubkey(), std::move(dht_update_rule),
                                             td::BufferSlice()};

  auto B = serialize_tl_object(dht_key_description.tl(), true);

  auto P = td::PromiseCreator::lambda([dht_node = dht_node_, SelfId = actor_id(this), addr_list = addr_list_.tl(),
                                       dht_key_description = std::move(dht_key_description),
                                       print_id = print_id()](td::Result<td::BufferSlice> R) mutable {
    if (R.is_error()) {
      LOG(ERROR) << print_id << ": cannot sign: " << R.move_as_error();
      return;
    }

    dht_key_description.update_signature(R.move_as_ok());
    dht_key_description.check().ensure();

    auto ttl = static_cast<td::uint32>(td::Clocks::system() + 3600);
    dht::DhtValue dht_value{std::move(dht_key_description), serialize_tl_object(addr_list, true), ttl,
                            td::BufferSlice("")};

    auto B = serialize_tl_object(dht_value.tl(), true);

    auto Q = td::PromiseCreator::lambda(
        [dht_node, dht_value = std::move(dht_value), print_id](td::Result<td::BufferSlice> R) mutable {
          if (R.is_error()) {
            LOG(ERROR) << print_id << ": cannot sign: " << R.move_as_error();
            return;
          }
          dht_value.update_signature(R.move_as_ok());
          dht_value.check().ensure();

          auto E = td::PromiseCreator::lambda([print_id](td::Result<td::Unit> R) {
            if (R.is_error()) {
              VLOG(ADNL_NOTICE) << print_id << ": failed to update addr list in DHT: " << R.move_as_error();
            } else {
              VLOG(ADNL_INFO) << print_id << ": updated dht addr list";
            }
          });

          td::actor::send_closure(dht_node, &dht::Dht::set_value, std::move(dht_value), std::move(E));
        });

    td::actor::send_closure(SelfId, &AdnlLocalId::sign_async, std::move(B), std::move(Q));
  });

  td::actor::send_closure(keyring_, &keyring::Keyring::sign_message, short_id_.pubkey_hash(), std::move(B),
                          std::move(P));

  if (addr_list_.has_reverse()) {
    td::actor::send_closure(
        dht_node_, &dht::Dht::register_reverse_connection, id_, [print_id = print_id()](td::Result<td::Unit> R) {
          if (R.is_error()) {
            VLOG(ADNL_NOTICE) << print_id << ": failed to register reverse connection in DHT: " << R.move_as_error();
          } else {
            VLOG(ADNL_INFO) << print_id << ": registered reverse connection";
          }
        });
  }
}

AdnlLocalId::AdnlLocalId(AdnlNodeIdFull id, AdnlAddressList addr_list, td::uint32 mode,
                         td::actor::ActorId<AdnlPeerTable> peer_table, td::actor::ActorId<keyring::Keyring> keyring,
                         td::actor::ActorId<dht::Dht> dht_node)
    : peer_table_(std::move(peer_table))
    , keyring_(std::move(keyring))
    , dht_node_(std::move(dht_node))
    , addr_list_(std::move(addr_list))
    , id_(std::move(id))
    , mode_(mode) {
  short_id_ = id_.compute_short_id();
  if (!addr_list_.empty()) {
    addr_list_.set_reinit_date(Adnl::adnl_start_time());
    addr_list_.set_version(static_cast<td::int32>(td::Clocks::system()));
  }

  VLOG(ADNL_INFO) << this << ": created local id " << short_id_;
}

void AdnlLocalId::get_self_node(td::Promise<AdnlNode> promise) {
  //addr_list_->version_ = static_cast<td::int32>(td::Clocks::system());
  promise.set_value(AdnlNode{id_, addr_list_});
}

void AdnlLocalId::decrypt_message(td::BufferSlice data, td::Promise<td::BufferSlice> promise) {
  td::actor::send_closure(keyring_, &keyring::Keyring::decrypt_message, short_id_.pubkey_hash(), std::move(data),
                          std::move(promise));
}

void AdnlLocalId::decrypt(td::BufferSlice data, td::Promise<AdnlPacket> promise) {
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), p = std::move(promise)](td::Result<td::BufferSlice> res) mutable {
        if (res.is_error()) {
          p.set_error(res.move_as_error());
        } else {
          td::actor::send_closure_later(SelfId, &AdnlLocalId::decrypt_continue, res.move_as_ok(), std::move(p));
        }
      });
  td::actor::send_closure(keyring_, &keyring::Keyring::decrypt_message, short_id_.pubkey_hash(), std::move(data),
                          std::move(P));
}

void AdnlLocalId::decrypt_continue(td::BufferSlice data, td::Promise<AdnlPacket> promise) {
  auto R = fetch_tl_object<ton_api::adnl_packetContents>(std::move(data), true);
  if (R.is_error()) {
    promise.set_error(R.move_as_error());
    return;
  }

  auto packetR = AdnlPacket::create(R.move_as_ok());
  if (packetR.is_error()) {
    promise.set_error(packetR.move_as_error());
    return;
  }
  promise.set_value(packetR.move_as_ok());
}

void AdnlLocalId::sign_async(td::BufferSlice data, td::Promise<td::BufferSlice> promise) {
  td::actor::send_closure(keyring_, &keyring::Keyring::sign_message, short_id_.pubkey_hash(), std::move(data),
                          std::move(promise));
}

void AdnlLocalId::sign_batch_async(std::vector<td::BufferSlice> data,
                                   td::Promise<std::vector<td::Result<td::BufferSlice>>> promise) {
  td::actor::send_closure(keyring_, &keyring::Keyring::sign_messages, short_id_.pubkey_hash(), std::move(data),
                          std::move(promise));
}

void AdnlLocalId::start_up() {
  publish_address_list();
  alarm_timestamp() = td::Timestamp::in(AdnlPeerTable::republish_addr_list_timeout() * td::Random::fast(1.0, 2.0));
}

void AdnlLocalId::alarm() {
  publish_address_list();
  alarm_timestamp() = td::Timestamp::in(AdnlPeerTable::republish_addr_list_timeout() * td::Random::fast(1.0, 2.0));
}

void AdnlLocalId::update_packet(AdnlPacket packet, bool update_id, bool sign, td::int32 update_addr_list_if,
                                td::int32 update_priority_addr_list_if, td::Promise<AdnlPacket> promise) {
  packet.init_random();
  if (update_id) {
    packet.set_source(id_);
  }
  if (!addr_list_.empty() && update_addr_list_if < addr_list_.version()) {
    packet.set_addr_list(addr_list_);
  }
  if (!sign) {
    promise.set_result(std::move(packet));
  } else {
    auto to_sign = packet.to_sign();
    auto P = td::PromiseCreator::lambda(
        [packet = std::move(packet), promise = std::move(promise)](td::Result<td::BufferSlice> R) mutable {
          if (R.is_error()) {
            promise.set_error(R.move_as_error());
          } else {
            packet.set_signature(R.move_as_ok());
            promise.set_value(std::move(packet));
          }
        });
    td::actor::send_closure(keyring_, &keyring::Keyring::sign_message, short_id_.pubkey_hash(), std::move(to_sign),
                            std::move(P));
  }
}

void AdnlLocalId::get_stats(bool all, td::Promise<tl_object_ptr<ton_api::adnl_stats_localId>> promise) {
  auto stats = create_tl_object<ton_api::adnl_stats_localId>();
  stats->short_id_ = short_id_.bits256_value();
  for (auto &[ip, x] : inbound_rate_limiter_) {
    if (x.currently_decrypting_packets != 0) {
      stats->current_decrypt_.push_back(create_tl_object<ton_api::adnl_stats_ipPackets>(
          ip.is_valid() ? PSTRING() << ip.get_ip_str() << ":" << ip.get_port() : "", x.currently_decrypting_packets));
    }
  }
  prepare_packet_stats();
  stats->packets_recent_ = packet_stats_prev_.tl();
  stats->packets_total_ = packet_stats_total_.tl(all);
  stats->packets_total_->ts_start_ = (double)Adnl::adnl_start_time();
  stats->packets_total_->ts_end_ = td::Clocks::system();
  promise.set_result(std::move(stats));
}

void AdnlLocalId::add_decrypted_packet_stats(td::IPAddress addr) {
  prepare_packet_stats();
  packet_stats_cur_.decrypted_packets[addr].inc();
  packet_stats_total_.decrypted_packets[addr].inc();
}

void AdnlLocalId::add_dropped_packet_stats(td::IPAddress addr) {
  prepare_packet_stats();
  packet_stats_cur_.dropped_packets[addr].inc();
  packet_stats_total_.dropped_packets[addr].inc();
}

void AdnlLocalId::prepare_packet_stats() {
  double now = td::Clocks::system();
  if (now >= packet_stats_cur_.ts_end) {
    packet_stats_prev_ = std::move(packet_stats_cur_);
    packet_stats_cur_ = {};
    auto now_int = (int)td::Clocks::system();
    packet_stats_cur_.ts_start = (double)(now_int / 60 * 60);
    packet_stats_cur_.ts_end = packet_stats_cur_.ts_start + 60.0;
    if (packet_stats_prev_.ts_end < now - 60.0) {
      packet_stats_prev_ = {};
      packet_stats_prev_.ts_end = packet_stats_cur_.ts_start;
      packet_stats_prev_.ts_start = packet_stats_prev_.ts_end - 60.0;
    }
  }
}

tl_object_ptr<ton_api::adnl_stats_localIdPackets> AdnlLocalId::PacketStats::tl(bool all) const {
  double threshold = all ? -1.0 : td::Clocks::system() - 600.0;
  auto obj = create_tl_object<ton_api::adnl_stats_localIdPackets>();
  obj->ts_start_ = ts_start;
  obj->ts_end_ = ts_end;
  for (const auto &[ip, packets] : decrypted_packets) {
    if (packets.last_packet_ts >= threshold) {
      obj->decrypted_packets_.push_back(create_tl_object<ton_api::adnl_stats_ipPackets>(
          ip.is_valid() ? PSTRING() << ip.get_ip_str() << ":" << ip.get_port() : "", packets.packets));
    }
  }
  for (const auto &[ip, packets] : dropped_packets) {
    if (packets.last_packet_ts >= threshold) {
      obj->dropped_packets_.push_back(create_tl_object<ton_api::adnl_stats_ipPackets>(
          ip.is_valid() ? PSTRING() << ip.get_ip_str() << ":" << ip.get_port() : "", packets.packets));
    }
  }
  return obj;
}


}  // namespace adnl

}  // namespace ton
