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

#include <map>

#include "td/actor/actor.h"
#include "td/utils/BufferedUdp.h"
#include "auto/tl/ton_api.h"
#include "keys/encryptor.h"
#include "adnl-peer-table.h"
#include "dht/dht.h"

#include "adnl-peer-table.h"
#include "utils.hpp"

namespace ton {

namespace adnl {

class AdnlLocalId : public td::actor::Actor {
 public:
  AdnlNodeIdFull get_id() const;
  AdnlNodeIdShort get_short_id() const;
  AdnlAddressList get_addr_list() const;
  void get_addr_list_async(td::Promise<AdnlAddressList> P) {
    P.set_value(get_addr_list());
  }

  void update_dht_node(td::actor::ActorId<dht::Dht> dht_node) {
    dht_node_ = dht_node;

    publish_address_list();
  }

  void decrypt(td::BufferSlice data, td::Promise<AdnlPacket> promise);
  void decrypt_continue(td::BufferSlice data, td::Promise<AdnlPacket> promise);
  void decrypt_message(td::BufferSlice data, td::Promise<td::BufferSlice> promise);
  void deliver(AdnlNodeIdShort src, td::BufferSlice data);
  void deliver_query(AdnlNodeIdShort src, td::BufferSlice data, td::Promise<td::BufferSlice> promise);
  void receive(td::IPAddress addr, td::BufferSlice data);
  void decrypt_packet_done(td::IPAddress addr);

  void subscribe(std::string prefix, std::unique_ptr<AdnlPeerTable::Callback> callback);
  void unsubscribe(std::string prefix);

  void update_address_list(AdnlAddressList addr_list);

  void get_self_node(td::Promise<AdnlNode> promise);

  void sign_async(td::BufferSlice data, td::Promise<td::BufferSlice> promise);
  void sign_batch_async(std::vector<td::BufferSlice> data,
                        td::Promise<std::vector<td::Result<td::BufferSlice>>> promise);

  AdnlLocalId(AdnlNodeIdFull id, AdnlAddressList addr_list, td::uint32 mode,
              td::actor::ActorId<AdnlPeerTable> peer_table, td::actor::ActorId<keyring::Keyring> keyring,
              td::actor::ActorId<dht::Dht> dht_node);

  void start_up() override;
  void alarm() override;

  void update_packet(AdnlPacket packet, bool update_id, bool sign, td::int32 update_addr_list_if,
                     td::int32 update_priority_addr_list_if, td::Promise<AdnlPacket> promise);

  void get_stats(bool all, td::Promise<tl_object_ptr<ton_api::adnl_stats_localId>> promise);

  td::uint32 get_mode() {
    return mode_;
  }

  struct PrintId {
    AdnlNodeIdShort id;
  };

  PrintId print_id() const {
    return PrintId{short_id_};
  }

 private:
  td::actor::ActorId<AdnlPeerTable> peer_table_;
  td::actor::ActorId<keyring::Keyring> keyring_;
  td::actor::ActorId<dht::Dht> dht_node_;
  std::vector<std::pair<std::string, std::unique_ptr<AdnlPeerTable::Callback>>> cb_;

  AdnlAddressList addr_list_;
  AdnlNodeIdFull id_;
  AdnlNodeIdShort short_id_;

  td::uint32 mode_;

  struct InboundRateLimiter {
    RateLimiter rate_limiter = RateLimiter(75, 0.33);
    td::uint64 currently_decrypting_packets = 0;
  };
  std::map<td::IPAddress, InboundRateLimiter> inbound_rate_limiter_;
  struct PacketStats {
    double ts_start = 0.0, ts_end = 0.0;

    struct Counter {
      td::uint64 packets = 0;
      double last_packet_ts = 0.0;

      void inc() {
        ++packets;
        last_packet_ts = td::Clocks::system();
      }
    };
    std::map<td::IPAddress, Counter> decrypted_packets;
    std::map<td::IPAddress, Counter> dropped_packets;

    tl_object_ptr<ton_api::adnl_stats_localIdPackets> tl(bool all = true) const;
  } packet_stats_cur_, packet_stats_prev_, packet_stats_total_;
  void add_decrypted_packet_stats(td::IPAddress addr);
  void add_dropped_packet_stats(td::IPAddress addr);
  void prepare_packet_stats();

  void publish_address_list();
};

inline td::StringBuilder &operator<<(td::StringBuilder &sb, const AdnlLocalId::PrintId &id) {
  sb << "[localid " << id.id << "]";
  return sb;
}

inline td::StringBuilder &operator<<(td::StringBuilder &sb, const AdnlLocalId &localid) {
  sb << localid.print_id();
  return sb;
}

inline td::StringBuilder &operator<<(td::StringBuilder &sb, const AdnlLocalId *localid) {
  sb << localid->print_id();
  return sb;
}

}  // namespace adnl

}  // namespace ton
