/*
    This file is part of TON Blockchain source code.

    TON Blockchain is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation; either version 2
    of the License, or (at your option) any later version.

    TON Blockchain is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with TON Blockchain.  If not, see <http://www.gnu.org/licenses/>.

    In addition, as a special exception, the copyright holders give permission
    to link the code of portions of this program with the OpenSSL library.
    You must obey the GNU General Public License in all respects for all
    of the code used other than OpenSSL. If you modify file(s) with this
    exception, you may extend this exception to your version of the file(s),
    but you are not obligated to do so. If you do not wish to do so, delete this
    exception statement from your version. If you delete this exception statement
    from all source files in the program, then also delete it here.

    Copyright 2017-2020 Telegram Systems LLP
*/
#pragma once

#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "adnl/adnl-sender-ex.h"
#include "overlay/overlay-manager.h"
#include "overlay/overlays.h"
#include "td/actor/actor.h"
#include "td/utils/buffer.h"

namespace ton::overlay::plumtree_sim {

struct SimGeoPoint {
  bool present = false;
  double lat = 0.0;
  double lon = 0.0;
};

struct DeliveryState {
  explicit DeliveryState(std::size_t node_count);

  td::Bits256 payload_hash;
  bool accept_any_payload_hash = false;
  std::vector<bool> delivered;
  std::vector<bool> expected;
  std::vector<td::Bits256> delivered_payload_hashes;
  std::vector<PublicKeyHash> delivered_sources;
  std::vector<double> delivery_ms;
  std::size_t remaining = 0;
  std::size_t expected_remaining = 0;
  double broadcast_start_time = 0.0;
  mutable std::mutex mutex;

  void start_broadcast(td::Bits256 hash, std::vector<bool> expected_nodes, double start_time,
                       bool accept_any_hash = false);
  bool mark_delivered(td::Bits256 hash, PublicKeyHash source, std::size_t node_index, double now);
  std::size_t expected_remaining_count() const;
  std::size_t delivered_count() const;
  std::vector<double> delivery_times() const;
  std::vector<bool> delivered_flags() const;
  std::vector<td::Bits256> delivered_hashes() const;
  std::vector<PublicKeyHash> delivered_source_hashes() const;
};

enum class SimEventKind { Message, Query, Response };

struct SimEvent {
  adnl::AdnlNodeIdShort src;
  adnl::AdnlNodeIdShort dst;
  td::BufferSlice data;
  SimEventKind kind = SimEventKind::Message;
  td::Promise<td::BufferSlice> promise;
  bool receive_queued = false;
  double last_byte_at = 0.0;
  std::size_t bytes = 0;
};

struct SimNetwork {
  double base_time = 0.0;
  double geo_alpha_ms = 3.554;
  double geo_beta_ms_per_km = 0.008963;
  double jitter = 0.1;
  double bandwidth_bytes_s = 100000000.0;
  td::actor::ActorId<OverlayManager> overlay_manager;
  std::map<adnl::AdnlNodeIdShort, std::size_t> node_by_adnl;
  std::vector<SimGeoPoint> geo_by_node;
  std::map<std::pair<double, td::uint64>, SimEvent> events;
  td::uint64 next_event_order = 0;
  td::uint32 random_state = 1;
  td::uint64 sent_bytes = 0;
  td::uint64 payload_deliveries = 0;
  td::uint64 prune_messages = 0;
  std::vector<td::uint64> sent_bytes_by_node;
  std::vector<td::uint64> received_bytes_by_node;
  std::vector<double> tx_free_at_by_node;
  std::vector<double> rx_free_at_by_node;
  mutable std::mutex mutex;

  double now_s() const;
  double propagation_latency_s(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst);
  void enqueue(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data);
  void enqueue_query(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data,
                     td::Promise<td::BufferSlice> promise);
  void enqueue_response(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data,
                        td::Promise<td::BufferSlice> promise);
  td::optional<double> next_event_time() const;
  std::vector<SimEvent> pop_due_events(double time_s);
  td::uint64 sent_bytes_count() const;
  td::uint64 payload_deliveries_count() const;
  td::uint64 prune_messages_count() const;
  std::vector<td::uint64> sent_bytes_by_node_snapshot() const;
  std::vector<td::uint64> received_bytes_by_node_snapshot() const;

 private:
  void enqueue_event(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data, SimEventKind kind,
                     td::Promise<td::BufferSlice> promise);
};

class SimulatedSender : public adnl::AdnlSenderEx {
 public:
  explicit SimulatedSender(std::shared_ptr<SimNetwork> network);

  void add_id(adnl::AdnlNodeIdShort local_id) override;
  void send_message(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data) override;
  void send_query(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, std::string name,
                  td::Promise<td::BufferSlice> promise, td::Timestamp timeout, td::BufferSlice data) override;
  void send_query_ex(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, std::string name,
                     td::Promise<td::BufferSlice> promise, td::Timestamp timeout, td::BufferSlice data,
                     td::uint64 max_answer_size) override;
  void get_conn_ip_str(adnl::AdnlNodeIdShort l_id, adnl::AdnlNodeIdShort p_id,
                       td::Promise<td::string> promise) override;

 private:
  std::shared_ptr<SimNetwork> network_;

  void on_mtu_updated(td::optional<adnl::AdnlNodeIdShort> local_id,
                      td::optional<adnl::AdnlNodeIdShort> peer_id) override;
};

class DeliveryCallback : public Overlays::Callback {
 public:
  DeliveryCallback(std::shared_ptr<DeliveryState> state, std::size_t node_index);

  void receive_broadcast(PublicKeyHash src, OverlayIdShort overlay_id, td::BufferSlice data) override;

 private:
  std::shared_ptr<DeliveryState> state_;
  std::size_t node_index_;
};

}  // namespace ton::overlay::plumtree_sim
