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

#include <vector>
#include <map>
#include <queue>

#include "adnl-peer.h"
#include "adnl-peer-table.h"
#include "adnl-network-manager.h"
#include "keys/encryptor.h"
#include "adnl-channel.h"
#include "adnl-query.h"

#include "crypto/Ed25519.h"
#include "td/utils/DecTree.h"

#include "utils.hpp"

namespace ton {

namespace adnl {

using AdnlConnectionIdShort = AdnlAddressImpl::Hash;

class AdnlPeerPairImpl : public AdnlPeerPair {
 public:
  static constexpr td::uint32 packet_header_max_size() {
    return 272;
  }
  static constexpr td::uint32 channel_packet_header_max_size() {
    return 128;
  }
  static constexpr td::uint32 addr_list_max_size() {
    return 128;
  }
  static constexpr td::uint32 get_mtu() {
    return Adnl::get_mtu() + 128;
  }
  static constexpr td::uint32 huge_packet_max_size() {
    return Adnl::huge_packet_max_size() + 128;
  }

  AdnlPeerPairImpl(td::actor::ActorId<AdnlNetworkManager> network_manager, td::actor::ActorId<AdnlPeerTable> peer_table,
                   td::uint32 local_mode, td::actor::ActorId<AdnlLocalId> local_actor,
                   td::actor::ActorId<dht::Dht> dht_node, AdnlNodeIdShort local_id,
                   AdnlNodeIdShort peer_id);
  void start_up() override;
  void alarm() override;

  void discover();

  void receive_packet_from_channel(AdnlChannelIdShort id, AdnlPacket packet, td::uint64 serialized_size) override;
  void receive_packet_checked(AdnlPacket packet) override;
  void receive_packet(AdnlPacket packet, td::uint64 serialized_size) override;
  void deliver_message(AdnlMessage message);

  void send_messages_from_queue();
  void send_messages(std::vector<OutboundAdnlMessage> messages) override;
  void send_packet_continue(AdnlPacket packet, td::actor::ActorId<AdnlNetworkConnection> conn, bool via_channel);
  void send_query(std::string name, td::Promise<td::BufferSlice> promise, td::Timestamp timeout, td::BufferSlice data,
                  td::uint32 flags) override;

  void alarm_query(AdnlQueryId id) override;

  void discover_query_result(td::Result<dht::DhtValue> B, bool dummy);

  void update_dht_node(td::actor::ActorId<dht::Dht> dht_node) override {
    dht_node_ = dht_node;
  }

  void update_addr_list(AdnlAddressList addr_list) override;
  void update_peer_id(AdnlNodeIdFull id) override;

  void get_conn_ip_str(td::Promise<td::string> promise) override;
  void get_stats(bool all, td::Promise<tl_object_ptr<ton_api::adnl_stats_peerPair>> promise) override;

  void got_data_from_db(td::Result<AdnlDbItem> R);
  void got_data_from_static_nodes(td::Result<AdnlNode> R);
  void got_data_from_dht(td::Result<AdnlNode> R);

  //void conn_ready(AdnlConnectionIdShort id, td::Result<td::actor::ActorOwn<AdnlNetworkConnection>> R);

  void process_message(const adnlmessage::AdnlMessageCreateChannel &message);
  void process_message(const adnlmessage::AdnlMessageConfirmChannel &message);
  void process_message(const adnlmessage::AdnlMessageCustom &message);
  void process_message(const adnlmessage::AdnlMessageNop &message);
  void process_message(const adnlmessage::AdnlMessageReinit &message);
  void process_message(const adnlmessage::AdnlMessageQuery &message);
  void process_message(const adnlmessage::AdnlMessageAnswer &message);
  void process_message(const adnlmessage::AdnlMessagePart &message);
  void process_message(const AdnlMessage::Empty &message) {
    UNREACHABLE();
  }

  void conn_change_state(AdnlConnectionIdShort conn_id, bool ready);

  void delete_query(AdnlQueryId id);

  struct PrintId {
    AdnlNodeIdShort peer_id;
    AdnlNodeIdShort local_id;
  };

  PrintId print_id() const {
    return PrintId{peer_id_short_, local_id_};
  }

 private:
  void respond_with_nop();
  void reinit(td::int32 date);
  td::Result<std::pair<td::actor::ActorId<AdnlNetworkConnection>, bool>> get_conn();
  void create_channel(pubkeys::Ed25519 pub, td::uint32 date);

  bool received_packet(td::uint64 seqno) const {
    CHECK(seqno > 0);
    if (seqno + 64 <= in_seqno_) {
      return true;
    }
    if (seqno > in_seqno_) {
      return false;
    }
    return recv_seqno_mask_ & (1ull << (in_seqno_ - seqno));
  }

  void add_received_packet(td::uint64 seqno) {
    CHECK(!received_packet(seqno));
    if (seqno <= in_seqno_) {
      recv_seqno_mask_ |= (1ull << (in_seqno_ - seqno));
    } else {
      auto old = in_seqno_;
      in_seqno_ = seqno;
      if (in_seqno_ - old >= 64) {
        recv_seqno_mask_ = 1;
      } else {
        recv_seqno_mask_ = recv_seqno_mask_ << (in_seqno_ - old);
        recv_seqno_mask_ |= 1;
      }
    }
  }

  void request_reverse_ping();
  void request_reverse_ping_result(td::Result<td::Unit> R);

  struct Conn {
    class ConnCallback : public AdnlNetworkConnection::Callback {
     public:
      void on_change_state(bool ready) override {
        td::actor::send_closure(root_, &AdnlPeerPairImpl::conn_change_state, conn_id_, ready);
      }
      ConnCallback(td::actor::ActorId<AdnlPeerPairImpl> root, AdnlConnectionIdShort conn_id)
          : root_(root), conn_id_(conn_id) {
      }

     private:
      td::actor::ActorId<AdnlPeerPairImpl> root_;
      AdnlConnectionIdShort conn_id_;
    };

    AdnlAddress addr;
    td::actor::ActorOwn<AdnlNetworkConnection> conn;

    Conn(AdnlAddress addr, td::actor::ActorId<AdnlPeerPairImpl> peer,
         td::actor::ActorId<AdnlNetworkManager> network_manager, td::actor::ActorId<Adnl> adnl)
        : addr(std::move(addr)) {
      create_conn(peer, network_manager, adnl);
    }
    Conn() {
    }

    bool ready() const {
      return !conn.empty() && conn.get_actor_unsafe().is_active();
    }

    bool is_direct() const {
      return addr->is_public();
    }

    void create_conn(td::actor::ActorId<AdnlPeerPairImpl> peer, td::actor::ActorId<AdnlNetworkManager> network_manager,
                     td::actor::ActorId<Adnl> adnl);
  };

  // Messages waiting for connection or for nochannel rate limiter
  std::queue<std::pair<OutboundAdnlMessage, td::Timestamp>> out_messages_queue_;
  td::uint64 out_messages_queue_total_size_ = 0;
  RateLimiter nochannel_rate_limiter_ = RateLimiter(50, 0.5);  // max 50, period = 0.5s
  td::Timestamp retry_send_at_ = td::Timestamp::never();
  bool disable_dht_query_ = false;
  bool skip_init_packet_ = false;
  double message_in_queue_ttl_ = 10.0;

  td::actor::ActorId<AdnlNetworkManager> network_manager_;
  td::actor::ActorId<AdnlPeerTable> peer_table_;
  td::actor::ActorId<AdnlLocalId> local_actor_;
  td::actor::ActorId<dht::Dht> dht_node_;

  td::uint32 priority_ = 0;

  td::int32 reinit_date_ = 0;

  bool channel_ready_ = false;
  bool channel_inited_ = false;
  AdnlChannelIdShort channel_in_id_;
  AdnlChannelIdShort channel_out_id_;
  privkeys::Ed25519 channel_pk_;
  pubkeys::Ed25519 channel_pub_;
  td::int32 channel_pk_date_;
  td::actor::ActorOwn<AdnlChannel> channel_;
  td::Timestamp respond_with_nop_after_;

  td::uint64 in_seqno_ = 0;
  td::uint64 out_seqno_ = 0;
  td::uint64 ack_seqno_ = 0;
  td::uint64 recv_seqno_mask_ = 0;

  td::uint32 peer_channel_date_ = 0;
  pubkeys::Ed25519 peer_channel_pub_;
  td::int32 peer_recv_addr_list_version_ = -1;
  td::int32 peer_recv_priority_addr_list_version_ = -1;

  td::Bits256 huge_message_hash_ = td::Bits256::zero();
  td::BufferSlice huge_message_;
  td::uint32 huge_message_offset_ = 0;

  AdnlAddressList addr_list_;
  AdnlAddressList priority_addr_list_;

  std::vector<Conn> conns_;
  std::vector<Conn> priority_conns_;

  AdnlNodeIdFull peer_id_;
  AdnlNodeIdShort peer_id_short_;
  AdnlNodeIdShort local_id_;

  std::unique_ptr<Encryptor> encryptor_;

  std::map<AdnlQueryId, td::actor::ActorId<AdnlQuery>> out_queries_;

  td::uint32 mode_;

  td::uint32 received_messages_ = 0;
  bool received_from_db_ = false;
  bool received_from_static_nodes_ = false;
  bool dht_query_active_ = false;

  td::Timestamp next_dht_query_at_ = td::Timestamp::never();
  td::Timestamp next_db_update_at_ = td::Timestamp::never();

  td::Timestamp last_received_packet_ = td::Timestamp::never();
  td::Timestamp try_reinit_at_ = td::Timestamp::never();
  td::Timestamp drop_addr_list_at_ = td::Timestamp::never();

  bool has_reverse_addr_ = false;
  td::Timestamp request_reverse_ping_after_ = td::Timestamp::now();
  bool request_reverse_ping_active_ = false;

  struct PacketStats {
    double ts_start = 0.0, ts_end = 0.0;
    td::uint64 in_packets = 0, in_bytes = 0, in_packets_channel = 0, in_bytes_channel = 0;
    td::uint64 out_packets = 0, out_bytes = 0, out_packets_channel = 0, out_bytes_channel = 0;
    td::uint64 out_expired_messages = 0, out_expired_bytes = 0;

    tl_object_ptr<ton_api::adnl_stats_packets> tl() const;
  } packet_stats_cur_, packet_stats_prev_, packet_stats_total_;
  double last_in_packet_ts_ = 0.0, last_out_packet_ts_ = 0.0;
  double started_ts_ = td::Clocks::system();
  void add_packet_stats(td::uint64 bytes, bool in, bool channel);
  void add_expired_msg_stats(td::uint64 bytes);
  void prepare_packet_stats();
};

}  // namespace adnl

}  // namespace ton

namespace td {

inline td::StringBuilder &operator<<(td::StringBuilder &sb, const ton::adnl::AdnlPeerPairImpl::PrintId &id) {
  sb << "[peerpair " << id.peer_id << "-" << id.local_id << "]";
  return sb;
}

inline td::StringBuilder &operator<<(td::StringBuilder &sb, const ton::adnl::AdnlPeerPairImpl &peer) {
  sb << peer.print_id();
  return sb;
}

inline td::StringBuilder &operator<<(td::StringBuilder &sb, const ton::adnl::AdnlPeerPairImpl *peer) {
  sb << peer->print_id();
  return sb;
}

}  // namespace td
