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
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

#include "auto/tl/ton_api.h"
#include "common/checksum.h"
#include "td/utils/Time.h"
#include "test/plumtree/transport.h"
#include "tl-utils/common-utils.hpp"

namespace ton::overlay::plumtree_sim {
namespace {

constexpr double GEO_FALLBACK_LATENCY_MS = 75.0;

double to_radians(double degrees) {
  return degrees * 3.14159265358979323846 / 180.0;
}

double haversine_km(const SimGeoPoint &from, const SimGeoPoint &to) {
  static constexpr double RADIUS_KM = 6371.0088;
  auto from_lat = to_radians(from.lat);
  auto to_lat = to_radians(to.lat);
  auto delta_lat = to_radians(to.lat - from.lat);
  auto delta_lon = to_radians(to.lon - from.lon);
  auto sin_lat = std::sin(delta_lat / 2.0);
  auto sin_lon = std::sin(delta_lon / 2.0);
  auto a = sin_lat * sin_lat + std::cos(from_lat) * std::cos(to_lat) * sin_lon * sin_lon;
  return 2.0 * RADIUS_KM * std::asin(std::sqrt(a));
}

std::int32_t tl_constructor_id(td::Slice data) {
  if (data.size() < 4) {
    return 0;
  }
  auto ptr = data.ubegin();
  auto id = static_cast<td::uint32>(ptr[0]) | (static_cast<td::uint32>(ptr[1]) << 8) |
            (static_cast<td::uint32>(ptr[2]) << 16) | (static_cast<td::uint32>(ptr[3]) << 24);
  return static_cast<std::int32_t>(id);
}

std::int32_t overlay_payload_constructor_id(td::Slice data) {
  td::Slice payload = data;
  if (fetch_tl_prefix<ton_api::overlay_messageWithExtra>(payload, true).is_ok()) {
    return tl_constructor_id(payload);
  }
  payload = data;
  if (fetch_tl_prefix<ton_api::overlay_message>(payload, true).is_ok()) {
    return tl_constructor_id(payload);
  }
  return tl_constructor_id(data);
}

}  // namespace

DeliveryState::DeliveryState(std::size_t node_count)
    : delivered(node_count, false)
    , delivered_payload_hashes(node_count)
    , delivered_sources(node_count)
    , delivery_ms(node_count, -1.0) {
}

void DeliveryState::start_broadcast(td::Bits256 hash, std::vector<bool> expected_nodes, double start_time,
                                    bool accept_any_hash) {
  std::lock_guard<std::mutex> lock(mutex);
  payload_hash = hash;
  accept_any_payload_hash = accept_any_hash;
  expected = std::move(expected_nodes);
  delivered.assign(delivered.size(), false);
  delivered_payload_hashes.assign(delivered_payload_hashes.size(), {});
  delivered_sources.assign(delivered_sources.size(), {});
  delivery_ms.assign(delivery_ms.size(), -1.0);
  remaining = delivered.size();
  expected_remaining = 0;
  for (bool node_expected : expected) {
    if (node_expected) {
      ++expected_remaining;
    }
  }
  broadcast_start_time = start_time;
}

bool DeliveryState::mark_delivered(td::Bits256 hash, PublicKeyHash source, std::size_t node_index, double now) {
  std::lock_guard<std::mutex> lock(mutex);
  if (!accept_any_payload_hash && hash != payload_hash) {
    return false;
  }
  if (delivered[node_index]) {
    return false;
  }
  delivered[node_index] = true;
  delivered_payload_hashes[node_index] = hash;
  delivered_sources[node_index] = source;
  delivery_ms[node_index] = (now - broadcast_start_time) * 1000.0;
  CHECK(remaining > 0);
  --remaining;
  if (node_index < expected.size() && expected[node_index]) {
    CHECK(expected_remaining > 0);
    --expected_remaining;
  }
  return true;
}

std::size_t DeliveryState::expected_remaining_count() const {
  std::lock_guard<std::mutex> lock(mutex);
  return expected_remaining;
}

std::size_t DeliveryState::delivered_count() const {
  std::lock_guard<std::mutex> lock(mutex);
  return delivered.size() - remaining;
}

std::vector<double> DeliveryState::delivery_times() const {
  std::lock_guard<std::mutex> lock(mutex);
  return delivery_ms;
}

std::vector<bool> DeliveryState::delivered_flags() const {
  std::lock_guard<std::mutex> lock(mutex);
  return delivered;
}

std::vector<td::Bits256> DeliveryState::delivered_hashes() const {
  std::lock_guard<std::mutex> lock(mutex);
  return delivered_payload_hashes;
}

std::vector<PublicKeyHash> DeliveryState::delivered_source_hashes() const {
  std::lock_guard<std::mutex> lock(mutex);
  return delivered_sources;
}

double SimNetwork::now_s() const {
  return std::max(0.0, td::Time::now() - base_time);
}

double SimNetwork::propagation_latency_s(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst) {
  double latency_ms = GEO_FALLBACK_LATENCY_MS;
  auto src_it = node_by_adnl.find(src);
  auto dst_it = node_by_adnl.find(dst);
  if (src_it != node_by_adnl.end() && dst_it != node_by_adnl.end() && src_it->second < geo_by_node.size() &&
      dst_it->second < geo_by_node.size()) {
    const auto &from_geo = geo_by_node[src_it->second];
    const auto &to_geo = geo_by_node[dst_it->second];
    if (from_geo.present && to_geo.present) {
      latency_ms = geo_alpha_ms + geo_beta_ms_per_km * haversine_km(from_geo, to_geo);
    }
  }
  latency_ms = std::max(0.0, latency_ms);
  auto jitter_value = std::max(0.0, jitter);
  if (jitter_value > 0.0) {
    random_state += 0x6d2b79f5u;
    td::uint32 value = random_state;
    value = (value ^ (value >> 15)) * (value | 1u);
    value ^= value + ((value ^ (value >> 7)) * (value | 61u));
    auto random = static_cast<double>(value ^ (value >> 14)) / 4294967296.0;
    latency_ms *= 1.0 + (random * 2.0 - 1.0) * jitter_value;
  }
  return std::max(0.0, latency_ms) / 1000.0;
}

namespace {

void set_response_result(td::Promise<td::BufferSlice> promise, td::Result<td::BufferSlice> result,
                         td::uint64 max_answer_size) {
  if (result.is_ok() && result.ok().size() > max_answer_size) {
    promise.set_error(td::Status::Error("simulated ADNL query response exceeds max answer size"));
    return;
  }
  promise.set_result(std::move(result));
}

}  // namespace

void SimNetwork::enqueue(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data) {
  enqueue_event(src, dst, std::move(data), SimEventKind::Message, {});
}

void SimNetwork::enqueue_query(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data,
                               td::Promise<td::BufferSlice> promise) {
  enqueue_event(src, dst, std::move(data), SimEventKind::Query, std::move(promise));
}

void SimNetwork::enqueue_response(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data,
                                  td::Promise<td::BufferSlice> promise) {
  enqueue_event(src, dst, std::move(data), SimEventKind::Response, std::move(promise));
}

void SimNetwork::enqueue_event(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data,
                               SimEventKind kind, td::Promise<td::BufferSlice> promise) {
  std::lock_guard<std::mutex> lock(mutex);
  auto bytes = data.size();
  auto type = overlay_payload_constructor_id(data.as_slice());
  auto now = now_s();
  sent_bytes += bytes;
  if (type == ton_api::overlay_broadcastPlumtreePrune::ID) {
    ++prune_messages;
  }
  auto src_it = node_by_adnl.find(src);
  if (src_it != node_by_adnl.end() && src_it->second < sent_bytes_by_node.size()) {
    sent_bytes_by_node[src_it->second] += bytes;
  }
  auto tx_start = now;
  auto tx_delay = static_cast<double>(bytes) / std::max(1.0, bandwidth_bytes_s);
  if (src_it != node_by_adnl.end() && src_it->second < tx_free_at_by_node.size()) {
    tx_start = std::max(tx_start, tx_free_at_by_node[src_it->second]);
    tx_free_at_by_node[src_it->second] = tx_start + tx_delay;
  }
  auto propagation = propagation_latency_s(src, dst);
  auto first_byte_at = tx_start + propagation;
  auto last_byte_at = tx_start + tx_delay + propagation;
  events.emplace(std::pair<double, td::uint64>{first_byte_at, next_event_order++},
                 SimEvent{src, dst, std::move(data), kind, std::move(promise), true, last_byte_at, bytes});
}

td::optional<double> SimNetwork::next_event_time() const {
  std::lock_guard<std::mutex> lock(mutex);
  if (events.empty()) {
    return {};
  }
  return events.begin()->first.first;
}

std::vector<SimEvent> SimNetwork::pop_due_events(double time_s) {
  std::vector<SimEvent> due_events;
  std::lock_guard<std::mutex> lock(mutex);
  while (!events.empty() && events.begin()->first.first <= time_s + 1e-12) {
    auto node = events.extract(events.begin());
    auto event_at = node.key().first;
    auto event = std::move(node.mapped());
    if (event.receive_queued) {
      auto deliver_at = event.last_byte_at;
      auto dst_it = node_by_adnl.find(event.dst);
      if (dst_it != node_by_adnl.end() && dst_it->second < rx_free_at_by_node.size()) {
        auto rx_delay = static_cast<double>(event.bytes) / std::max(1.0, bandwidth_bytes_s);
        auto rx_start = std::max(event_at, rx_free_at_by_node[dst_it->second]);
        deliver_at = std::max(event.last_byte_at, rx_start + rx_delay);
        rx_free_at_by_node[dst_it->second] = deliver_at;
      }
      event.receive_queued = false;
      if (deliver_at > time_s + 1e-12) {
        events.emplace(std::pair<double, td::uint64>{deliver_at, next_event_order++}, std::move(event));
        continue;
      }
    }
    auto payload_type = overlay_payload_constructor_id(event.data.as_slice());
    if (payload_type == ton_api::overlay_broadcastPlumtreeFec::ID ||
        payload_type == ton_api::overlay_broadcastPlumtreeSimple::ID) {
      ++payload_deliveries;
    }
    auto dst_it = node_by_adnl.find(event.dst);
    if (dst_it != node_by_adnl.end() && dst_it->second < received_bytes_by_node.size()) {
      received_bytes_by_node[dst_it->second] += event.data.size();
    }
    due_events.push_back(std::move(event));
  }
  return due_events;
}

td::uint64 SimNetwork::sent_bytes_count() const {
  std::lock_guard<std::mutex> lock(mutex);
  return sent_bytes;
}

td::uint64 SimNetwork::payload_deliveries_count() const {
  std::lock_guard<std::mutex> lock(mutex);
  return payload_deliveries;
}

td::uint64 SimNetwork::prune_messages_count() const {
  std::lock_guard<std::mutex> lock(mutex);
  return prune_messages;
}

std::vector<td::uint64> SimNetwork::sent_bytes_by_node_snapshot() const {
  std::lock_guard<std::mutex> lock(mutex);
  return sent_bytes_by_node;
}

std::vector<td::uint64> SimNetwork::received_bytes_by_node_snapshot() const {
  std::lock_guard<std::mutex> lock(mutex);
  return received_bytes_by_node;
}

SimulatedSender::SimulatedSender(std::shared_ptr<SimNetwork> network)
    : adnl::AdnlSenderEx(40 << 10), network_(std::move(network)) {
}

void SimulatedSender::add_id(adnl::AdnlNodeIdShort local_id) {
}

void SimulatedSender::send_message(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data) {
  network_->enqueue(src, dst, std::move(data));
}

void SimulatedSender::send_query(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, std::string name,
                                 td::Promise<td::BufferSlice> promise, td::Timestamp timeout, td::BufferSlice data) {
  send_query_ex(src, dst, std::move(name), std::move(promise), timeout, std::move(data), td::uint64(-1));
}

void SimulatedSender::send_query_ex(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, std::string name,
                                    td::Promise<td::BufferSlice> promise, td::Timestamp timeout, td::BufferSlice data,
                                    td::uint64 max_answer_size) {
  auto response_promise =
      td::PromiseCreator::lambda([network = network_, src, dst, max_answer_size,
                                  promise = std::move(promise)](td::Result<td::BufferSlice> result) mutable {
        if (result.is_error()) {
          set_response_result(std::move(promise), std::move(result), max_answer_size);
          return;
        }
        auto response = result.move_as_ok();
        network->enqueue_response(dst, src, std::move(response),
                                  td::PromiseCreator::lambda([promise = std::move(promise), max_answer_size](
                                                                 td::Result<td::BufferSlice> delivered) mutable {
                                    set_response_result(std::move(promise), std::move(delivered), max_answer_size);
                                  }));
      });
  network_->enqueue_query(src, dst, std::move(data), std::move(response_promise));
}

void SimulatedSender::get_conn_ip_str(adnl::AdnlNodeIdShort l_id, adnl::AdnlNodeIdShort p_id,
                                      td::Promise<td::string> promise) {
  promise.set_value("");
}

void SimulatedSender::on_mtu_updated(td::optional<adnl::AdnlNodeIdShort> local_id,
                                     td::optional<adnl::AdnlNodeIdShort> peer_id) {
}

DeliveryCallback::DeliveryCallback(std::shared_ptr<DeliveryState> state, std::size_t node_index)
    : state_(std::move(state)), node_index_(node_index) {
}

void DeliveryCallback::receive_broadcast(PublicKeyHash src, OverlayIdShort overlay_id, td::BufferSlice data) {
  state_->mark_delivered(td::sha256_bits256(data.as_slice()), src, node_index_, td::Time::now());
}

}  // namespace ton::overlay::plumtree_sim
