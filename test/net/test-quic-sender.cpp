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
*/
#include <array>
#include <atomic>
#include <mutex>
#include <optional>

#include "adnl/adnl-network-manager.h"
#include "adnl/adnl-node-id.hpp"
#include "adnl/adnl-peer-table.h"
#include "adnl/adnl.h"
#include "adnl/utils.hpp"
#include "auto/tl/ton_api.hpp"
#include "keyring/keyring.h"
#include "keys/keys.hpp"
#include "quic/quic-pimpl.h"
#include "quic/quic-sender.h"
#include "quic/quic-server.h"
#include "td/actor/coro_task.h"
#include "td/actor/coro_utils.h"
#include "td/utils/HashSet.h"
#include "td/utils/OptionParser.h"
#include "td/utils/Random.h"
#include "td/utils/Time.h"
#include "td/utils/crypto.h"
#include "td/utils/format.h"
#include "td/utils/port/path.h"
#include "td/utils/port/signals.h"
#include "td/utils/port/sleep.h"
#include "td/utils/tests.h"
#include "tl-utils/common-utils.hpp"

namespace ton::quic {

struct QuicSenderTest : QuicSender {
  QuicSenderTest() : QuicSender({}, {}) {
  }

  td::actor::Task<> expired_waiter_ownership() {
    auto& sender = *this;
    for (bool started : {false, true}) {
      auto connection = sender.get_or_create_connection({});
      connection->init_started = started;
      // Model a deadline crossing after the query's first check and connection lookup.
      auto result = co_await sender.wait_connection_ready(connection, td::Timestamp::in(-1)).wrap();
      ASSERT_TRUE(result.is_error());
      ASSERT_EQ(ErrorCode::timeout, result.error().code());
      ASSERT_EQ(started ? 1u : 0u, sender.outbound_.size());
      ASSERT_TRUE(sender.waiter_timeouts_.empty());
      ASSERT_TRUE(connection->waiting_ready.empty());
      if (started) {
        ASSERT_TRUE(sender.outbound_.begin()->second == connection);
      }
    }

    sender.outbound_.clear();
    bool expired = false;
    Waiter::Queue waiting;
    auto waiter = waiting.emplace(waiting.end(), waiting, td::make_promise([&](td::Result<td::Unit> result) {
                                    ASSERT_TRUE(result.is_error());
                                    // Callbacks must finish before a new, unstarted connection is published.
                                    ASSERT_TRUE(sender.outbound_.empty());
                                    expired = true;
                                  }));
    waiter->position = waiter;
    sender.waiter_timeouts_.insert(td::Timestamp::in(-1), &*waiter);
    auto connection = sender.get_or_create_connection({});
    ASSERT_TRUE(expired);
    ASSERT_TRUE(waiting.empty());
    auto result = co_await sender.wait_connection_ready(connection, td::Timestamp::in(-1)).wrap();
    ASSERT_TRUE(result.is_error());
    ASSERT_TRUE(sender.outbound_.empty());
    co_return {};
  }
};

}  // namespace ton::quic

namespace {

struct Config {
  std::atomic<int> port_counter{21000};
  int threads{4};
  double timeout{60.0};
  int large_nodes{5};
  int large_queries{200};
  int large_size{131072};
};

Config g_config;

int next_port() {
  return g_config.port_counter.fetch_add(1);
}

ton::PrivateKey make_key(int seed) {
  td::Bits256 hash;
  auto seed_str = std::to_string(seed);
  td::sha256(td::Slice(seed_str), hash.as_slice());
  return ton::PrivateKey{ton::privkeys::Ed25519{hash}};
}

ton::adnl::AdnlAddressList make_addr_list(td::Slice ip_str, int port) {
  td::IPAddress ip;
  ip.init_host_port(PSTRING() << ip_str << ":" << port).ensure();
  ton::adnl::AdnlAddressList list;
  list.add_udp_adnl_address(ip).ensure();
  list.set_version(static_cast<td::int32>(td::Clocks::system()));
  list.set_reinit_date(ton::adnl::Adnl::adnl_start_time());
  return list;
}

ton::quic::QuicServer::Options quic_test_options() {
  return ton::quic::QuicServer::Options{};
}

// Disable per-IP admission (live cap + new-connection rate) so tests can open many connections from
// 127.0.0.1 without tripping flood control; every other limit a test sets is preserved.
ton::quic::FloodLimits quic_test_flood_limits(ton::quic::FloodLimits limits = {}) {
  limits.max_conns_per_ip.reset();
  limits.conn_rate_capacity = 0;
  return limits;
}

class ReceivedMessages {
 public:
  void push(td::BufferSlice message) {
    std::lock_guard guard(mutex_);
    messages_.push_back(std::move(message));
  }

  size_t size() const {
    std::lock_guard guard(mutex_);
    return messages_.size();
  }

  bool empty() const {
    return size() == 0;
  }

  td::BufferSlice get(size_t index) const {
    std::lock_guard guard(mutex_);
    return messages_.at(index).clone();
  }

 private:
  mutable std::mutex mutex_;
  std::vector<td::BufferSlice> messages_;
};

class EchoCallback : public ton::adnl::Adnl::Callback {
 public:
  std::shared_ptr<ReceivedMessages> received_messages;

  explicit EchoCallback(std::shared_ptr<ReceivedMessages> msgs = nullptr) : received_messages(std::move(msgs)) {
  }

  void receive_message(ton::adnl::AdnlNodeIdShort, ton::adnl::AdnlNodeIdShort, td::BufferSlice data) override {
    LOG(ERROR) << "receive message message";
    if (received_messages) {
      received_messages->push(std::move(data));
    }
  }

  void receive_query(ton::adnl::AdnlNodeIdShort, ton::adnl::AdnlNodeIdShort, td::BufferSlice data,
                     td::Promise<td::BufferSlice> promise) override {
    promise.set_value(std::move(data));
  }
};

class DelayedResponse : public td::actor::Actor {
 public:
  DelayedResponse(double delay, td::BufferSlice data, td::Promise<td::BufferSlice> promise)
      : delay_(delay), data_(std::move(data)), promise_(std::move(promise)) {
  }

  void start_up() override {
    alarm_timestamp() = td::Timestamp::in(delay_);
  }

  void alarm() override {
    promise_.set_value(std::move(data_));
    stop();
  }

 private:
  double delay_;
  td::BufferSlice data_;
  td::Promise<td::BufferSlice> promise_;
};

class SlowEchoCallback : public ton::adnl::Adnl::Callback {
 public:
  double delay_seconds;

  explicit SlowEchoCallback(double delay) : delay_seconds(delay) {
  }

  void receive_message(ton::adnl::AdnlNodeIdShort, ton::adnl::AdnlNodeIdShort, td::BufferSlice) override {
  }

  void receive_query(ton::adnl::AdnlNodeIdShort, ton::adnl::AdnlNodeIdShort, td::BufferSlice data,
                     td::Promise<td::BufferSlice> promise) override {
    td::actor::create_actor<DelayedResponse>("delay", delay_seconds, std::move(data), std::move(promise)).release();
  }
};

// Holds a message-delivery ack for `delay` seconds, so the message's in-flight charge stays held.
class DelayedAck : public td::actor::Actor {
 public:
  DelayedAck(double delay, td::Promise<td::Unit> promise) : delay_(delay), promise_(std::move(promise)) {
  }
  void start_up() override {
    alarm_timestamp() = td::Timestamp::in(delay_);
  }
  void alarm() override {
    promise_.set_value(td::Unit{});
    stop();
  }

 private:
  double delay_;
  td::Promise<td::Unit> promise_;
};

class SlowMessageCallback : public ton::adnl::Adnl::Callback {
 public:
  explicit SlowMessageCallback(double delay) : delay_(delay) {
  }
  void receive_message(ton::adnl::AdnlNodeIdShort, ton::adnl::AdnlNodeIdShort, td::BufferSlice,
                       td::Promise<td::Unit> promise) override {
    td::actor::create_actor<DelayedAck>("ack", delay_, std::move(promise)).release();
  }

 private:
  double delay_;
};

class LimitedEchoCallback : public ton::adnl::Adnl::Callback {
 public:
  td::uint64 max_query_size;

  explicit LimitedEchoCallback(td::uint64 limit) : max_query_size(limit) {
  }

  void receive_message(ton::adnl::AdnlNodeIdShort, ton::adnl::AdnlNodeIdShort, td::BufferSlice) override {
  }

  void receive_query(ton::adnl::AdnlNodeIdShort, ton::adnl::AdnlNodeIdShort, td::BufferSlice data,
                     td::Promise<td::BufferSlice> promise) override {
    if (data.size() > max_query_size) {
      promise.set_error(td::Status::Error("query too large"));
      return;
    }
    promise.set_value(std::move(data));
  }
};

class NeverRespondCallback : public ton::adnl::Adnl::Callback {
 public:
  std::vector<td::Promise<td::BufferSlice>> pending_;

  void receive_message(ton::adnl::AdnlNodeIdShort, ton::adnl::AdnlNodeIdShort, td::BufferSlice) override {
  }

  void receive_query(ton::adnl::AdnlNodeIdShort, ton::adnl::AdnlNodeIdShort, td::BufferSlice data,
                     td::Promise<td::BufferSlice> promise) override {
    pending_.push_back(std::move(promise));
  }
};

class StaticResponseCallback : public ton::adnl::Adnl::Callback {
 public:
  explicit StaticResponseCallback(std::string response) : response_(std::move(response)) {
  }

  void receive_message(ton::adnl::AdnlNodeIdShort, ton::adnl::AdnlNodeIdShort, td::BufferSlice) override {
  }

  void receive_query(ton::adnl::AdnlNodeIdShort, ton::adnl::AdnlNodeIdShort, td::BufferSlice,
                     td::Promise<td::BufferSlice> promise) override {
    promise.set_value(td::BufferSlice(td::Slice(response_)));
  }

 private:
  std::string response_;
};

struct TestNode {
  ton::adnl::AdnlNodeIdShort id;
  ton::PrivateKey key;
  std::string ip{"127.0.0.1"};
  int port{0};

  td::actor::ActorOwn<ton::keyring::Keyring> keyring;
  td::actor::ActorOwn<ton::adnl::AdnlNetworkManager> network_manager;
  td::actor::ActorOwn<ton::adnl::Adnl> adnl;
  td::actor::ActorOwn<ton::quic::QuicSender> quic_sender;
  std::shared_ptr<ReceivedMessages> received_messages;

  TestNode() = default;
  TestNode(TestNode&&) = default;
  TestNode& operator=(TestNode&&) = default;
};

void assert_timeout(const td::Result<td::BufferSlice>& result) {
  ASSERT_TRUE(result.is_error());
  ASSERT_EQ(ton::ErrorCode::timeout, result.error().code());
}

td::Ed25519::PrivateKey make_quic_key(int seed) {
  td::Bits256 hash;
  auto seed_str = std::to_string(seed);
  td::sha256(td::Slice(seed_str), hash.as_slice());
  return td::Ed25519::PrivateKey(td::SecureString(hash.as_slice()));
}

td::Ed25519::PrivateKey clone_quic_key(const td::Ed25519::PrivateKey& key) {
  return td::Ed25519::PrivateKey(key.as_octet_string());
}

struct RawQuicEndpointState {
  void remember_connection(ton::quic::QuicConnectionId cid, bool is_outbound, ton::adnl::AdnlNodeIdShort local_id) {
    std::lock_guard guard(mutex);
    if (is_outbound) {
      outbound_cid = cid;
      outbound_local_id = local_id;
    } else {
      inbound_cid = cid;
      inbound_local_id = local_id;
    }
  }

  void remember_closed_connection(ton::quic::QuicConnectionId cid) {
    std::lock_guard guard(mutex);
    closed_connections.insert(cid);
  }

  std::optional<ton::adnl::AdnlNodeIdShort> get_inbound_local_id() const {
    std::lock_guard guard(mutex);
    return inbound_local_id;
  }

  std::optional<ton::adnl::AdnlNodeIdShort> get_outbound_local_id() const {
    std::lock_guard guard(mutex);
    return outbound_local_id;
  }

  void remember_local_stream(ton::quic::QuicStreamID sid) {
    std::lock_guard guard(mutex);
    locally_opened_streams.insert(sid);
  }

  bool is_local_stream(ton::quic::QuicStreamID sid) const {
    std::lock_guard guard(mutex);
    return locally_opened_streams.contains(sid);
  }

  void remember_closed_stream(ton::quic::StreamCloseEvent event) {
    std::lock_guard guard(mutex);
    locally_opened_streams.erase(event.sid);
    closed_streams.insert(event.sid);
    closed_stream_events_[event.sid] = event;
    closed_stream_count++;
    if (event.initiator == ton::quic::StreamInitiator::Local) {
      local_closed_stream_count++;
    }
  }

  bool has_closed_stream(ton::quic::QuicStreamID sid) const {
    std::lock_guard guard(mutex);
    return closed_streams.contains(sid);
  }

  size_t get_local_closed_stream_count() const {
    std::lock_guard guard(mutex);
    return local_closed_stream_count;
  }

  std::optional<ton::quic::StreamCloseEvent> get_close_event(ton::quic::QuicStreamID sid) const {
    std::lock_guard guard(mutex);
    auto it = closed_stream_events_.find(sid);
    if (it == closed_stream_events_.end()) {
      return std::nullopt;
    }
    return it->second;
  }

  bool has_closed_connection(ton::quic::QuicConnectionId cid) const {
    std::lock_guard guard(mutex);
    return closed_connections.contains(cid);
  }

  // Stands in for an upper layer that is slow to dispatch what arrived: closed peer streams pile up
  // here instead of having their credit released, and the test drains them when it chooses to.
  void hold_peer_streams() {
    std::lock_guard guard(mutex);
    hold_peer_streams_ = true;
  }

  bool hold_stream(ton::quic::QuicConnectionId cid) {
    std::lock_guard guard(mutex);
    if (!hold_peer_streams_) {
      return false;
    }
    held_streams_.push_back(cid);
    return true;
  }

  std::vector<ton::quic::QuicConnectionId> take_held_streams() {
    std::lock_guard guard(mutex);
    hold_peer_streams_ = false;
    return std::move(held_streams_);
  }

  size_t get_held_stream_count() const {
    std::lock_guard guard(mutex);
    return held_streams_.size();
  }

  void remember_rejected_stream() {
    std::lock_guard guard(mutex);
    rejected_streams_++;
  }

  size_t get_rejected_streams() const {
    std::lock_guard guard(mutex);
    return rejected_streams_;
  }

  void remember_stream_payload() {
    std::lock_guard guard(mutex);
    delivered_payloads_++;
  }

  size_t get_delivered_payloads() const {
    std::lock_guard guard(mutex);
    return delivered_payloads_;
  }

  std::optional<ton::quic::QuicConnectionId> get_outbound_cid() const {
    std::lock_guard guard(mutex);
    return outbound_cid;
  }

  std::optional<ton::quic::QuicConnectionId> get_inbound_cid() const {
    std::lock_guard guard(mutex);
    return inbound_cid;
  }

  mutable std::mutex mutex;
  std::optional<ton::quic::QuicConnectionId> outbound_cid;
  std::optional<ton::quic::QuicConnectionId> inbound_cid;
  std::optional<ton::adnl::AdnlNodeIdShort> outbound_local_id;
  std::optional<ton::adnl::AdnlNodeIdShort> inbound_local_id;
  td::HashSet<ton::quic::QuicConnectionId> closed_connections;
  td::HashSet<ton::quic::QuicStreamID> locally_opened_streams;
  td::HashSet<ton::quic::QuicStreamID> closed_streams;
  size_t closed_stream_count = 0;
  size_t local_closed_stream_count = 0;

 private:
  bool hold_peer_streams_ = false;
  std::vector<ton::quic::QuicConnectionId> held_streams_;
  std::map<ton::quic::QuicStreamID, ton::quic::StreamCloseEvent> closed_stream_events_;
  size_t delivered_payloads_ = 0;
  size_t rejected_streams_ = 0;
};

class RawQuicCallback final : public ton::quic::QuicServer::Callback {
 public:
  explicit RawQuicCallback(std::shared_ptr<RawQuicEndpointState> state) : state_(std::move(state)) {
  }

  void set_server(td::actor::ActorId<ton::quic::QuicServer> server) {
    server_ = server;
  }

  td::Status on_connected(ton::quic::QuicConnectionId cid, ton::adnl::AdnlNodeIdShort local_id,
                          ton::adnl::AdnlNodeIdShort, bool is_outbound, ton::quic::QuicServer::PeerMtuInfo) override {
    state_->remember_connection(cid, is_outbound, local_id);
    return td::Status::OK();
  }

  void on_message(ton::quic::QuicConnectionId cid, ton::quic::QuicStreamID sid,
                  td::Result<td::BufferSlice> message) override {
    if (!state_->is_local_stream(sid)) {
      if (message.is_ok()) {
        state_->remember_stream_payload();
      } else {
        state_->remember_rejected_stream();  // the guard refused and reset the stream
      }
    }
    // A complete peer-initiated message: echo a FIN back (payload ignored, as before). A failed
    // stream (message is an error) was already reset by the guard — nothing to echo. Nothing can be
    // written back on a stream the peer opened one-way; its credit comes back through
    // release_peer_uni_stream_credit() instead, from on_stream_closed below.
    if (message.is_ok() && !state_->is_local_stream(sid) && sid >= 0 && ngtcp2_is_bidi_stream(sid)) {
      td::actor::send_closure(server_, &ton::quic::QuicServer::send_stream_end, cid, sid);
    }
  }

  void on_closed(ton::quic::QuicConnectionId cid) override {
    state_->remember_closed_connection(cid);
  }

  void on_stream_closed(ton::quic::QuicConnectionId cid, ton::quic::StreamCloseEvent event) override {
    state_->remember_closed_stream(event);
    if (event.initiator == ton::quic::StreamInitiator::Peer &&
        event.direction == ton::quic::StreamDirection::Unidirectional && !state_->hold_stream(cid)) {
      td::actor::send_closure(server_, &ton::quic::QuicServer::release_peer_uni_stream_credit, cid);
    }
  }

 private:
  td::actor::ActorId<ton::quic::QuicServer> server_;
  std::shared_ptr<RawQuicEndpointState> state_;
};

struct RawQuicEndpoint {
  int port;
  td::Ed25519::PrivateKey key;
  td::actor::ActorOwn<ton::quic::QuicServer> server;
  std::shared_ptr<RawQuicEndpointState> state;
};

class TestRunner : public td::actor::Actor {
 public:
  using TestFunc = std::function<td::actor::Task<td::Unit>(TestRunner&)>;

  TestRunner(std::string db_root, double timeout, TestFunc test)
      : db_root_(std::move(db_root)), timeout_(timeout), test_(std::move(test)) {
  }

  void start_up() override {
    alarm_timestamp() = td::Timestamp::in(timeout_);
    [](td::actor::Task<td::Unit> task) -> td::actor::Task<td::Unit> {
      (co_await std::move(task).wrap()).ensure();
      co_await td::actor::Yield{};
      td::actor::SchedulerContext::get().stop();
      co_return td::Unit{};
    }(test_(*this))
                                              .start_immediate()
                                              .detach("test");
  }

  void alarm() override {
    LOG(FATAL) << "Test timeout after " << timeout_ << "s";
  }

  // raw_flood_limits keeps the passed limits verbatim; by default per-IP admission is stripped
  // (quic_test_flood_limits) so tests can open many loopback connections without tripping it.
  td::actor::Task<TestNode> create_node(std::string name, int port, std::optional<ton::PrivateKey> key = std::nullopt,
                                        std::string ip = "127.0.0.1", ton::quic::FloodLimits flood_limits = {},
                                        ton::quic::QuicServer::Options options = quic_test_options(),
                                        bool raw_flood_limits = false) {
    TestNode node;
    node.ip = ip;
    node.port = port;
    node.key = key.value_or(make_key(port));
    node.id = ton::adnl::AdnlNodeIdShort{node.key.compute_public_key().compute_short_id()};

    std::string db = db_root_ + "/" + name;
    td::rmrf(db).ignore();
    td::mkdir(db).ensure();

    node.keyring = ton::keyring::Keyring::create(db);
    node.network_manager = ton::adnl::AdnlNetworkManager::create();
    node.adnl = ton::adnl::Adnl::create(db, node.keyring.get());

    td::actor::send_closure(node.adnl, &ton::adnl::Adnl::register_network_manager, node.network_manager.get());

    ton::adnl::AdnlCategoryMask cat_mask;
    cat_mask[0] = true;
    td::IPAddress addr;
    addr.init_host_port(PSTRING() << ip << ":" << port).ensure();
    td::actor::send_closure(node.network_manager, &ton::adnl::AdnlNetworkManager::add_self_addr, addr,
                            std::move(cat_mask), 0);

    co_await td::actor::ask(node.keyring, &ton::keyring::Keyring::add_key, node.key, true);

    auto addr_list = make_addr_list(ip, port);
    td::actor::send_closure(node.adnl, &ton::adnl::Adnl::add_id,
                            ton::adnl::AdnlNodeIdFull{node.key.compute_public_key()}, addr_list, td::uint8(0));

    td::actor::send_closure(node.adnl, &ton::adnl::Adnl::subscribe, node.id, "Q", std::make_unique<EchoCallback>());

    node.received_messages = std::make_shared<ReceivedMessages>();
    td::actor::send_closure(node.adnl, &ton::adnl::Adnl::subscribe, node.id, "M",
                            std::make_unique<EchoCallback>(node.received_messages));

    node.quic_sender = td::actor::create_actor<ton::quic::QuicSender>(
        "quic-" + name, td::actor::actor_dynamic_cast<ton::adnl::AdnlPeerTable>(node.adnl.get()), node.keyring.get(),
        options, raw_flood_limits ? flood_limits : quic_test_flood_limits(flood_limits));

    co_await td::actor::ask(node.quic_sender, &ton::quic::QuicSender::add_id_and_wait, node.id);
    co_return std::move(node);
  }

  // Establish from->to (waiting out the async add_id) with a tiny query, before asserting limits.
  td::actor::Task<td::Unit> warmup(TestNode& from, TestNode& to) {
    for (int i = 0; i < 200; i++) {
      if ((co_await send_query_ex(from, to, "ping", 2.0, 1 << 20).wrap()).is_ok()) {
        co_return td::Unit{};
      }
      co_await td::actor::coro_sleep(td::Timestamp::in(0.02));
    }
    LOG(FATAL) << "warmup: could not reach peer";
    co_return td::Unit{};
  }

  void add_peer(TestNode& from, const TestNode& to) {
    auto addr_list = make_addr_list(to.ip, to.port);
    td::actor::send_closure(from.adnl, &ton::adnl::Adnl::add_peer, from.id,
                            ton::adnl::AdnlNodeIdFull{to.key.compute_public_key()}, addr_list);
  }

  // Register `to` as a trusted peer of `from`: from's QuicServer treats inbound traffic from `to`
  // as trusted (unshaped, reassembly-pool-exempt, drawing on the trusted in-flight budget).
  void trust_peer(TestNode& from, const TestNode& to, td::uint64 mtu) {
    td::actor::send_closure(from.quic_sender, &ton::adnl::AdnlSenderEx::add_peer_mtu, from.id, to.id, mtu,
                            /* trusted = */ true);
  }

  void untrust_peer(TestNode& from, const TestNode& to, td::uint64 mtu) {
    td::actor::send_closure(from.quic_sender, &ton::adnl::AdnlSenderEx::remove_peer_mtu, from.id, to.id, mtu,
                            /* trusted = */ true);
  }

  void set_slow_echo(TestNode& node, double delay) {
    td::actor::send_closure(node.adnl, &ton::adnl::Adnl::subscribe, node.id, "S",
                            std::make_unique<SlowEchoCallback>(delay));
  }

  void set_slow_message(TestNode& node, double delay) {
    td::actor::send_closure(node.adnl, &ton::adnl::Adnl::subscribe, node.id, "N",
                            std::make_unique<SlowMessageCallback>(delay));
  }

  void send_slow_message(TestNode& from, TestNode& to, td::Slice data) {
    td::BufferSlice msg(1 + data.size());
    msg.as_slice()[0] = 'N';
    msg.as_slice().substr(1).copy_from(data);
    td::actor::send_closure(from.quic_sender, &ton::quic::QuicSender::send_message, from.id, to.id, std::move(msg));
  }

  void set_never_respond(TestNode& node) {
    td::actor::send_closure(node.adnl, &ton::adnl::Adnl::subscribe, node.id, "X",
                            std::make_unique<NeverRespondCallback>());
  }

  void set_static_response(TestNode& node, std::string prefix, std::string response) {
    td::actor::send_closure(node.adnl, &ton::adnl::Adnl::subscribe, node.id, std::move(prefix),
                            std::make_unique<StaticResponseCallback>(std::move(response)));
  }

  td::actor::Task<td::BufferSlice> send_never_respond_query(TestNode& from, TestNode& to, td::Slice data,
                                                            double timeout, td::uint64 max_answer_size) {
    td::BufferSlice query(1 + data.size());
    query.as_slice()[0] = 'X';
    query.as_slice().substr(1).copy_from(data);

    auto [future, promise] = td::actor::StartedTask<td::BufferSlice>::make_bridge();
    td::actor::send_closure(from.quic_sender, &ton::quic::QuicSender::send_query_ex, from.id, to.id, std::string("X"),
                            std::move(promise), td::Timestamp::in(timeout), std::move(query), max_answer_size);
    co_return co_await std::move(future);
  }

  td::actor::Task<td::BufferSlice> send_query(TestNode& from, TestNode& to, td::Slice data) {
    td::BufferSlice query(1 + data.size());
    query.as_slice()[0] = 'Q';
    query.as_slice().substr(1).copy_from(data);

    auto [future, promise] = td::actor::StartedTask<td::BufferSlice>::make_bridge();
    td::actor::send_closure(from.quic_sender, &ton::quic::QuicSender::send_query, from.id, to.id, std::string("Q"),
                            std::move(promise), td::Timestamp::in(30.0), std::move(query));
    co_return co_await std::move(future);
  }

  td::actor::Task<td::BufferSlice> send_query_ex(TestNode& from, TestNode& to, td::Slice data, double timeout,
                                                 td::uint64 max_answer_size) {
    td::BufferSlice query(1 + data.size());
    query.as_slice()[0] = 'Q';
    query.as_slice().substr(1).copy_from(data);

    auto [future, promise] = td::actor::StartedTask<td::BufferSlice>::make_bridge();
    td::actor::send_closure(from.quic_sender, &ton::quic::QuicSender::send_query_ex, from.id, to.id, std::string("Q"),
                            std::move(promise), td::Timestamp::in(timeout), std::move(query), max_answer_size);
    co_return co_await std::move(future);
  }

  td::actor::Task<td::BufferSlice> send_slow_query_ex(TestNode& from, TestNode& to, td::Slice data, double timeout,
                                                      td::uint64 max_answer_size) {
    td::BufferSlice query(1 + data.size());
    query.as_slice()[0] = 'S';
    query.as_slice().substr(1).copy_from(data);

    auto [future, promise] = td::actor::StartedTask<td::BufferSlice>::make_bridge();
    td::actor::send_closure(from.quic_sender, &ton::quic::QuicSender::send_query_ex, from.id, to.id, std::string("S"),
                            std::move(promise), td::Timestamp::in(timeout), std::move(query), max_answer_size);
    co_return co_await std::move(future);
  }

  // Send a slow query without awaiting the answer, so it stays in flight; returns its future.
  auto launch_slow_query_ex(TestNode& from, TestNode& to, td::Slice data, double timeout, td::uint64 max_answer_size) {
    td::BufferSlice query(1 + data.size());
    query.as_slice()[0] = 'S';
    query.as_slice().substr(1).copy_from(data);

    auto [future, promise] = td::actor::StartedTask<td::BufferSlice>::make_bridge();
    td::actor::send_closure(from.quic_sender, &ton::quic::QuicSender::send_query_ex, from.id, to.id, std::string("S"),
                            std::move(promise), td::Timestamp::in(timeout), std::move(query), max_answer_size);
    return std::move(future);
  }

  td::actor::Task<td::BufferSlice> send_prefixed_query_ex(TestNode& from, TestNode& to, td::Slice prefix,
                                                          td::Slice data, double timeout, td::uint64 max_answer_size) {
    td::BufferSlice query(prefix.size() + data.size());
    query.as_slice().substr(0, prefix.size()).copy_from(prefix);
    query.as_slice().substr(prefix.size()).copy_from(data);

    auto [future, promise] = td::actor::StartedTask<td::BufferSlice>::make_bridge();
    td::actor::send_closure(from.quic_sender, &ton::quic::QuicSender::send_query_ex, from.id, to.id, prefix.str(),
                            std::move(promise), td::Timestamp::in(timeout), std::move(query), max_answer_size);
    co_return co_await std::move(future);
  }

  void send_message(TestNode& from, TestNode& to, td::Slice data) {
    td::BufferSlice msg(1 + data.size());
    msg.as_slice()[0] = 'M';
    msg.as_slice().substr(1).copy_from(data);
    td::actor::send_closure(from.quic_sender, &ton::quic::QuicSender::send_message, from.id, to.id, std::move(msg));
  }

  td::actor::Task<std::string> collect_quic_metrics(TestNode& node) {
    ton::metrics::Sink sink;
    auto ctx = ton::metrics::Context{sink}.with_name("ton");
    co_await td::actor::ask(node.quic_sender, &ton::quic::QuicSender::collect, ctx);
    co_return std::move(sink).build().render();
  }

  template <class Predicate>
  td::actor::Task<td::Unit> wait_until(Predicate&& predicate, double timeout) {
    auto deadline = td::Timestamp::in(timeout);
    while (!predicate()) {
      ASSERT_TRUE(!deadline.is_in_past());
      co_await td::actor::yield_on_current();
    }
    co_return td::Unit{};
  }

  // wait_until for a condition that must itself be awaited (reading a peer's metrics, say).
  template <class Predicate>
  td::actor::Task<td::Unit> wait_until_async(Predicate&& predicate, double timeout) {
    auto deadline = td::Timestamp::in(timeout);
    while (!co_await predicate()) {
      ASSERT_TRUE(!deadline.is_in_past());
      co_await td::actor::yield_on_current();
    }
    co_return td::Unit{};
  }

  td::actor::Task<std::string> quic_metrics(const TestNode& node) {
    ton::metrics::Sink sink;
    co_await td::actor::ask(node.quic_sender.get(), &ton::quic::QuicSender::collect, ton::metrics::Context{sink});
    co_return std::move(sink).build().render();
  }

  // --- raw-attacker helpers: a bare QuicServer that can hold partial (un-FIN'd) reassembly on a
  // victim QuicSender node, the only way to pin reclaimable bytes now that a completed stream
  // releases its charge at extract. Mirrors the file-scope RawQuic* / RawQuicTestRunner machinery.
  td::actor::Task<RawQuicEndpoint> create_raw_endpoint(ton::quic::FloodLimits flood_limits = {}) {
    auto port = next_port();
    auto key = make_quic_key(port);
    auto state = std::make_shared<RawQuicEndpointState>();
    auto callback = std::make_unique<RawQuicCallback>(state);
    auto* callback_ptr = callback.get();
    auto local_id = ton::adnl::AdnlNodeIdFull(ton::PublicKey(ton::pubkeys::Ed25519(key.get_public_key().move_as_ok())))
                        .compute_short_id();
    auto identity = ton::quic::ServerIdentity{.local_id = local_id, .key = clone_quic_key(key)};
    auto server =
        (ton::quic::QuicServer::create(port, std::move(callback), 4096, std::move(identity), "ton", "127.0.0.1",
                                       quic_test_options(), quic_test_flood_limits(flood_limits)))
            .move_as_ok();
    callback_ptr->set_server(server.get());
    co_await td::actor::Yield{};
    co_return RawQuicEndpoint{port, std::move(key), std::move(server), std::move(state)};
  }

  // Connect a raw endpoint to a QuicSender victim node. The node's QuicServer listens on
  // adnl_port + NODE_PORT_OFFSET (1000) and selects the victim's identity via SNI.
  td::actor::Task<ton::quic::QuicConnectionId> raw_connect_to_node(RawQuicEndpoint& client, TestNode& victim) {
    auto sni = ton::quic::ServerIdentity::sni(victim.id);
    auto cid =
        co_await td::actor::ask(client.server, &ton::quic::QuicServer::connect, td::Slice("127.0.0.1"),
                                victim.port + 1000, clone_quic_key(client.key), td::Slice("ton"), td::Slice(sni));
    co_await wait_until([&] { return client.state->get_outbound_cid().has_value(); }, 5.0);
    co_await td::actor::coro_sleep(td::Timestamp::in(0.05));
    co_return cid;
  }

  td::actor::Task<ton::quic::QuicStreamID> raw_open_stream(RawQuicEndpoint& endpoint, ton::quic::QuicConnectionId cid) {
    auto deadline = td::Timestamp::in(5.0);
    while (true) {
      auto r =
          co_await td::actor::ask(endpoint.server, &ton::quic::QuicServer::open_stream, cid, ton::quic::StreamOptions{})
              .wrap();
      if (r.is_ok()) {
        auto sid = r.move_as_ok();
        endpoint.state->remember_local_stream(sid);
        co_return sid;
      }
      ASSERT_TRUE(!deadline.is_in_past());
      co_await td::actor::coro_sleep(td::Timestamp::in(0.01));
    }
  }

  // Send stream data WITHOUT a FIN, leaving the victim stuck mid-reassembly (charge held).
  td::actor::Task<td::Unit> raw_send_partial(RawQuicEndpoint& endpoint, ton::quic::QuicConnectionId cid,
                                             ton::quic::QuicStreamID sid, td::BufferSlice data) {
    co_await td::actor::ask(endpoint.server, &ton::quic::QuicServer::send_stream, cid, sid, std::move(data), false)
        .wrap();
    co_return td::Unit{};
  }

  static ton::adnl::AdnlNodeIdShort raw_peer_id(const RawQuicEndpoint& endpoint) {
    return ton::adnl::AdnlNodeIdFull(ton::PublicKey(ton::pubkeys::Ed25519(endpoint.key.get_public_key().move_as_ok())))
        .compute_short_id();
  }

  // True while `node`'s QuicSender still holds a connection to `peer_id`. Eviction on the victim is
  // a silent local teardown (no CONNECTION_CLOSE), so we observe it on the victim, not the peer.
  td::actor::Task<bool> node_has_peer_connection(TestNode& node, ton::adnl::AdnlNodeIdShort peer_id) {
    co_return co_await td::actor::ask(node.quic_sender, &ton::quic::QuicSender::has_peer_connection, peer_id);
  }

  // Begin a raw handshake to a QuicSender victim without waiting for it to complete (so a refused /
  // throttled handshake can be observed instead of asserted). Success shows up as get_outbound_cid.
  td::actor::Task<td::Unit> raw_begin_connect(RawQuicEndpoint& client, TestNode& victim) {
    auto sni = ton::quic::ServerIdentity::sni(victim.id);
    co_await td::actor::ask(client.server, &ton::quic::QuicServer::connect, td::Slice("127.0.0.1"), victim.port + 1000,
                            clone_quic_key(client.key), td::Slice("ton"), td::Slice(sni))
        .wrap();
    co_return td::Unit{};
  }

  static bool raw_handshake_done(const RawQuicEndpoint& endpoint) {
    return endpoint.state->get_outbound_cid().has_value();
  }

  // Abandon one peer-initiated stream on the victim: open it, deliver partial data (no FIN), let it
  // land, then RST it. That is a genuine mid-message abandonment — the only thing that charges churn.
  td::actor::Task<td::Unit> abandon_stream(RawQuicEndpoint& attacker, ton::quic::QuicConnectionId cid) {
    auto sid = co_await raw_open_stream(attacker, cid);
    co_await raw_send_partial(attacker, cid, sid, td::BufferSlice(td::Slice("junk")));
    co_await td::actor::coro_sleep(td::Timestamp::in(0.1));  // let the data land before the RST
    td::actor::send_closure(attacker.server, &ton::quic::QuicServer::shutdown_stream, cid, sid);
    co_await td::actor::coro_sleep(td::Timestamp::in(0.05));
    co_return td::Unit{};
  }

  // Open a stream and reset it before sending its first byte. The victim receives RESET_STREAM with
  // final_size=0: stream-open creates the FloodGuard StreamState, but no stream-data callback runs.
  td::actor::Task<td::Unit> abandon_empty_stream(RawQuicEndpoint& attacker, ton::quic::QuicConnectionId cid) {
    auto sid = co_await raw_open_stream(attacker, cid);
    td::actor::send_closure(attacker.server, &ton::quic::QuicServer::shutdown_stream, cid, sid);
    co_await td::actor::coro_sleep(td::Timestamp::in(0.1));
    co_return td::Unit{};
  }

 private:
  std::string db_root_;
  double timeout_;
  TestFunc test_;
};

void run_test(TestRunner::TestFunc test) {
  std::string db_root = "tmp-dir-test-quic-sender";
  td::rmrf(db_root).ignore();
  td::mkdir(db_root).ensure();

  td::actor::Scheduler scheduler({g_config.threads});
  scheduler.run_in_context(
      [&] { td::actor::create_actor<TestRunner>("test", db_root, g_config.timeout, std::move(test)).release(); });
  scheduler.run();

  td::rmrf(db_root).ignore();
}

ton::quic::QuicServer::Options small_stream_limit_options(size_t max_streams_bidi) {
  auto options = quic_test_options();
  options.enable_gso = false;
  options.enable_gro = false;
  options.enable_mmsg = false;
  options.max_streams_bidi = max_streams_bidi;
  return options;
}

class RawQuicTestRunner final : public td::actor::Actor {
 public:
  using TestFunc = std::function<td::actor::Task<td::Unit>(RawQuicTestRunner&)>;

  RawQuicTestRunner(double timeout, TestFunc test) : timeout_(timeout), test_(std::move(test)) {
  }

  void start_up() override {
    alarm_timestamp() = td::Timestamp::in(timeout_);
    [this]() -> td::actor::Task<td::Unit> {
      (co_await test_(*this).wrap()).ensure();
      co_await td::actor::Yield{};
      td::actor::SchedulerContext::get().stop();
      co_return td::Unit{};
    }()
                    .start_immediate()
                    .detach("raw quic test");
  }

  void alarm() override {
    LOG(FATAL) << "Test timeout after " << timeout_ << "s";
  }

  static td::actor::Task<RawQuicEndpoint> create_endpoint(ton::quic::QuicServer::Options options) {
    auto port = next_port();
    auto key = make_quic_key(port);
    auto state = std::make_shared<RawQuicEndpointState>();

    auto callback = std::make_unique<RawQuicCallback>(state);
    auto* callback_ptr = callback.get();
    auto local_id = ton::adnl::AdnlNodeIdFull(ton::PublicKey(ton::pubkeys::Ed25519(key.get_public_key().move_as_ok())))
                        .compute_short_id();
    auto identity = ton::quic::ServerIdentity{.local_id = local_id, .key = clone_quic_key(key)};
    auto server_result = ton::quic::QuicServer::create(port, std::move(callback), 4096, std::move(identity), "ton",
                                                       "127.0.0.1", options, quic_test_flood_limits());
    ASSERT_TRUE(server_result.is_ok());
    auto server = server_result.move_as_ok();
    callback_ptr->set_server(server.get());

    co_await td::actor::Yield{};
    co_return RawQuicEndpoint{port, std::move(key), std::move(server), std::move(state)};
  }

  td::actor::Task<std::pair<ton::quic::QuicConnectionId, ton::quic::QuicConnectionId>> connect(RawQuicEndpoint& client,
                                                                                               RawQuicEndpoint& server,
                                                                                               td::Slice sni = {}) {
    auto outbound_cid_result =
        co_await td::actor::ask(client.server, &ton::quic::QuicServer::connect, td::Slice("127.0.0.1"), server.port,
                                clone_quic_key(client.key), td::Slice("ton"), sni)
            .wrap();
    LOG_CHECK(outbound_cid_result.is_ok()) << "connect failed: " << outbound_cid_result.error();
    auto outbound_cid = outbound_cid_result.move_as_ok();

    co_await wait_until(
        [&] { return client.state->get_outbound_cid().has_value() && server.state->get_inbound_cid().has_value(); },
        5.0);
    co_await td::actor::coro_sleep(td::Timestamp::in(0.05));

    ASSERT_EQ(client.state->get_outbound_cid().value(), outbound_cid);
    co_return std::pair{outbound_cid, server.state->get_inbound_cid().value()};
  }

  td::actor::Task<ton::quic::QuicStreamID> open_stream_with_retry(
      RawQuicEndpoint& endpoint, ton::quic::QuicConnectionId cid,
      ton::quic::StreamDirection direction = ton::quic::StreamDirection::Bidirectional, double timeout = 5.0) {
    auto deadline = td::Timestamp::in(timeout);
    while (true) {
      ton::quic::StreamOptions options;
      options.direction = direction;
      auto sid_result =
          co_await td::actor::ask(endpoint.server, &ton::quic::QuicServer::open_stream, cid, options).wrap();
      if (sid_result.is_ok()) {
        auto sid = sid_result.move_as_ok();
        endpoint.state->remember_local_stream(sid);
        co_return sid;
      }

      auto error = sid_result.error().to_string();
      LOG_CHECK(error.find("-206") != std::string::npos) << "open_stream failed for " << cid << ": " << error;
      LOG_CHECK(!deadline.is_in_past()) << "timed out waiting for stream credit on " << cid;
      co_await td::actor::coro_sleep(td::Timestamp::in(0.001));
    }
  }

  td::actor::Task<ton::quic::QuicStreamID> send_and_fin_stream(
      RawQuicEndpoint& endpoint, ton::quic::QuicConnectionId cid, char byte,
      ton::quic::StreamDirection direction = ton::quic::StreamDirection::Bidirectional) {
    auto sid = co_await open_stream_with_retry(endpoint, cid, direction);

    td::BufferSlice data(1);
    data.as_slice()[0] = byte;

    auto sent_sid_result =
        co_await td::actor::ask(endpoint.server, &ton::quic::QuicServer::send_stream, cid, sid, std::move(data), false)
            .wrap();
    LOG_CHECK(sent_sid_result.is_ok()) << "send_stream(data) failed for sid=" << sid << ": " << sent_sid_result.error();
    auto sent_sid = sent_sid_result.move_as_ok();
    ASSERT_EQ(sent_sid, sid);

    auto fin_sid_result =
        co_await td::actor::ask(endpoint.server, &ton::quic::QuicServer::send_stream, cid, sid, td::BufferSlice{}, true)
            .wrap();
    LOG_CHECK(fin_sid_result.is_ok()) << "send_stream(fin) failed for sid=" << sid << ": " << fin_sid_result.error();
    auto fin_sid = fin_sid_result.move_as_ok();
    ASSERT_EQ(fin_sid, sid);
    co_return sid;
  }

  td::actor::Task<size_t> open_streams_until_blocked(
      RawQuicEndpoint& endpoint, ton::quic::QuicConnectionId cid, size_t max_attempts,
      ton::quic::StreamDirection direction = ton::quic::StreamDirection::Bidirectional) {
    size_t opened = 0;
    for (size_t i = 0; i < max_attempts; i++) {
      ton::quic::StreamOptions options;
      options.direction = direction;
      auto sid_result =
          co_await td::actor::ask(endpoint.server, &ton::quic::QuicServer::open_stream, cid, options).wrap();
      if (sid_result.is_error()) {
        break;
      }
      auto sid = sid_result.move_as_ok();
      endpoint.state->remember_local_stream(sid);
      opened++;
    }
    co_return opened;
  }

  td::actor::Task<td::Unit> wait_for_stream_close(const RawQuicEndpoint& endpoint, ton::quic::QuicStreamID sid,
                                                  double timeout = 5.0) {
    co_await wait_until([&] { return endpoint.state->has_closed_stream(sid); }, timeout);
    co_return td::Unit{};
  }

  td::actor::Task<td::Unit> wait_for_connection_close(const RawQuicEndpoint& endpoint, ton::quic::QuicConnectionId cid,
                                                      double timeout = 5.0) {
    co_await wait_until([&] { return endpoint.state->has_closed_connection(cid); }, timeout);
    co_return td::Unit{};
  }

  template <class Predicate>
  td::actor::Task<td::Unit> wait_until(Predicate&& predicate, double timeout) {
    auto deadline = td::Timestamp::in(timeout);
    while (!predicate()) {
      ASSERT_TRUE(!deadline.is_in_past());
      co_await td::actor::yield_on_current();
    }
    co_return td::Unit{};
  }

 private:
  double timeout_;
  TestFunc test_;
};

void run_raw_quic_test(RawQuicTestRunner::TestFunc test) {
  td::actor::Scheduler scheduler({g_config.threads});
  scheduler.run_in_context([&] {
    td::actor::create_actor<RawQuicTestRunner>("raw quic test", g_config.timeout, std::move(test)).release();
  });
  scheduler.run();
}

void jump_time_to(double at, double epsilon = 0.00) {
  td::Time::jump_in_future(at + epsilon);
}

void jump_time_by(double dt) {
  jump_time_to(td::Time::now() + dt);
}

void assert_ready_connections(const std::string& metrics, td::Slice direction, td::Slice trust, td::uint64 value) {
  auto line = PSTRING() << "ton_quic_transport_connections_ready{direction=\"" << direction << "\",trust=\"" << trust
                        << "\"} " << static_cast<double>(value) << '\n';
  ASSERT_TRUE(metrics.find(line) != std::string::npos);
}

bool has_metric(const std::string& metrics, td::Slice sample, td::uint64 value) {
  auto line = PSTRING() << sample << ' ' << static_cast<double>(value) << '\n';
  return metrics.find(line) != std::string::npos;
}

void assert_metric(const std::string& metrics, td::Slice sample, td::uint64 value) {
  ASSERT_TRUE(has_metric(metrics, sample, value));
}

// The current value of a rendered sample, for tests that must wait on a threshold rather than an
// exact number (a saturating pool passes through many values on its way up).
std::optional<double> metric_value(const std::string& metrics, td::Slice sample) {
  auto pos = metrics.find(PSTRING() << sample << ' ');
  if (pos == std::string::npos) {
    return std::nullopt;
  }
  return td::to_double(td::Slice(metrics).substr(pos + sample.size() + 1));
}

// Client-side timeout/eviction completes its promise before the receiver's handler finishes.
// Drain that real work while the scheduler still runs, including charges orphaned by teardown.
td::actor::Task<td::Unit> wait_for_receiver_inflight(TestRunner& runner, TestNode& receiver, double timeout = 10.0) {
  auto deadline = td::Timestamp::in(timeout);
  while (true) {
    auto metrics = co_await runner.collect_quic_metrics(receiver);
    if (has_metric(metrics, "ton_quic_flood_inflight_bytes{trust=\"untrusted\"}", 0) &&
        has_metric(metrics, "ton_quic_flood_inflight_bytes{trust=\"trusted\"}", 0)) {
      co_return td::Unit{};
    }
    ASSERT_TRUE(!deadline.is_in_past());
    co_await td::actor::coro_sleep(td::Timestamp::in(0.01));
  }
}

}  // namespace

// Ingress shaper - shared token bucket over the connection-level MAX_DATA offset (pure, injected
// time). The server owns the round-robin drain over waiters; here we exercise the bucket math.
// ============================================================================

TEST(QuicIngressShaper, DisabledWhenRateZero) {
  ton::quic::IngressAggregate agg{.total_rate = 0};
  ton::quic::IngressShaper shaper;
  shaper.start(&agg);
  ASSERT_TRUE(!shaper.shaped());
}

TEST(QuicIngressShaper, FullInitialTokensThenRefillAtRate) {
  ton::quic::IngressAggregate agg{.total_rate = 8192};
  ton::quic::IngressShaper shaper;
  shaper.start(&agg);
  shaper.on_data(20000);
  ASSERT_EQ(shaper.take(td::Timestamp::at(1000.0)), static_cast<td::uint64>(8192));  // full initial tokens
  ASSERT_EQ(shaper.take(td::Timestamp::at(1000.5)), static_cast<td::uint64>(4096));  // 0.5s * 8192 = MIN_GRANT
  ASSERT_EQ(shaper.take(td::Timestamp::at(1000.75)), static_cast<td::uint64>(0));    // 2048 < MIN_GRANT: wait
}

TEST(QuicIngressShaper, SmallTailGrantedWhole) {
  ton::quic::IngressAggregate agg{.total_rate = 8192};
  ton::quic::IngressShaper shaper;
  shaper.start(&agg);
  shaper.on_data(1000);  // below MIN_GRANT: granted whole so it can't stall the peer
  ASSERT_EQ(shaper.take(td::Timestamp::at(1000.0)), static_cast<td::uint64>(1000));
  ASSERT_EQ(shaper.debt(), static_cast<td::uint64>(0));
}

TEST(QuicIngressShaper, WorkConservingSingleActiveGetsFullRate) {
  ton::quic::IngressAggregate agg{.total_rate = 8192};
  ton::quic::IngressShaper a, b;
  a.start(&agg);
  b.start(&agg);  // b stays idle; the shared bucket is not diluted by its mere presence
  a.on_data(20000);
  ASSERT_EQ(a.take(td::Timestamp::at(1000.0)), static_cast<td::uint64>(8192));  // full rate, not half
}

TEST(QuicIngressShaper, SharedBucketIsFirstComeAcrossActive) {
  ton::quic::IngressAggregate agg{.total_rate = 8192};
  ton::quic::IngressShaper a, b;
  a.start(&agg);
  b.start(&agg);
  a.on_data(20000);
  b.on_data(20000);
  ASSERT_EQ(a.take(td::Timestamp::at(1000.0)), static_cast<td::uint64>(8192));  // a takes the whole pool
  ASSERT_EQ(b.take(td::Timestamp::at(1000.0)), static_cast<td::uint64>(0));     // pool empty for b
  ASSERT_EQ(b.take(td::Timestamp::at(1000.5)), static_cast<td::uint64>(4096));  // b gets the 0.5s refill
}

TEST(QuicIngressShaper, StopDumpsRemainingDebt) {
  ton::quic::IngressAggregate agg{.total_rate = 8192};
  ton::quic::IngressShaper shaper;
  shaper.start(&agg);
  shaper.on_data(20000);
  shaper.take(td::Timestamp::at(1000.0));  // grants 8192, leaves debt 11808
  ASSERT_EQ(shaper.stop(), static_cast<td::uint64>(11808));
  ASSERT_TRUE(!shaper.shaped());
}

TEST(QuicIngressShaper, TakeRespectsCap) {
  // The fair-share cap bounds a single grant even when both debt and tokens exceed it, so one
  // connection can't drain the bucket in a drain pass while others wait.
  ton::quic::IngressAggregate agg{.total_rate = 1 << 20};  // 1 MB/s -> 1 MB burst
  ton::quic::IngressShaper shaper;
  shaper.start(&agg);
  shaper.on_data(200 << 10);  // wants 200 KB, bucket holds 1 MB
  ASSERT_EQ(shaper.take(td::Timestamp::at(1000.0), 64 << 10), static_cast<td::uint64>(64 << 10));  // capped
  ASSERT_EQ(shaper.debt(), static_cast<td::uint64>((200 - 64) << 10));
}

TEST(QuicIngressShaper, OneDrainPassServesManyWaiters) {
  // The anti-herd property: a full bucket serves a thousand small-debt waiters in a single pass —
  // no per-waiter wake. 1000 * 100B = 100KB fits in the 1MB burst, so all are granted at one time.
  ton::quic::IngressAggregate agg{.total_rate = 1 << 20};
  std::vector<ton::quic::IngressShaper> waiters(1000);
  int served = 0;
  for (auto& w : waiters) {
    w.start(&agg);
    w.on_data(100);
    if (w.take(td::Timestamp::at(1000.0)) == 100) {
      served++;
    }
  }
  ASSERT_EQ(served, 1000);
}

// Test double for FloodGuard::Host mirroring the real QuicServer/pimpl transport seam: each shaped
// connection owns an IngressShaper drawing from the guard's aggregate (as the pimpl does), configure
// starts/stops shaping (a stop dumps leftover debt at line rate, outside the bucket), grant_ingress
// serves debt through the shaper. Everything else is a no-op.
class MockGuardHost : public ton::quic::FloodGuard::Host {
 public:
  struct Conn {
    ton::quic::IngressShaper shaper;
    ton::quic::FloodGuard::TransportCaps caps;
    td::uint64 granted = 0;      // bytes credited from the shared bucket
    td::uint64 grant_calls = 0;  // drain visits
    td::uint64 dumped = 0;       // debt credited instantly on trust upgrade, NOT from the bucket
  };

  void attach(ton::quic::IngressAggregate* agg) {
    agg_ = agg;
  }
  void drop_connection(ton::quic::QuicConnectionId cid, ton::quic::CloseCode code) override {
    dropped.emplace_back(cid, code);
  }
  void reset_stream(ton::quic::QuicConnectionId cid, ton::quic::QuicStreamID sid) override {
    resets.emplace_back(cid, sid);
    if (on_reset) {
      on_reset(sid);
    }
  }
  void deliver(ton::quic::QuicConnectionId, ton::quic::QuicStreamID, td::Result<td::BufferSlice> result) override {
    delivered.push_back(std::move(result));
  }
  void configure(ton::quic::QuicConnectionId cid, ton::quic::FloodGuard::TransportCaps caps) override {
    auto& c = conns_[cid];
    c.caps = caps;
    if (caps.shape_ingress) {
      c.shaper.start(agg_);
    } else {
      c.dumped += c.shaper.stop();
    }
  }
  ton::quic::FloodGuard::IngressGrant grant_ingress(ton::quic::QuicConnectionId cid, td::Timestamp now,
                                                    td::uint64 cap) override {
    auto& c = conns_[cid];
    c.grant_calls++;
    auto granted = c.shaper.take(now, cap);
    c.granted += granted;
    return {granted, c.shaper.debt()};
  }

  Conn& conn(ton::quic::QuicConnectionId cid) {
    return conns_[cid];
  }
  td::uint64 total_granted() const {
    td::uint64 sum = 0;
    for (auto& [cid, c] : conns_) {
      sum += c.granted;
    }
    return sum;
  }

  std::vector<std::pair<ton::quic::QuicConnectionId, ton::quic::CloseCode>> dropped;
  std::vector<std::pair<ton::quic::QuicConnectionId, ton::quic::QuicStreamID>> resets;
  std::vector<td::Result<td::BufferSlice>> delivered;
  std::function<void(ton::quic::QuicStreamID)> on_reset;

 private:
  ton::quic::IngressAggregate* agg_ = nullptr;
  std::map<ton::quic::QuicConnectionId, Conn> conns_;
};

ton::quic::QuicConnectionId make_guard_cid(td::uint8 tag) {
  td::uint8 raw[8] = {tag, 0xab, 0xcd, 1, 2, 3, 4, 5};
  return ton::quic::QuicConnectionId::from_raw(raw, sizeof(raw)).move_as_ok();
}

TEST(QuicIngressShaper, FloodGuardDrainFairnessAndTrustBypass) {
  // The guard-level drain properties the pure bucket-math tests above can't see: driven on a fake
  // clock against the real FloodGuard waiter queue through a mock Host. Asserts (1) permanently
  // backlogged connections split the bucket evenly, (2) total grants obey the token-bucket
  // invariant, (3) a small waiter that clears strands no capacity, (4) a trust upgrade leaves the
  // drain immediately: its debt is dumped outside the bucket and it never consumes tokens again.
  using namespace ton::quic;
  FloodLimits limits;
  limits.untrusted_ingress_rate = 1 << 20;  // 1 MiB/s: burst 1 MiB, each 10ms tick refills ~10.5 KB
  MockGuardHost host;
  FloodGuard guard(limits, host);
  host.attach(&guard.ingress_aggregate());

  auto add_backlogged = [&](td::uint8 tag, td::Slice ip, td::uint64 debt) {
    auto cid = make_guard_cid(tag);
    guard.on_inbound_created(cid, ip.str());
    guard.on_handshake_done(cid, /* trusted = */ false, /* max_message_size = */ 1 << 20).ensure();
    CHECK(host.conn(cid).caps.shape_ingress);
    host.conn(cid).shaper.on_data(debt);
    guard.on_ingress_debt(cid, host.conn(cid).shaper.debt());
    return cid;
  };

  constexpr size_t kConns = 5;
  std::vector<QuicConnectionId> cids;
  for (size_t i = 0; i < kConns; i++) {
    cids.push_back(add_backlogged(td::uint8(i), PSTRING() << "10.0.0." << i, td::uint64{4} << 20));
  }

  // Fake clock in the past: once the first (real-clock-armed) pace tick fires, every re-arm comes
  // from the fake timestamps we pass, so all further pacing is deterministic.
  double base = td::Timestamp::now().at() - 1000.0;
  int k = 1;
  auto fake = [&] { return td::Timestamp::at(base + k * IngressAggregate::DRAIN_TICK); };
  auto armed = td::Timestamp::in(5.0);
  while (host.total_granted() == 0) {
    CHECK(!armed.is_in_past());
    td::usleep_for(2000);
    guard.drain_shaper(fake());
  }
  auto run_ticks = [&](int ticks) {
    for (int i = 0; i < ticks; i++) {
      k++;
      guard.drain_shaper(fake());
    }
  };

  // (1) fairness: the initial burst splits evenly across equal debtors, then refills are served
  // round-robin in MIN_GRANT chunks — every connection stays close to 1/kConns of the total.
  run_ticks(200);
  auto total = host.total_granted();
  for (auto& cid : cids) {
    auto share = double(host.conn(cid).granted) / double(total);
    ASSERT_TRUE(share > 0.17 && share < 0.23);
  }
  // (2) bucket invariant: nothing beyond the initial burst plus rate * elapsed ever leaves.
  double elapsed = (k - 1) * IngressAggregate::DRAIN_TICK;
  ASSERT_TRUE(double(total) <=
              double(limits.untrusted_ingress_rate) * (IngressAggregate::BURST_SECONDS + elapsed) + 1.0);

  // (3) a small waiter is served to zero, leaves the queue, and strands nothing: the backlogged
  // rest keep consuming essentially the whole refill.
  auto small = add_backlogged(0x50, "10.0.1.1", 8 << 10);
  auto before = host.total_granted();
  run_ticks(50);
  ASSERT_EQ(host.conn(small).shaper.debt(), static_cast<td::uint64>(0));
  auto refill = static_cast<td::uint64>(double(limits.untrusted_ingress_rate) * 50 * IngressAggregate::DRAIN_TICK);
  ASSERT_TRUE(host.total_granted() - before + 2 * IngressAggregate::MIN_GRANT >= refill);

  // (4) trust upgrade: unshaped and re-capped at once, leftover debt dumped outside the bucket,
  // never drained again; the remaining four split the full rate between them.
  auto upgraded = cids[0];
  auto calls_at_upgrade = host.conn(upgraded).grant_calls;
  std::map<QuicConnectionId, td::uint64> granted_at_upgrade;
  for (auto& cid : cids) {
    granted_at_upgrade[cid] = host.conn(cid).granted;
  }
  guard.on_peer_info_updated(upgraded, /* trusted = */ true, /* max_message_size = */ 1 << 20);
  ASSERT_TRUE(!host.conn(upgraded).caps.shape_ingress);
  ASSERT_TRUE(host.conn(upgraded).caps.egress_enforced);
  ASSERT_EQ(host.conn(upgraded).caps.egress_cap, limits.max_trusted_connection_egress);
  ASSERT_TRUE(host.conn(upgraded).dumped > 0);  // leftover debt credited instantly, not from the bucket
  run_ticks(100);
  ASSERT_EQ(host.conn(upgraded).grant_calls, calls_at_upgrade);  // no bucket tokens ever again
  td::uint64 delta_total = 0;
  for (size_t i = 1; i < kConns; i++) {
    delta_total += host.conn(cids[i]).granted - granted_at_upgrade[cids[i]];
  }
  for (size_t i = 1; i < kConns; i++) {
    auto share = double(host.conn(cids[i]).granted - granted_at_upgrade[cids[i]]) / double(delta_total);
    ASSERT_TRUE(share > 0.20 && share < 0.30);
  }
}

TEST(QuicFlood, CleanEmptyStreamsDoNotConsumeChurnBudget) {
  using namespace ton::quic;
  FloodLimits limits;
  limits.churn_capacity = 1;
  limits.churn_rate = 0.01;
  MockGuardHost host;
  FloodGuard guard(limits, host);
  host.attach(&guard.ingress_aggregate());

  auto cid = make_guard_cid(0x73);
  guard.on_inbound_created(cid, "10.0.2.2");
  guard.on_handshake_done(cid, /* trusted = */ false, /* max_message_size = */ 1 << 20).ensure();
  auto close = [&](QuicStreamID sid, bool clean) {
    guard.on_stream_closed(
        cid,
        {.sid = sid, .initiator = StreamInitiator::Peer, .direction = StreamDirection::Bidirectional, .clean = clean});
  };

  close(0, true);
  close(4, true);
  close(8, false);  // consumes the one-token allowance; clean closes consumed none
  guard.tick(td::Timestamp::now());
  ASSERT_TRUE(host.dropped.empty());
  close(12, false);  // the next unclean close trips the bucket
  guard.tick(td::Timestamp::now());
  ASSERT_EQ(host.dropped.size(), static_cast<size_t>(1));
  ASSERT_EQ(host.dropped.front().second, CloseCode::too_many_requests);
}

TEST(QuicMemory, StreamStateOverheadIsAccountedUntilClose) {
  using namespace ton::quic;
  FloodLimits limits;
  limits.stream_state_overhead_bytes = 16;
  limits.max_connection_buffered = 32;  // one 16-byte state + 1 byte succeeds; the second is rejected
  limits.max_untrusted_buffered = 1 << 20;
  MockGuardHost host;
  FloodGuard guard(limits, host);
  host.attach(&guard.ingress_aggregate());

  auto cid = make_guard_cid(0x70);
  guard.on_inbound_created(cid, "10.0.2.1");
  guard.on_handshake_done(cid, /* trusted = */ false, /* max_message_size = */ 1 << 20).ensure();

  ASSERT_TRUE(guard.on_stream_data(cid, 0, td::BufferSlice(1), /* fin = */ false).is_ok());
  ASSERT_EQ(guard.stats().buffered_bytes.at(Trust::untrusted).value(), static_cast<td::uint64>(17));

  // The logical state cost follows a live trust reclassification with the rest of ConnectionMemory.
  guard.on_peer_info_updated(cid, /* trusted = */ true, /* max_message_size = */ 1 << 20);
  ASSERT_EQ(guard.stats().buffered_bytes.at(Trust::untrusted).value(), static_cast<td::uint64>(0));
  ASSERT_EQ(guard.stats().buffered_bytes.at(Trust::trusted).value(), static_cast<td::uint64>(17));
  guard.on_peer_info_updated(cid, /* trusted = */ false, /* max_message_size = */ 1 << 20);

  // Preflight refuses the second state before allocating it, so the gauge never overshoots the cap.
  ASSERT_TRUE(guard.on_stream_data(cid, 4, td::BufferSlice(1), /* fin = */ false).is_error());
  ASSERT_EQ(guard.stats().buffered_bytes.at(Trust::untrusted).value(), static_cast<td::uint64>(17));

  guard.on_stream_closed(
      cid, {.sid = 4, .initiator = StreamInitiator::Peer, .direction = StreamDirection::Bidirectional, .clean = false});
  guard.on_stream_closed(
      cid, {.sid = 0, .initiator = StreamInitiator::Peer, .direction = StreamDirection::Bidirectional, .clean = false});
  ASSERT_EQ(guard.stats().buffered_bytes.at(Trust::untrusted).value(), static_cast<td::uint64>(0));
}

TEST(QuicMemory, TinyFragmentsHaveBoundedStorageAndKeepBackingCharged) {
  using namespace ton::quic;
  FloodLimits limits;
  limits.max_untrusted_buffered = 1 << 20;
  limits.max_untrusted_inflight_bytes = 1 << 20;
  MockGuardHost host;
  FloodGuard guard(limits, host);
  host.attach(&guard.ingress_aggregate());
  auto cid = make_guard_cid(0x74);
  guard.on_inbound_created(cid, "10.0.2.3");
  guard.on_handshake_done(cid, false, 1 << 20).ensure();

  // More than 16k one-byte callbacks used to retain one BufferSlice each in BufferBuilder.
  // The charge must grow with coalescing capacity, and the allocator's retained storage must stay
  // proportional to payload rather than callback count.
  constexpr size_t kSize = (32 << 10) + 1;
  const auto allocated_before = td::BufferAllocator::get_buffer_mem();
  for (size_t i = 0; i < kSize; ++i) {
    guard.on_stream_data(cid, 0, td::BufferSlice("x"), false).ensure();
  }
  auto buffered = guard.stats().buffered_bytes.at(Trust::untrusted).value();
  ASSERT_EQ(buffered, limits.stream_state_overhead_bytes + (64u << 10));
  ASSERT_TRUE(td::BufferAllocator::get_buffer_mem() <= allocated_before + (128u << 10));
  guard.on_stream_data(cid, 0, {}, true).ensure();
  ASSERT_EQ(host.delivered.size(), size_t{1});
  ASSERT_EQ(host.delivered[0].ok().as_slice(), td::Slice(std::string(kSize, 'x')));
  ASSERT_EQ(guard.stats().inflight_bytes.at(Trust::untrusted).value(),
            (64u << 10) + limits.inflight_request_overhead_bytes);
  guard.on_stream_closed(
      cid, {.sid = 0, .initiator = StreamInitiator::Peer, .direction = StreamDirection::Bidirectional, .clean = false});
  ASSERT_EQ(guard.stats().buffered_bytes.at(Trust::untrusted).value(), td::uint64{0});
  ASSERT_EQ(guard.stats().inflight_bytes.at(Trust::untrusted).value(),
            (64u << 10) + limits.inflight_request_overhead_bytes);
  guard.on_stream_finished(cid, 0);
  ASSERT_EQ(guard.stats().inflight_bytes.at(Trust::untrusted).value(), td::uint64{0});
}

TEST(QuicMemory, SingleChunkDispatchStaysZeroCopy) {
  using namespace ton::quic;
  MockGuardHost host;
  FloodGuard guard({}, host);
  host.attach(&guard.ingress_aggregate());
  auto cid = make_guard_cid(0x75);
  guard.on_inbound_created(cid, "10.0.2.4");
  guard.on_handshake_done(cid, false, 1 << 20).ensure();
  td::BufferSlice payload(std::string(1024, 'z'));
  auto* original = payload.data();
  guard.on_stream_data(cid, 2, std::move(payload), true).ensure();
  ASSERT_EQ(host.delivered.size(), size_t{1});
  ASSERT_TRUE(host.delivered[0].ok().data() == original);
  guard.on_stream_finished(cid, 2);
  ASSERT_EQ(guard.stats().inflight_bytes.at(Trust::untrusted).value(), td::uint64{0});
}

TEST(QuicMemory, TinyInflightWorkKeepsOverheadAfterStreamAndConnectionClose) {
  using namespace ton::quic;
  for (bool trusted : {false, true}) {
    FloodLimits limits;
    limits.inflight_request_overhead_bytes = 32;
    limits.max_untrusted_inflight_bytes = 65;
    limits.max_trusted_inflight_bytes = 65;
    MockGuardHost host;
    FloodGuard guard(limits, host);
    host.attach(&guard.ingress_aggregate());
    auto cid = make_guard_cid(0x76);
    auto trust = trusted ? Trust::trusted : Trust::untrusted;
    guard.on_inbound_created(cid, "10.0.2.5");
    guard.on_handshake_done(cid, trusted, 1024).ensure();
    guard.on_stream_data(cid, 2, td::BufferSlice("x"), true).ensure();
    guard.on_stream_closed(
        cid,
        {.sid = 2, .initiator = StreamInitiator::Peer, .direction = StreamDirection::Unidirectional, .clean = true});
    ASSERT_EQ(guard.stats().buffered_bytes.at(trust).value(), td::uint64{0});
    ASSERT_EQ(guard.stats().inflight_bytes.at(trust).value(), td::uint64{33});
    ASSERT_TRUE(guard.on_stream_data(cid, 6, td::BufferSlice("y"), true).is_error());
    guard.on_connection_closed(cid);
    ASSERT_EQ(guard.stats().inflight_bytes.at(trust).value(), td::uint64{33});
    guard.on_stream_finished(cid, 2);
    ASSERT_EQ(guard.stats().inflight_bytes.at(trust).value(), td::uint64{0});
  }
}

TEST(QuicMemory, ReorderedFirstByteIsAdmittedAndExpiresBeforeDelivery) {
  using namespace ton::quic;
  FloodLimits limits;
  limits.max_connection_buffered = limits.stream_state_overhead_bytes;  // exactly one pending state
  limits.stream_timeout = 0.1;
  MockGuardHost host;
  FloodGuard guard(limits, host);
  host.attach(&guard.ingress_aggregate());

  struct Callback final : QuicConnectionPImpl::Callback {
    FloodGuard* guard = nullptr;
    QuicConnectionId cid;
    bool ready = false;
    size_t opened = 0;
    size_t data_events = 0;
    void set_connection_id(QuicConnectionId id) override {
      cid = id;
    }
    void on_local_cid_issued(QuicConnectionId) override {
    }
    void on_local_cid_retired(QuicConnectionId) override {
    }
    void on_handshake_completed(HandshakeCompletedEvent) override {
      ready = true;
      if (guard) {
        guard->on_handshake_done(cid, false, 1024).ensure();
      }
    }
    td::Status on_stream_open(QuicStreamID sid) override {
      ++opened;
      return guard ? guard->on_peer_stream_open(cid, sid) : td::Status::OK();
    }
    td::Status on_stream_data(StreamDataEvent event) override {
      ++data_events;
      return guard ? guard->on_stream_data(cid, event.sid, std::move(event.data), event.fin) : td::Status::OK();
    }
    void on_stream_closed(StreamCloseEvent event) override {
      if (guard) {
        guard->on_stream_closed(cid, event);
      }
    }
  };

  td::IPAddress client_address, server_address;
  client_address.init_ipv4_port("127.0.0.1", 19101).ensure();
  server_address.init_ipv4_port("127.0.0.1", 19102).ensure();
  auto key = make_quic_key(19102);
  auto server_id = ton::adnl::AdnlNodeIdFull(ton::PublicKey(ton::pubkeys::Ed25519(key.get_public_key().move_as_ok())))
                       .compute_short_id();
  auto identities = td::make_ref<ServerIdentities>();
  ASSERT_TRUE(identities.write().add_identity({.local_id = server_id, .key = std::move(key)}));
  auto client_callback = std::make_unique<Callback>();
  auto* client_cb = client_callback.get();
  auto client = QuicConnectionPImpl::create_client(client_address, server_address, make_quic_key(19101), "ton", {},
                                                   std::move(client_callback))
                    .move_as_ok();
  auto server_callback = std::make_unique<Callback>();
  auto* server_cb = server_callback.get();
  server_cb->guard = &guard;
  std::unique_ptr<QuicConnectionPImpl> server;
  std::array<char, 65536> packet_bytes, close_bytes;
  auto receive = [&](QuicConnectionPImpl& endpoint, UdpMessageBuffer packet, const td::IPAddress& from) {
    packet.address = from;
    UdpMessageBuffer close{.storage = td::MutableSlice(close_bytes.data(), close_bytes.size()), .address = {}};
    endpoint.handle_ingress(packet, close).ensure();
    ASSERT_TRUE(close.storage.empty());
  };
  auto transfer = [&](bool from_client, bool discard) {
    auto& source = from_client ? *client : *server;
    UdpMessageBuffer packet{.storage = td::MutableSlice(packet_bytes.data(), packet_bytes.size()), .address = {}};
    source.produce_egress(packet, 1).ensure();
    if (packet.storage.empty() || discard) {
      return;
    }
    if (from_client && !server) {
      auto initial = VersionCid::from_initial_datagram(packet.storage).move_as_ok();
      auto info = ServerInitialInfo{.packet = initial, .original_dcid = initial.dcid, .retry_scid = std::nullopt};
      server = QuicConnectionPImpl::create_server(server_address, client_address, identities, "ton", info,
                                                  std::move(server_callback))
                   .move_as_ok();
      guard.on_inbound_created(server_cb->cid, "127.0.0.1");
      host.on_reset = [&](QuicStreamID sid) { server->shutdown_stream(sid); };
    }
    receive(from_client ? *server : *client, packet, from_client ? client_address : server_address);
  };
  auto deadline = td::Timestamp::in(5.0);
  while (!client_cb->ready || !server || !server_cb->ready) {
    ASSERT_TRUE(!deadline.is_in_past());
    transfer(true, false);
    if (server) {
      transfer(false, false);
    }
    td::usleep_for(1000);
  }
  // Flush handshake confirmation/acks before intentionally dropping application packets.
  for (int i = 0; i < 5; ++i) {
    transfer(true, false);
    transfer(false, false);
    td::usleep_for(1000);
  }
  TransportStats transport_stats;
  auto send_until_drained = [&](bool discard) {
    auto until = td::Timestamp::in(1.0);
    while (client->get_stats(transport_stats).bytes_unsent.value() != 0) {
      ASSERT_TRUE(!until.is_in_past());
      transfer(true, discard);
      td::usleep_for(100);
    }
  };
  // No special production API: discard the encrypted packet carrying byte 0, then forward the
  // normal writer's next packet carrying byte 1. ngtcp2 must buffer that byte without a data callback.
  for (int i = 0; i < 2; ++i) {
    auto sid = client->open_stream(StreamDirection::Bidirectional, td::BufferSlice("a"), false).move_as_ok();
    send_until_drained(true);
    client->buffer_stream(sid, td::BufferSlice("b"), false).ensure();
    send_until_drained(false);
  }
  ASSERT_EQ(server_cb->opened, size_t{2});
  ASSERT_EQ(server_cb->data_events, size_t{0});
  ASSERT_TRUE(host.delivered.empty());
  ASSERT_EQ(guard.stats().buffered_bytes.at(Trust::untrusted).value(), limits.stream_state_overhead_bytes);
  ASSERT_EQ(guard.stats().streams_failed.at(StreamFail::connection_memory).value(), td::uint64{1});
  guard.tick(td::Timestamp::in(1.0));
  ASSERT_EQ(host.resets.size(), size_t{1});
  ASSERT_EQ(host.delivered.size(), size_t{1});
  ASSERT_TRUE(host.delivered[0].is_error());
  ASSERT_EQ(guard.stats().streams_failed.at(StreamFail::deadline).value(), td::uint64{1});
  guard.on_connection_closed(server_cb->cid);
  ASSERT_EQ(guard.stats().buffered_bytes.at(Trust::untrusted).value(), td::uint64{0});
}

// ngtcp2 re-credits MAX_DATA by itself for a peer stream we stop (refused at open, failed on data) or
// the peer resets. Each such stream owes the bucket its receive window, once; a stream whose FIN was
// delivered owes nothing. The owed bytes are drawn from the bucket before any debt is extended.
TEST(QuicIngressShaper, StoppedStreamCreditIsCharged) {
  using namespace ton::quic;
  struct Callback final : QuicConnectionPImpl::Callback {
    bool ready = false;
    bool refuse_open = false;
    void set_connection_id(QuicConnectionId) override {
    }
    void on_local_cid_issued(QuicConnectionId) override {
    }
    void on_local_cid_retired(QuicConnectionId) override {
    }
    void on_handshake_completed(HandshakeCompletedEvent) override {
      ready = true;
    }
    td::Status on_stream_open(QuicStreamID) override {
      return refuse_open ? td::Status::Error("refused") : td::Status::OK();
    }
    td::Status on_stream_data(StreamDataEvent event) override {
      return event.data.as_slice() == "bad" ? td::Status::Error("bad") : td::Status::OK();
    }
    void on_stream_closed(StreamCloseEvent) override {
    }
  };

  td::IPAddress client_address, server_address;
  client_address.init_ipv4_port("127.0.0.1", 19111).ensure();
  server_address.init_ipv4_port("127.0.0.1", 19112).ensure();
  auto key = make_quic_key(19112);
  auto server_id = ton::adnl::AdnlNodeIdFull(ton::PublicKey(ton::pubkeys::Ed25519(key.get_public_key().move_as_ok())))
                       .compute_short_id();
  auto identities = td::make_ref<ServerIdentities>();
  ASSERT_TRUE(identities.write().add_identity({.local_id = server_id, .key = std::move(key)}));
  auto client_callback = std::make_unique<Callback>();
  auto* client_cb = client_callback.get();
  auto client = QuicConnectionPImpl::create_client(client_address, server_address, make_quic_key(19111), "ton", {},
                                                   std::move(client_callback))
                    .move_as_ok();
  auto server_callback = std::make_unique<Callback>();
  auto* server_cb = server_callback.get();
  std::unique_ptr<QuicConnectionPImpl> server;
  IngressAggregate agg{.total_rate = 1 << 20};
  std::array<char, 65536> packet_bytes, close_bytes;
  auto transfer = [&](bool from_client) {
    auto& source = from_client ? *client : *server;
    UdpMessageBuffer packet{.storage = td::MutableSlice(packet_bytes.data(), packet_bytes.size()), .address = {}};
    source.produce_egress(packet, 1).ensure();
    if (packet.storage.empty()) {
      return;
    }
    if (from_client && !server) {
      auto initial = VersionCid::from_initial_datagram(packet.storage).move_as_ok();
      auto info = ServerInitialInfo{.packet = initial, .original_dcid = initial.dcid, .retry_scid = std::nullopt};
      server = QuicConnectionPImpl::create_server(server_address, client_address, identities, "ton", info,
                                                  std::move(server_callback))
                   .move_as_ok();
      server->start_ingress_shaping(&agg);
    }
    packet.address = from_client ? client_address : server_address;
    UdpMessageBuffer close{.storage = td::MutableSlice(close_bytes.data(), close_bytes.size()), .address = {}};
    (from_client ? *server : *client).handle_ingress(packet, close).ensure();
    ASSERT_TRUE(close.storage.empty());
  };
  auto pump = [&] {
    for (int i = 0; i < 20; ++i) {
      transfer(true);
      if (server) {
        transfer(false);
      }
      td::usleep_for(1000);
    }
  };
  auto deadline = td::Timestamp::in(5.0);
  while (!client_cb->ready || !server_cb->ready) {
    ASSERT_TRUE(!deadline.is_in_past());
    pump();
  }

  const td::uint64 window = QuicConnectionOptions{}.max_stream_window;
  ASSERT_EQ(server->ingress_debt(), td::uint64{0});
  client->open_stream(StreamDirection::Unidirectional, td::BufferSlice("ok"), true).ensure();
  pump();
  ASSERT_EQ(server->ingress_debt(), td::uint64{2});  // FIN delivered: nothing left to mint

  auto bad = client->open_stream(StreamDirection::Unidirectional, td::BufferSlice("bad"), false).move_as_ok();
  pump();
  ASSERT_EQ(server->ingress_debt(), 2 + 3 + window);  // failed on data
  server->shutdown_stream(bad);
  pump();
  ASSERT_EQ(server->ingress_debt(), 2 + 3 + window);  // stopped twice, peer reset after STOP_SENDING: charged once

  auto reset = client->open_stream(StreamDirection::Bidirectional, td::BufferSlice("r"), false).move_as_ok();
  pump();
  client->shutdown_stream(reset);
  pump();
  ASSERT_EQ(server->ingress_debt(), 2 + 3 + 1 + 2 * window);  // reset by the peer

  server_cb->refuse_open = true;
  client->open_stream(StreamDirection::Unidirectional, td::BufferSlice("x"), false).ensure();
  pump();
  ASSERT_EQ(server->ingress_debt(), 2 + 3 + 1 + 3 * window);  // refused at open: data never delivered

  // A full 1 MiB bucket pays only what is owed; none of it becomes MAX_DATA credit.
  auto debt = server->ingress_debt();
  ASSERT_EQ(server->grant_ingress(td::Timestamp::now(), debt), td::uint64{1} << 20);
  ASSERT_EQ(server->ingress_debt(), debt - (td::uint64{1} << 20));
  IngressShaper shaper;
  shaper.start(&agg);
  shaper.on_data(5000);
  shaper.on_minted(4096);
  agg.tokens = 8192;
  ASSERT_EQ(shaper.take(agg.last_refill), td::uint64{4096});  // 8192 drawn, the first 4096 paid the owed
  ASSERT_EQ(shaper.debt(), td::uint64{904});
}

// Refusing a stream for lack of budget resets it, and that reset returns as an unclean peer-initiated
// close — the same shape as real churn. It may only be charged when the peer filled its OWN budget:
// the shared pool is filled by whoever is fattest, so charging pool refusals would let one peer get
// bystanders on other IPs discouraged.
TEST(QuicFlood, SharedPoolRefusalDoesNotChurnBystander) {
  using namespace ton::quic;
  constexpr td::uint64 kOverhead = 64 << 10;

  auto run = [](FloodLimits limits, int hog_streams) {
    struct Result {
      bool bystander_closed = false;
    };
    MockGuardHost host;
    FloodGuard guard(limits, host);
    host.attach(&guard.ingress_aggregate());

    auto open = [&](td::uint8 tag, td::Slice ip) {
      auto cid = make_guard_cid(tag);
      guard.on_inbound_created(cid, ip.str());
      guard.on_handshake_done(cid, /* trusted = */ false, /* max_message_size = */ 1 << 20).ensure();
      return cid;
    };
    auto hog = open(0x91, "10.0.3.1");
    auto bystander = open(0x92, "10.0.3.2");

    // Live stream states on the hog; payload is irrelevant, the metadata charge is what fills the pool.
    for (int i = 0; i < hog_streams; i++) {
      guard.on_stream_data(hog, i * 4, td::BufferSlice("x"), false).ensure();
    }

    // Refuse far more often than churn_capacity, feeding each reset back as the transport would.
    size_t refusals = 0;
    for (int i = 0; i < 10; i++) {
      QuicStreamID sid = 100 + i * 4;
      if (guard.on_stream_data(bystander, sid, td::BufferSlice("x"), false).is_error()) {
        refusals++;
        guard.on_stream_closed(bystander, {.sid = sid,
                                           .initiator = StreamInitiator::Peer,
                                           .direction = StreamDirection::Bidirectional,
                                           .clean = false});
      }
    }
    CHECK(refusals > limits.churn_capacity);  // enough to trip the bucket if they were charged
    guard.tick(td::Timestamp::now());         // may shed the hog by byte budget; capacity, not abuse

    Result result;
    for (auto& [cid, code] : host.dropped) {
      if (cid == bystander && code == CloseCode::too_many_requests) {
        result.bystander_closed = true;
      }
    }
    return result;
  };

  // The shared pool is the binding constraint: the bystander's own budget is untouched, so it must
  // survive — and keep its IP undiscouraged.
  FloodLimits pool_bound;
  pool_bound.churn_capacity = 2;
  pool_bound.churn_rate = 0.01;
  pool_bound.stream_state_overhead_bytes = kOverhead;
  pool_bound.untrusted_ingress_rate = 0;  // unshaped: this test is about accounting, not pacing
  pool_bound.max_connection_buffered = td::uint64{1} << 30;
  pool_bound.max_untrusted_buffered = kOverhead * 2 + (32 << 10);  // the hog's two states fill it
  ASSERT_TRUE(!run(pool_bound, /* hog_streams = */ 2).bystander_closed);

  // Contrast: with a per-connection budget small enough that the peer's own second stream does not
  // fit, the refusals are self-inflicted and the churn verdict must still fire.
  FloodLimits self_bound;
  self_bound.churn_capacity = 2;
  self_bound.churn_rate = 0.01;
  self_bound.stream_state_overhead_bytes = kOverhead;
  self_bound.untrusted_ingress_rate = 0;
  self_bound.max_connection_buffered = kOverhead + (32 << 10);  // one state fits, the next does not
  self_bound.max_untrusted_buffered = td::uint64{4} << 30;      // pool is never the constraint here
  ASSERT_TRUE(run(self_bound, /* hog_streams = */ 0).bystander_closed);
}

TEST(QuicFlood, ReservedOnlyUpgradeBeforeDrainSurvives) {
  using namespace ton::quic;
  FloodLimits limits;
  MockGuardHost host;
  FloodGuard guard(limits, host);
  host.attach(&guard.ingress_aggregate());

  auto cid = make_guard_cid(0x71);
  guard.on_inbound_created(cid, "10.0.3.1");
  guard.on_handshake_done(cid, /* trusted = */ false, /* max_message_size = */ 1 << 20).ensure();
  guard.set_reserved_only(true);  // queues a policy close
  guard.on_peer_info_updated(cid, /* trusted = */ true, /* max_message_size = */ 1 << 20);
  guard.tick(td::Timestamp::now());
  ASSERT_TRUE(host.dropped.empty());
}

TEST(QuicFlood, ReservedOnlyDisableBeforeDrainSurvives) {
  using namespace ton::quic;
  FloodLimits limits;
  MockGuardHost host;
  FloodGuard guard(limits, host);
  host.attach(&guard.ingress_aggregate());

  auto cid = make_guard_cid(0x72);
  guard.on_inbound_created(cid, "10.0.3.2");
  guard.on_handshake_done(cid, /* trusted = */ false, /* max_message_size = */ 1 << 20).ensure();
  guard.set_reserved_only(true);  // queues a policy close
  guard.set_reserved_only(false);
  guard.tick(td::Timestamp::now());
  ASSERT_TRUE(host.dropped.empty());
}

TEST(QuicEgress, PickerFavorsTrustedFourToOne) {
  // The weighted round-robin sequence, not just "everyone eventually progresses": four trusted-first
  // picks then one untrusted-first, repeating (4:1 at weight 5). Advances on served picks only.
  ton::quic::EgressPicker picker;
  std::string order;
  for (int i = 0; i < 10; i++) {
    order += picker.prefer_untrusted() ? 'u' : 't';
    picker.on_served();
  }
  ASSERT_EQ(order, std::string("ttttuttttu"));
}

TEST(QuicSender, BasicQuery) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    auto a = co_await t.create_node("a", next_port());
    auto b = co_await t.create_node("b", next_port());

    t.add_peer(a, b);
    t.add_peer(b, a);

    auto resp1 = co_await t.send_query(a, b, "a-to-b");
    ASSERT_EQ(resp1.as_slice(), td::Slice("Qa-to-b"));

    auto resp2 = co_await t.send_query(b, a, "b-to-a");
    ASSERT_EQ(resp2.as_slice(), td::Slice("Qb-to-a"));

    co_return td::Unit{};
  });
}

// Sets both peers' default MTU large so the per-stream size cap doesn't mask the memory limits.
static void set_big_mtu(TestNode& a, TestNode& b) {
  td::actor::send_closure(a.quic_sender, &ton::quic::QuicSender::set_default_mtu, td::uint64{1} << 20);
  td::actor::send_closure(b.quic_sender, &ton::quic::QuicSender::set_default_mtu, td::uint64{1} << 20);
}

TEST(QuicIngressShaper, ShapedConnectionDeliversAndTrustedBypasses) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    // End-to-end wiring: with untrusted_ingress_rate enabled, an untrusted peer's query still
    // travels the shaped extend-offset path and completes (the drain/deadline/handle_expiry loop
    // must keep granting credit), and a trusted peer — unshaped after its handshake proves trust —
    // completes too. Pure token-bucket math is covered by the QuicIngressShaper unit tests above.
    ton::quic::FloodLimits limits;
    limits.untrusted_ingress_rate = 256 << 10;  // 256 KB/s aggregate
    auto b = co_await t.create_node("shp-b", next_port(), std::nullopt, "127.0.0.1", limits);
    auto unknown = co_await t.create_node("shp-unknown", next_port());
    auto trusted = co_await t.create_node("shp-trusted", next_port());
    set_big_mtu(unknown, b);
    set_big_mtu(trusted, b);
    td::actor::send_closure(b.quic_sender, &ton::adnl::AdnlSenderEx::add_peer_mtu, b.id, trusted.id,
                            td::uint64{1} << 20, /* trusted = */ true);
    for (auto* p : {&unknown, &trusted}) {
      t.add_peer(*p, b);
      t.add_peer(b, *p);
    }
    co_await t.warmup(unknown, b);
    co_await t.warmup(trusted, b);

    ASSERT_EQ((co_await t.send_query(unknown, b, "shaped")).as_slice(), td::Slice("Qshaped"));
    ASSERT_EQ((co_await t.send_query(trusted, b, "trusted")).as_slice(), td::Slice("Qtrusted"));
    co_return td::Unit{};
  });
}

TEST(QuicIngressShaper, QueuedWaitersDrainedRoundRobin) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    // Force the server-side queue+drain path: with shaping enabled both untrusted queries accrue
    // debt, enqueue in shaper_waiters_, and are served round-robin by drain_shaper. Both must complete.
    ton::quic::FloodLimits limits;
    limits.untrusted_ingress_rate = 2 << 20;  // 2 MB/s aggregate
    auto b = co_await t.create_node("shq-b", next_port(), std::nullopt, "127.0.0.1", limits);
    auto p1 = co_await t.create_node("shq-p1", next_port());
    auto p2 = co_await t.create_node("shq-p2", next_port());
    t.set_slow_echo(b, 0.1);  // register b's echo handler; both queries stay in flight together
    for (auto* p : {&p1, &p2}) {
      set_big_mtu(*p, b);
      t.add_peer(*p, b);
      t.add_peer(b, *p);
      co_await t.warmup(*p, b);
    }
    // Shaped credit is granted only by the paced drain, so both connections queue and are drained round-robin.
    auto q1 = t.launch_slow_query_ex(p1, b, std::string(64 << 10, 'x'), 10.0, 1 << 20);
    auto q2 = t.launch_slow_query_ex(p2, b, std::string(64 << 10, 'y'), 10.0, 1 << 20);
    ASSERT_EQ((co_await std::move(q1)).size(), (64u << 10) + 1);
    ASSERT_EQ((co_await std::move(q2)).size(), (64u << 10) + 1);
    co_return td::Unit{};
  });
}

TEST(QuicIngressShaper, SustainedRateBoundedWithoutWindowPin) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    // The connection window is no longer pinned, so ngtcp2 flow-control auto-tuning is live. Auto-
    // tuning can only inject a bounded one-time burst (<= max_window - initial_max_data, ~20 MB);
    // the shaper still bounds the *sustained* rate by pacing MAX_DATA offset extension. Send far
    // more than that slack and confirm it takes the time the rate dictates — if auto-tuning let the
    // peer bypass the shaper, the whole transfer would land in well under a second.
    ton::quic::FloodLimits limits;
    limits.untrusted_ingress_rate = 4 << 20;  // 4 MB/s aggregate
    auto b = co_await t.create_node("srb-b", next_port(), std::nullopt, "127.0.0.1", limits);
    auto a = co_await t.create_node("srb-a", next_port());
    td::actor::send_closure(a.quic_sender, &ton::quic::QuicSender::set_default_mtu, td::uint64{64} << 20);
    td::actor::send_closure(b.quic_sender, &ton::quic::QuicSender::set_default_mtu, td::uint64{64} << 20);
    t.set_static_response(b, "R", "ok");  // small answer: only the inbound query is shaped
    t.add_peer(a, b);
    t.add_peer(b, a);
    co_await t.warmup(a, b);

    auto start = td::Timestamp::now();
    // 40 MB inbound: ~28 MB flows through the initial window + one bucket + the auto-tune slack,
    // the remaining ~12 MB is paced at 4 MB/s (~3 s). A generous floor keeps this robust.
    auto answer = co_await t.send_prefixed_query_ex(a, b, "R", std::string(40 << 20, 'x'), 30.0, 1 << 20);
    auto elapsed = td::Timestamp::now().at() - start.at();
    ASSERT_EQ(answer.as_slice(), td::Slice("ok"));
    LOG(INFO) << "shaped 40 MB in " << elapsed << "s";
    ASSERT_TRUE(elapsed > 2.0);
    co_return td::Unit{};
  });
}

TEST(QuicIngressShaper, ShapedUploadsConvergeWhileTrustedRacesAhead) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    // Five untrusted peers and one trusted peer each upload 16 MB concurrently. The shared bucket
    // paces the untrusted aggregate at 6 MiB/s; per-connection free headroom is ~4 MB (the initial
    // window — auto-tune needs sub-2-RTT MAX_DATA cadence, impossible at shaped pace), so each
    // paced upload takes ~(16 - 4) / 1.2 ≈ 10s. The rate sits well under what this process can push
    // through QUIC under ASAN, so the trusted upload has real CPU headroom to race ahead. Three
    // timing properties the delivery tests above don't check: the uploads are actually SLOW, the
    // round-robin drain makes them finish TOGETHER (a serial drain would spread them ~0.8*max), and
    // the trusted upload finishes far ahead, unshaped. Completion times are stamped per-upload as
    // each promise resolves — awaiting sequentially would fake convergence.
    constexpr size_t kUntrusted = 5;
    constexpr size_t kPayload = 16 << 20;
    ton::quic::FloodLimits limits;
    limits.untrusted_ingress_rate = 6 << 20;
    limits.stream_timeout = 60.0;  // a shaped 16 MB upload reassembles for ~10s; the default 10s would kill it
    auto b = co_await t.create_node("cvg-b", next_port(), std::nullopt, "127.0.0.1", limits);
    td::actor::send_closure(b.quic_sender, &ton::quic::QuicSender::set_default_mtu, td::uint64{64} << 20);
    t.set_static_response(b, "R", "ok");  // tiny answers: only the shaped inbound leg is measured

    auto trusted = co_await t.create_node("cvg-t", next_port());
    td::actor::send_closure(trusted.quic_sender, &ton::quic::QuicSender::set_default_mtu, td::uint64{64} << 20);
    td::actor::send_closure(b.quic_sender, &ton::adnl::AdnlSenderEx::add_peer_mtu, b.id, trusted.id,
                            td::uint64{64} << 20, /* trusted = */ true);
    t.add_peer(trusted, b);
    t.add_peer(b, trusted);
    co_await t.warmup(trusted, b);

    std::vector<TestNode> peers;
    peers.reserve(kUntrusted);
    for (size_t i = 0; i < kUntrusted; i++) {
      std::string name = PSTRING() << "cvg-u" << i;  // materialize before co_await
      peers.push_back(co_await t.create_node(name, next_port()));
      td::actor::send_closure(peers.back().quic_sender, &ton::quic::QuicSender::set_default_mtu, td::uint64{64} << 20);
      t.add_peer(peers.back(), b);
      t.add_peer(b, peers.back());
      co_await t.warmup(peers.back(), b);
    }

    // slot kUntrusted = the trusted upload; each detached task stamps its own completion.
    auto done_at = std::make_shared<std::array<std::atomic<double>, kUntrusted + 1>>();
    auto failures = std::make_shared<std::atomic<int>>(0);
    auto done = std::make_shared<std::atomic<int>>(0);
    auto start = td::Timestamp::now();
    auto launch = [&](TestNode& p, size_t slot) {
      [](TestRunner& t, TestNode& p, TestNode& b, size_t slot,
         std::shared_ptr<std::array<std::atomic<double>, kUntrusted + 1>> done_at,
         std::shared_ptr<std::atomic<int>> failures, std::shared_ptr<std::atomic<int>> done,
         td::Timestamp start) -> td::actor::Task<td::Unit> {
        auto r = co_await t.send_prefixed_query_ex(p, b, "R", std::string(kPayload, 'x'), 45.0, 1 << 20).wrap();
        if (r.is_error() || r.ok().as_slice() != td::Slice("ok")) {
          failures->fetch_add(1);
        }
        (*done_at)[slot].store(td::Timestamp::now().at() - start.at());
        done->fetch_add(1);
        co_return td::Unit{};
      }(t, p, b, slot, done_at, failures, done, start)
                                     .start_immediate()
                                     .detach("cvg-upload");
    };
    launch(trusted, kUntrusted);
    for (size_t i = 0; i < kUntrusted; i++) {
      launch(peers[i], i);
    }
    co_await t.wait_until([&] { return done->load() == int(kUntrusted) + 1; }, 50.0);
    ASSERT_EQ(failures->load(), 0);

    double lo = 1e9, hi = 0;
    for (size_t i = 0; i < kUntrusted; i++) {
      auto e = (*done_at)[i].load();
      lo = std::min(lo, e);
      hi = std::max(hi, e);
    }
    double trusted_elapsed = (*done_at)[kUntrusted].load();
    LOG(INFO) << "untrusted uploads finished in [" << lo << ", " << hi << "]s, trusted in " << trusted_elapsed << "s";
    ASSERT_TRUE(lo > 4.0);                            // shaped, not fast: the bucket clocks these uploads
    ASSERT_TRUE(hi - lo < std::max(2.5, 0.35 * hi));  // round-robin converges; serial would spread ~0.8*hi
    // Trust bypasses the shaper: an accidentally-shaped trusted peer would ride the same bucket and
    // land at ratio ~1.0. The trusted upload is CPU-bound (ASAN), not bucket-clocked, so leave slack.
    ASSERT_TRUE(trusted_elapsed < 0.75 * lo);
    co_return td::Unit{};
  });
}

TEST(QuicEgress, PerConnectionBufferedCapAppliesToOutbound) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    // The per-connection buffered limit bounds BOTH directions: a small query whose answer exceeds
    // the cap can't be buffered, so b resets the answer stream and the query fails — b never
    // buffers unbounded egress for a peer that solicits answers but never reads them. The reset
    // releases the stream, so small queries on the same connection keep working.
    ton::quic::FloodLimits limits{.max_connection_buffered = 16 << 10,
                                  .max_untrusted_buffered = td::uint64{1} << 30,
                                  .stream_state_overhead_bytes = 0};  // isolate the egress byte cap
    auto a = co_await t.create_node("eg-a", next_port());
    auto b = co_await t.create_node("eg-b", next_port(), std::nullopt, "127.0.0.1", limits);
    set_big_mtu(a, b);
    t.set_static_response(b, "R", std::string(64 << 10, 'y'));  // 64 KB answer > 16 KB cap
    t.add_peer(a, b);
    t.add_peer(b, a);
    co_await t.warmup(a, b);

    for (int i = 0; i < 3; i++) {  // small query, oversized answer: fails on b's egress cap
      ASSERT_TRUE((co_await t.send_prefixed_query_ex(a, b, "R", "hi", 3.0, 1 << 20).wrap()).is_error());
    }
    // The connection isn't wedged: a normal small query still round-trips.
    auto ok = co_await t.send_query_ex(a, b, "small", 5.0, 1 << 20);
    ASSERT_EQ(ok.as_slice(), td::Slice("Qsmall"));
    co_return td::Unit{};
  });
}

TEST(QuicEgress, TrustedFirstDoesNotStarveUntrusted) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    // Egress is scheduled trusted-first but work-conserving: with one trusted and several untrusted
    // peers all soliciting large answers at once (trusted and untrusted egress pending together,
    // exercising the weighted pick), every query still completes — trusted preference must not
    // starve untrusted egress.
    auto b = co_await t.create_node("teg-b", next_port());
    auto trusted = co_await t.create_node("teg-t", next_port());
    t.trust_peer(b, trusted, 1 << 20);
    set_big_mtu(trusted, b);
    t.add_peer(trusted, b);
    t.add_peer(b, trusted);
    co_await t.warmup(trusted, b);
    constexpr int kUntrusted = 4;
    std::vector<TestNode> peers;
    peers.reserve(kUntrusted);
    for (int i = 0; i < kUntrusted; i++) {
      std::string name = PSTRING() << "teg-u" << i;
      peers.push_back(co_await t.create_node(name, next_port()));
      set_big_mtu(peers.back(), b);
      t.add_peer(peers.back(), b);
      t.add_peer(b, peers.back());
      co_await t.warmup(peers.back(), b);
    }
    t.set_static_response(b, "R", std::string(200 << 10, 'y'));  // 200 KB answers backlog egress
    auto launch = [](TestNode& p, TestNode& b) {
      td::BufferSlice q(1);
      q.as_slice()[0] = 'R';
      auto [future, promise] = td::actor::StartedTask<td::BufferSlice>::make_bridge();
      td::actor::send_closure(p.quic_sender, &ton::quic::QuicSender::send_query_ex, p.id, b.id, std::string("R"),
                              std::move(promise), td::Timestamp::in(15.0), std::move(q), td::uint64{1} << 20);
      return std::move(future);
    };
    std::vector<td::actor::StartedTask<td::BufferSlice>> qs;
    qs.push_back(launch(trusted, b));
    for (auto& p : peers) {
      qs.push_back(launch(p, b));
    }
    for (auto& q : qs) {
      ASSERT_EQ((co_await std::move(q)).size(), 200u << 10);  // every class made progress
    }
    co_return td::Unit{};
  });
}

TEST(QuicStreamLimits, BareSendQueryHasDefaultAnswerCap) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    // The bare send_query (no explicit max_answer_size) applies a finite default (16 MB): an answer
    // above it fails, while the same answer succeeds through send_query_ex with a larger cap.
    auto a = co_await t.create_node("cap-a", next_port());
    auto b = co_await t.create_node("cap-b", next_port());
    set_big_mtu(a, b);
    std::string big(20 << 20, 'y');  // 20 MB > 16 MB default
    t.set_static_response(b, "R", big);
    t.add_peer(a, b);
    t.add_peer(b, a);
    co_await t.warmup(a, b);

    auto bare = [&](TestNode& from, TestNode& to) {
      auto [future, promise] = td::actor::StartedTask<td::BufferSlice>::make_bridge();
      td::BufferSlice q(1);
      q.as_slice()[0] = 'R';
      td::actor::send_closure(from.quic_sender, &ton::quic::QuicSender::send_query, from.id, to.id, std::string("R"),
                              std::move(promise), td::Timestamp::in(10.0), std::move(q));
      return std::move(future);
    };
    ASSERT_TRUE((co_await bare(a, b).wrap()).is_error());                        // capped by the 16 MB default
    auto ok = co_await t.send_prefixed_query_ex(a, b, "R", "", 10.0, 32 << 20);  // explicit larger cap
    ASSERT_EQ(ok.size(), big.size());
    co_return td::Unit{};
  });
}

TEST(QuicStreamLimits, ConfiguredDefaultAnswerCapIsHonored) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    // The bare send_query enforces the CONFIGURED default_max_answer_size, not a hardcoded 16 MB: a
    // client with a small custom cap rejects an answer that the struct default would have allowed,
    // so the test fails if the field were ignored. (default_max_answer_size becomes the response
    // stream's reassembly cap on the client side.)
    ton::quic::FloodLimits a_limits;
    a_limits.default_max_answer_size = 100 << 10;  // 100 KB custom cap on the client
    auto a = co_await t.create_node("ccap-a", next_port(), std::nullopt, "127.0.0.1", a_limits);
    auto b = co_await t.create_node("ccap-b", next_port());
    set_big_mtu(a, b);
    t.set_static_response(b, "R", std::string(200 << 10, 'y'));  // 200 KB: over the custom cap, under 16 MB
    t.add_peer(a, b);
    t.add_peer(b, a);
    co_await t.warmup(a, b);

    auto bare = [&](TestNode& from, TestNode& to) {
      auto [future, promise] = td::actor::StartedTask<td::BufferSlice>::make_bridge();
      td::BufferSlice q(1);
      q.as_slice()[0] = 'R';
      td::actor::send_closure(from.quic_sender, &ton::quic::QuicSender::send_query, from.id, to.id, std::string("R"),
                              std::move(promise), td::Timestamp::in(10.0), std::move(q));
      return std::move(future);
    };
    ASSERT_TRUE((co_await bare(a, b).wrap()).is_error());                       // 200 KB > 100 KB custom cap
    auto ok = co_await t.send_prefixed_query_ex(a, b, "R", "", 10.0, 1 << 20);  // explicit larger cap succeeds
    ASSERT_EQ(ok.size(), 200u << 10);
    co_return td::Unit{};
  });
}

TEST(QuicMemory, PerConnectionLimit) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    // Responder caps one connection's reassembly at 64 KB; the pool is huge, so the per-connection
    // cap is the binding limit. A query above it is failed mid-reassembly.
    ton::quic::FloodLimits limits{.max_connection_buffered = 64 << 10, .max_untrusted_buffered = td::uint64{1} << 30};
    auto a = co_await t.create_node("mem-a", next_port());
    auto b = co_await t.create_node("mem-b", next_port(), std::nullopt, "127.0.0.1", limits);
    set_big_mtu(a, b);
    t.add_peer(a, b);
    t.add_peer(b, a);
    co_await t.warmup(a, b);

    auto small = co_await t.send_query_ex(a, b, std::string(16 << 10, 'x'), 5.0, 1 << 20);
    ASSERT_EQ(small.size(), (16u << 10) + 1);  // "Q" + payload, echoed back

    ASSERT_TRUE((co_await t.send_query_ex(a, b, std::string(128 << 10, 'x'), 5.0, 1 << 20).wrap()).is_error());

    co_return td::Unit{};
  });
}

TEST(QuicMemory, UntrustedPoolLimitAndTrustedExemption) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    // Pool cap (64 KB) below the per-connection cap (1 MB): a single untrusted connection whose
    // reassembly crosses the pool cap is failed, while a trusted peer is pool-exempt.
    ton::quic::FloodLimits limits{.max_connection_buffered = td::uint64{1} << 20, .max_untrusted_buffered = 64 << 10};
    auto b = co_await t.create_node("mp-b", next_port(), std::nullopt, "127.0.0.1", limits);
    auto unknown = co_await t.create_node("mp-unknown", next_port());
    auto trusted = co_await t.create_node("mp-trusted", next_port());
    set_big_mtu(unknown, b);
    set_big_mtu(trusted, b);

    // Register the trusted peer via the real path: trust rides on its MTU registration.
    td::actor::send_closure(b.quic_sender, &ton::adnl::AdnlSenderEx::add_peer_mtu, b.id, trusted.id,
                            td::uint64{1} << 20,
                            /* trusted = */ true);

    t.add_peer(unknown, b);
    t.add_peer(b, unknown);
    t.add_peer(trusted, b);
    t.add_peer(b, trusted);
    co_await t.warmup(unknown, b);
    co_await t.warmup(trusted, b);

    std::string big(128 << 10, 'x');  // > pool cap, < per-connection cap
    ASSERT_TRUE((co_await t.send_query_ex(unknown, b, big, 5.0, 1 << 20).wrap()).is_error());

    auto ok = co_await t.send_query_ex(trusted, b, big, 5.0, 1 << 20);
    ASSERT_EQ(ok.size(), (128u << 10) + 1);

    co_return td::Unit{};
  });
}

TEST(QuicMemory, InFlightQueryHoldsInflightBudget) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    // The in-flight budget bounds dispatched-but-unanswered query payload — non-reclaimable memory
    // that eviction can't touch. A slow query held in flight keeps its bytes charged, so a second
    // query that would exceed the budget is refused at admission. The reassembly pools are huge, so
    // only the in-flight budget can bite here.
    ton::quic::FloodLimits limits{.max_connection_buffered = td::uint64{1} << 30,
                                  .max_untrusted_buffered = td::uint64{1} << 30,
                                  .max_untrusted_inflight_bytes = 112 << 10};
    auto a = co_await t.create_node("hold-a", next_port());
    auto b = co_await t.create_node("hold-b", next_port(), std::nullopt, "127.0.0.1", limits);
    set_big_mtu(a, b);
    t.set_slow_echo(b, 3.0);  // b holds the answer 3s, keeping the query in flight
    t.add_peer(a, b);
    t.add_peer(b, a);
    co_await t.warmup(a, b);

    // First slow query (~40 KB) is dispatched and held in flight; its payload stays charged.
    auto slow = t.launch_slow_query_ex(a, b, std::string(40 << 10, 'x'), 10.0, 1 << 20);
    co_await td::actor::coro_sleep(td::Timestamp::in(0.3));  // let it arrive and be dispatched

    // Two 40 KB requests plus their backing capacity and 16 KB fixed costs exceed the 112 KB pool.
    ASSERT_TRUE((co_await t.send_query_ex(a, b, std::string(40 << 10, 'x'), 5.0, 1 << 20).wrap()).is_error());

    // The first query still completes once its delay elapses.
    auto slow_resp = co_await std::move(slow);
    ASSERT_EQ(slow_resp.size(), (40u << 10) + 1);

    co_return td::Unit{};
  });
}

TEST(QuicMemory, InflightChargeSurvivesPeerStreamTeardown) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    // A peer that tears its stream down mid-query must not free the in-flight budget early — the
    // query still runs and its memory is non-reclaimable. The charge is orphaned at stream close
    // and released only once the handler finishes the stream.
    ton::quic::FloodLimits limits{.max_connection_buffered = td::uint64{1} << 30,
                                  .max_untrusted_buffered = td::uint64{1} << 30,
                                  .max_untrusted_inflight_bytes = 112 << 10};
    auto a = co_await t.create_node("orph-a", next_port());
    auto b = co_await t.create_node("orph-b", next_port(), std::nullopt, "127.0.0.1", limits);
    set_big_mtu(a, b);
    t.set_slow_echo(b, 2.0);
    t.add_peer(a, b);
    t.add_peer(b, a);
    co_await t.warmup(a, b);

    // 40 KB slow query; the client gives up after 0.5s and resets its stream while b's handler
    // still holds the query for 2s.
    ASSERT_TRUE((co_await t.send_slow_query_ex(a, b, std::string(40 << 10, 'x'), 0.5, 1 << 20).wrap()).is_error());
    co_await td::actor::coro_sleep(td::Timestamp::in(0.3));  // let the reset close b's stream

    // The torn-down query still runs: its 40 KB stay charged, so another 40 KB is refused. (Were
    // the charge dropped at stream close, this fast query would be admitted and succeed.)
    ASSERT_TRUE((co_await t.send_query_ex(a, b, std::string(40 << 10, 'x'), 5.0, 1 << 20).wrap()).is_error());

    // Once the handler finishes, the orphaned charge is released and admission recovers.
    co_await wait_for_receiver_inflight(t, b);
    auto ok = co_await t.send_query_ex(a, b, std::string(40 << 10, 'x'), 5.0, 1 << 20);
    ASSERT_EQ(ok.size(), (40u << 10) + 1);
    co_return td::Unit{};
  });
}

TEST(QuicMemory, InflightChargeSurvivesConnectionClose) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    // Same, but the whole connection dies mid-query (reserved-only shed): the charge must survive
    // connection teardown and be released only when the handler finishes.
    ton::quic::FloodLimits limits{.max_connection_buffered = td::uint64{1} << 30,
                                  .max_untrusted_buffered = td::uint64{1} << 30,
                                  .max_untrusted_inflight_bytes = 112 << 10};
    // Isolate orphan accounting from the reserved-only reconnect discouragement policy.
    limits.forbidden_discourage_period = 0.0;
    auto a = co_await t.create_node("orphc-a", next_port());
    auto b = co_await t.create_node("orphc-b", next_port(), std::nullopt, "127.0.0.1", limits);
    set_big_mtu(a, b);
    t.set_slow_echo(b, 2.0);
    t.add_peer(a, b);
    t.add_peer(b, a);
    co_await t.warmup(a, b);

    // Park a 40 KB slow query, then shed a's (untrusted) connection out from under it.
    auto parked = t.launch_slow_query_ex(a, b, std::string(40 << 10, 'x'), 10.0, 1 << 20);
    co_await t.wait_until_async(
        [&]() -> td::actor::Task<bool> {
          auto metrics = co_await t.collect_quic_metrics(b);
          auto inflight = metric_value(metrics, "ton_quic_flood_inflight_bytes{trust=\"untrusted\"}");
          co_return inflight.has_value() && *inflight > 0;
        },
        5.0);
    co_await td::actor::ask(b.quic_sender, &ton::quic::QuicSender::set_reserved_only, true);
    ASSERT_TRUE((co_await std::move(parked).wrap()).is_error());  // its connection is gone
    co_await td::actor::ask(b.quic_sender, &ton::quic::QuicSender::set_reserved_only, false);

    auto orphan_metrics = co_await t.collect_quic_metrics(b);
    auto orphan_inflight = metric_value(orphan_metrics, "ton_quic_flood_inflight_bytes{trust=\"untrusted\"}");
    ASSERT_TRUE(orphan_inflight.has_value() && *orphan_inflight > 0);
    auto refused_before =
        metric_value(orphan_metrics, "ton_quic_flood_streams_failed_total{reason=\"inflight\"}").value_or(0.0);

    // The shed query still runs: its 40 KB stay charged, so a fresh connection's 40 KB is refused.
    ASSERT_TRUE((co_await t.send_query_ex(a, b, std::string(40 << 10, 'x'), 5.0, 1 << 20).wrap()).is_error());
    auto probe_metrics = co_await t.collect_quic_metrics(b);
    auto refused_after = metric_value(probe_metrics, "ton_quic_flood_streams_failed_total{reason=\"inflight\"}");
    ASSERT_TRUE(refused_after.has_value() && *refused_after > refused_before);

    // Once the handler finishes, the orphaned charge is released and admission recovers.
    co_await wait_for_receiver_inflight(t, b);
    auto ok = co_await t.send_query_ex(a, b, std::string(40 << 10, 'x'), 5.0, 1 << 20);
    ASSERT_EQ(ok.size(), (40u << 10) + 1);
    co_return td::Unit{};
  });
}

TEST(QuicMemory, RefusedMtuZeroPathLeavesNoInflightCharge) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    // A local id with default mtu 0 accepts no inbound connections. Empty-FIN streams pipelined
    // behind the handshake must not leave in-flight charges behind once the connection is gone.
    auto victim = co_await t.create_node("mtu0-victim", next_port());
    co_await td::actor::ask(victim.quic_sender, &ton::quic::QuicSender::set_default_mtu, td::uint64{0});
    co_await td::actor::coro_sleep(td::Timestamp::in(0.1));  // the server applies the registry change

    auto attacker = co_await t.create_raw_endpoint();
    auto cid = co_await td::actor::ask(attacker.server, &ton::quic::QuicServer::connect, td::Slice("127.0.0.1"),
                                       victim.port + 1000, clone_quic_key(attacker.key), td::Slice("ton"),
                                       td::Slice(ton::quic::ServerIdentity::sni(victim.id)));
    auto deadline = td::Timestamp::in(5.0);
    while (!attacker.state->has_closed_connection(cid)) {
      ASSERT_TRUE(!deadline.is_in_past());
      co_await td::actor::ask(
          attacker.server, &ton::quic::QuicServer::send_stream, cid,
          std::variant<ton::quic::QuicStreamID, ton::quic::StreamOptions>{ton::quic::StreamOptions{}},
          td::BufferSlice{}, true)
          .wrap();
      co_await td::actor::coro_sleep(td::Timestamp::in(0.001));
    }

    ASSERT_TRUE(!(co_await t.node_has_peer_connection(victim, t.raw_peer_id(attacker))));
    auto metrics = co_await t.collect_quic_metrics(victim);
    auto rejected = metric_value(metrics, "ton_quic_transport_handshakes_total{direction=\"in\",result=\"rejected\"}");
    ASSERT_TRUE(rejected.has_value() && *rejected >= 1);
    co_await wait_for_receiver_inflight(t, victim);
    co_return td::Unit{};
  });
}

TEST(QuicMemory, MessageLargerThanInflightBudgetIsRefused) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    // Dispatched backing capacity and fixed overhead stay in flight until delivery completes.
    // Ordinary delivery is quick, but a message larger than the whole pool is refused at dispatch.
    ton::quic::FloodLimits limits{.max_connection_buffered = td::uint64{1} << 30,
                                  .max_untrusted_buffered = td::uint64{1} << 30,
                                  .max_untrusted_inflight_bytes = 112 << 10};
    auto a = co_await t.create_node("hmsg-a", next_port());
    auto b = co_await t.create_node("hmsg-b", next_port(), std::nullopt, "127.0.0.1", limits);
    set_big_mtu(a, b);
    t.add_peer(a, b);
    t.add_peer(b, a);
    co_await t.warmup(a, b);

    // A 128 KB message exceeds the 112 KB capacity-plus-overhead budget and is never delivered.
    t.send_message(a, b, std::string(128 << 10, 'm'));
    co_await td::actor::coro_sleep(td::Timestamp::in(0.5));
    ASSERT_TRUE(b.received_messages->empty());

    // A 40 KB message fits and is delivered; its charge frees once the stream closes, so a later
    // 40 KB query is admitted against the same budget.
    t.send_message(a, b, std::string(40 << 10, 'm'));
    co_await t.wait_until([&] { return !b.received_messages->empty(); }, 5.0);
    co_await td::actor::coro_sleep(td::Timestamp::in(0.3));  // let the message stream close
    auto ok = co_await t.send_query_ex(a, b, std::string(40 << 10, 'x'), 5.0, 1 << 20);
    ASSERT_EQ(ok.size(), (40u << 10) + 1);

    co_return td::Unit{};
  });
}

TEST(QuicMemory, TinyMessagesHoldRequestOverheadUntilDelivered) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    ton::quic::FloodLimits limits;
    limits.max_untrusted_inflight_bytes = limits.inflight_request_overhead_bytes + 1024;
    auto a = co_await t.create_node("tiny-flight-a", next_port());
    auto b = co_await t.create_node("tiny-flight-b", next_port(), std::nullopt, "127.0.0.1", limits);
    t.set_slow_message(b, 2.0);
    t.add_peer(a, b);
    t.add_peer(b, a);
    co_await t.warmup(a, b);

    // The uni stream closes immediately and its stream metadata is gone. Its tiny payload alone
    // would leave practically the whole pool free, but the outstanding delivery owns one fixed cost.
    t.send_slow_message(a, b, "x");
    co_await td::actor::coro_sleep(td::Timestamp::in(0.2));
    ASSERT_TRUE((co_await t.send_query_ex(a, b, "x", 1.0, 1024).wrap()).is_error());
    co_await wait_for_receiver_inflight(t, b);
    auto answer = co_await t.send_query_ex(a, b, "x", 2.0, 1024);
    ASSERT_EQ(answer.size(), size_t{2});
    co_return td::Unit{};
  });
}

TEST(QuicMemory, InFlightMessageHeldUntilDelivered) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    // A message's in-flight charge is held until the subscriber finishes handling it (deliver_ex
    // ack), not merely until the fin is sent. A slow message parks the charge, so a concurrent
    // query over budget is refused while it's still being handled, and admitted once it completes.
    ton::quic::FloodLimits limits{.max_connection_buffered = td::uint64{1} << 30,
                                  .max_untrusted_buffered = td::uint64{1} << 30,
                                  .max_untrusted_inflight_bytes = 112 << 10};
    auto a = co_await t.create_node("imsg-a", next_port());
    auto b = co_await t.create_node("imsg-b", next_port(), std::nullopt, "127.0.0.1", limits);
    set_big_mtu(a, b);
    t.set_slow_message(b, 3.0);  // b holds each message's delivery ack for 3s
    t.add_peer(a, b);
    t.add_peer(b, a);
    co_await t.warmup(a, b);

    t.send_slow_message(a, b, std::string(40 << 10, 'm'));  // 40 KB parked in flight for 3s
    co_await td::actor::coro_sleep(td::Timestamp::in(0.3));
    // Two requests plus capacity and fixed costs exceed the pool while the message is held. (Before F9 the
    // charge would have freed at the immediate fin and this would be admitted.)
    ASSERT_TRUE((co_await t.send_query_ex(a, b, std::string(40 << 10, 'x'), 2.0, 1 << 20).wrap()).is_error());
    // Observe the handler ack releasing its charge before testing admission recovery.
    co_await wait_for_receiver_inflight(t, b);
    auto ok = co_await t.send_query_ex(a, b, std::string(40 << 10, 'x'), 5.0, 1 << 20);
    ASSERT_EQ(ok.size(), (40u << 10) + 1);
    co_return td::Unit{};
  });
}

TEST(QuicMemory, TrustedInflightReserveAdmitsTrustedUnderUntrustedSaturation) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    // Trusted queries draw on their own in-flight pool, independent of the untrusted one. A slow
    // untrusted query that saturates the untrusted budget must not block a trusted query.
    ton::quic::FloodLimits limits;
    limits.max_connection_buffered = td::uint64{1} << 30;
    limits.max_untrusted_buffered = td::uint64{1} << 30;
    limits.max_untrusted_inflight_bytes = 96 << 10;  // capacity plus per-request overhead
    auto b = co_await t.create_node("res-b", next_port(), std::nullopt, "127.0.0.1", limits);
    auto untrusted = co_await t.create_node("res-untrusted", next_port());
    auto trusted = co_await t.create_node("res-trusted", next_port());
    set_big_mtu(untrusted, b);
    set_big_mtu(trusted, b);
    td::actor::send_closure(b.quic_sender, &ton::adnl::AdnlSenderEx::add_peer_mtu, b.id, trusted.id,
                            td::uint64{1} << 20,
                            /* trusted = */ true);
    t.set_slow_echo(b, 3.0);  // hold dispatched queries in flight
    for (auto* p : {&untrusted, &trusted}) {
      t.add_peer(*p, b);
      t.add_peer(b, *p);
    }
    co_await t.warmup(untrusted, b);
    co_await t.warmup(trusted, b);

    // First 35 KB query parks its backing capacity and fixed cost in the untrusted share.
    auto slow = t.launch_slow_query_ex(untrusted, b, std::string(35 << 10, 'x'), 10.0, 1 << 20);
    co_await td::actor::coro_sleep(td::Timestamp::in(0.3));

    // Even their logical payloads plus two fixed costs exceed 96 KB, so a second query is refused.
    ASSERT_TRUE(
        (co_await t.send_slow_query_ex(untrusted, b, std::string(35 << 10, 'x'), 3.0, 1 << 20).wrap()).is_error());
    // A trusted query of the same size is still admitted: it draws on the reserved capacity.
    auto ok = co_await t.send_slow_query_ex(trusted, b, std::string(35 << 10, 'x'), 5.0, 1 << 20);
    ASSERT_EQ(ok.size(), (35u << 10) + 1);

    co_await std::move(slow).wrap();
    co_return td::Unit{};
  });
}

TEST(QuicMemory, TrustedInflightPoolIsFinite) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    // Trusted is generously bounded, not exempt: a trusted query parked in flight charges the
    // trusted in-flight pool, so a second trusted query over the (here tiny) budget is refused at
    // admission — a compromised validator key can't pin unbounded in-flight memory.
    ton::quic::FloodLimits limits;
    limits.max_connection_buffered = td::uint64{1} << 30;
    limits.max_trusted_buffered = td::uint64{1} << 30;
    limits.max_trusted_inflight_bytes = 112 << 10;
    auto b = co_await t.create_node("tif-b", next_port(), std::nullopt, "127.0.0.1", limits);
    auto trusted = co_await t.create_node("tif-t", next_port());
    set_big_mtu(trusted, b);
    t.trust_peer(b, trusted, 1 << 20);
    t.set_slow_echo(b, 3.0);
    t.add_peer(trusted, b);
    t.add_peer(b, trusted);
    co_await t.warmup(trusted, b);

    auto slow = t.launch_slow_query_ex(trusted, b, std::string(40 << 10, 'x'), 10.0, 1 << 20);
    co_await td::actor::coro_sleep(td::Timestamp::in(0.3));
    ASSERT_TRUE((co_await t.send_query_ex(trusted, b, std::string(40 << 10, 'x'), 5.0, 1 << 20).wrap()).is_error());
    auto slow_resp = co_await std::move(slow);
    ASSERT_EQ(slow_resp.size(), (40u << 10) + 1);
    co_return td::Unit{};
  });
}

TEST(QuicMemory, TrustedBufferedFailsNewestWithoutEvicting) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    // Over the trusted reassembly pool the newest stream is failed — the trusted connection is
    // NEVER evicted (unlike untrusted, which sheds fattest). A query above the pool cap fails, but
    // the connection survives and a smaller query still round-trips.
    ton::quic::FloodLimits limits;
    limits.max_connection_buffered = td::uint64{1} << 30;  // large: isolate the pool cap
    limits.max_trusted_buffered = 64 << 10;
    auto b = co_await t.create_node("tbf-b", next_port(), std::nullopt, "127.0.0.1", limits);
    auto trusted = co_await t.create_node("tbf-t", next_port());
    set_big_mtu(trusted, b);
    t.trust_peer(b, trusted, 1 << 20);
    t.add_peer(trusted, b);
    t.add_peer(b, trusted);
    co_await t.warmup(trusted, b);

    // 128 KB > 64 KB trusted pool: the stream is reset mid-reassembly.
    ASSERT_TRUE((co_await t.send_query_ex(trusted, b, std::string(128 << 10, 'x'), 5.0, 1 << 20).wrap()).is_error());
    // The connection was not evicted: a small query still works.
    auto ok = co_await t.send_query_ex(trusted, b, "ping", 5.0, 1 << 20);
    ASSERT_EQ(ok.as_slice(), td::Slice("Qping"));
    co_return td::Unit{};
  });
}

TEST(QuicHandshake, PerIpCapReleasesSlotOnCompletion) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    // A per-IP half-open cap of 1 must not lock out sequential peers: each handshake releases its
    // slot on completion. If register/unregister leaked, the second peer's Initial (same 127.0.0.1)
    // would be dropped forever and its warmup would time out. Peers connect sequentially, so at most
    // one handshake is ever in flight.
    ton::quic::FloodLimits limits;
    limits.max_handshaking_conns_per_ip = 1;
    auto b = co_await t.create_node("hs-b", next_port(), std::nullopt, "127.0.0.1", limits);
    auto p1 = co_await t.create_node("hs-p1", next_port());
    auto p2 = co_await t.create_node("hs-p2", next_port());
    auto p3 = co_await t.create_node("hs-p3", next_port());
    for (auto* p : {&p1, &p2, &p3}) {
      t.add_peer(*p, b);
      t.add_peer(b, *p);
    }
    co_await t.warmup(p1, b);
    co_await t.warmup(p2, b);
    co_await t.warmup(p3, b);
    // All three reached b => the single half-open slot was released after each handshake.
    ASSERT_EQ((co_await t.send_query(p3, b, "ok")).as_slice(), td::Slice("Qok"));
    co_return td::Unit{};
  });
}

TEST(QuicHandshake, TokenlessInitialSpendsNoPerIpBudget) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    // Two-stage admission: a tokenless Initial draws only a stateless Retry (uncounted); admission
    // is charged only against the retried, token-bearing Initial. Exercise it with per-IP admission
    // ACTUALLY ENABLED (raw_flood_limits: the usual helper strips it) — before the reorder the
    // tokenless Initial spent the budget too, so a peer had to survive double-charging to connect.
    // The rate is finite (not the disabled default), so a hard regression that charged stage A or
    // refused tokenless Initials outright would surface here as a stalled handshake.
    ton::quic::FloodLimits limits;
    limits.max_conns_per_ip = 64;
    limits.conn_rate_capacity = 8;
    limits.conn_rate_period = 0.5;  // ~2 tokens/s refill on top of the burst
    auto b = co_await t.create_node("ts-b", next_port(), std::nullopt, "127.0.0.1", limits, quic_test_options(),
                                    /* raw_flood_limits = */ true);
    auto p = co_await t.create_node("ts-p", next_port());
    t.add_peer(p, b);
    t.add_peer(b, p);
    co_await t.warmup(p, b);
    ASSERT_EQ((co_await t.send_query(p, b, "ok")).as_slice(), td::Slice("Qok"));
    co_return td::Unit{};
  });
}

TEST(QuicHandshake, DeadlineClosesStuckHalfOpen) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    // A handshake that never completes is closed at handshake_deadline, well before the 15s idle
    // timeout. Dial a port where nothing listens: the outbound connection stays half-open until the
    // 1s deadline fires. (build_connection_options feeds the deadline to outbound dials too.)
    auto client = co_await t.create_raw_endpoint(ton::quic::FloodLimits{.handshake_deadline = 1.0});
    auto cid = co_await td::actor::ask(client.server, &ton::quic::QuicServer::connect, td::Slice("127.0.0.1"),
                                       next_port(), clone_quic_key(client.key), td::Slice("ton"), td::Slice("dead"));
    co_await td::actor::coro_sleep(td::Timestamp::in(0.4));
    ASSERT_TRUE(!client.state->has_closed_connection(cid));  // still within the deadline
    co_await t.wait_until([&] { return client.state->has_closed_connection(cid); },
                          3.0);  // closed well before 15s idle
    co_return td::Unit{};
  });
}

TEST(QuicHandshake, GlobalCapDoesNotLockOutHonestPeers) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    // A global half-open cap evicts the oldest half-open (503, a short evict-discourage refusal) rather
    // than refusing newcomers, so concurrent honest peers all still complete — an evicted one retries.
    // Refusal would permanently lock some out. Warming the peers up concurrently overshoots the cap
    // (exercising eviction), and warmup's retry rides out both the evictions and the add_id race.
    // (The cap is mildly over-subscribed, not 1: pure FIFO eviction can starve under a max-rate
    // stampede — that regime is what F5's dial backoff addresses, not this liveness property.)
    ton::quic::FloodLimits limits;
    limits.max_handshaking_conns = 2;
    auto b = co_await t.create_node("gc-b", next_port(), std::nullopt, "127.0.0.1", limits);
    constexpr int kPeers = 4;
    std::vector<TestNode> peers;
    peers.reserve(kPeers);  // stable addresses for the detached warmup coroutines
    for (int i = 0; i < kPeers; i++) {
      std::string name = PSTRING() << "gc-p" << i;                 // materialize before co_await: a StackAllocator
      peers.push_back(co_await t.create_node(name, next_port()));  // temporary must not span a suspension
      t.add_peer(peers.back(), b);
      t.add_peer(b, peers.back());
    }
    auto done = std::make_shared<std::atomic<int>>(0);
    for (auto& p : peers) {
      [](TestRunner& t, TestNode& p, TestNode& b, std::shared_ptr<std::atomic<int>> done) -> td::actor::Task<td::Unit> {
        co_await t.warmup(p, b);
        done->fetch_add(1);
        co_return td::Unit{};
      }(t, p, b, done)
                                                                                                 .start_immediate()
                                                                                                 .detach("gc-warmup");
    }
    co_await t.wait_until([&] { return done->load() == kPeers; }, 30.0);  // every honest peer connected
    co_return td::Unit{};
  });
}

TEST(QuicHandshake, ZeroHalfOpenCapsAreUnlimited) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    ton::quic::FloodLimits limits;
    limits.max_handshaking_conns_per_ip = 0;
    auto b = co_await t.create_node("hs0-b", next_port(), std::nullopt, "127.0.0.1", limits);
    auto p1 = co_await t.create_node("hs0-p1", next_port());
    auto p2 = co_await t.create_node("hs0-p2", next_port());
    for (auto* p : {&p1, &p2}) {
      t.add_peer(*p, b);
      t.add_peer(b, *p);
      co_await t.warmup(*p, b);
    }
    ASSERT_EQ((co_await t.send_query(p2, b, "ok")).as_slice(), td::Slice("Qok"));
    co_return td::Unit{};
  });
}

TEST(QuicFlood, ChurnCloseDiscouragesThenThrottlesReconnect) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    // A peer that repeatedly abandons streams mid-message trips the per-connection churn bucket; the
    // victim closes the connection and discourages the IP. Reconnects are then throttled (one per
    // throttle period) for the discourage window. churn_rate is tiny so refill never masks the trip:
    // it fires at exactly churn_capacity + 1 abandonments.
    ton::quic::FloodLimits limits;
    limits.discourage_period = 5.0;
    limits.discourage_throttle_period = 1.5;
    limits.churn_capacity = 2;
    limits.churn_rate = 0.01;  // period 100s: no refill during the test
    auto victim = co_await t.create_node("churn-victim", next_port(), std::nullopt, "127.0.0.1", limits);

    auto a1 = co_await t.create_raw_endpoint();
    auto a1_id = t.raw_peer_id(a1);
    auto cid1 = co_await t.raw_connect_to_node(a1, victim);
    for (int i = 0; i < 3; i++) {  // churn_capacity (2) + 1 -> trip
      co_await t.abandon_stream(a1, cid1);
    }
    // The churn close is silent (no CONNECTION_CLOSE); observe it on the victim. The discourage ban
    // is recorded in the same drain, so it is in place by the time the connection is gone.
    auto closed = td::Timestamp::in(5.0);
    while (co_await t.node_has_peer_connection(victim, a1_id)) {
      ASSERT_TRUE(!closed.is_in_past());
      co_await td::actor::coro_sleep(td::Timestamp::in(0.1));
    }

    // A fresh peer on the same IP is throttled: its Initial is dropped for the throttle window.
    auto a2 = co_await t.create_raw_endpoint();
    co_await t.raw_begin_connect(a2, victim);
    auto refuse_deadline = td::Timestamp::in(0.5);  // < discourage_throttle_period
    while (!refuse_deadline.is_in_past()) {
      ASSERT_TRUE(!t.raw_handshake_done(a2));
      co_await td::actor::coro_sleep(td::Timestamp::in(0.05));
    }
    // Past the throttle period, a2's retransmitted Initial is admitted (throttle, not hard refusal).
    co_await t.wait_until([&] { return t.raw_handshake_done(a2); }, 4.0);

    co_return td::Unit{};
  });
}

TEST(QuicFlood, ResetBeforeFirstByteConsumesChurnBudget) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    // RESET_STREAM(final_size=0) never reaches on_stream_data, but it still allocates and recycles
    // transport stream state. It must consume the same churn budget as a reset after a partial body.
    ton::quic::FloodLimits limits;
    limits.churn_capacity = 2;
    limits.churn_rate = 0.01;  // no refill during the test
    auto victim = co_await t.create_node("empty-churn-victim", next_port(), std::nullopt, "127.0.0.1", limits);

    auto attacker = co_await t.create_raw_endpoint();
    auto attacker_id = t.raw_peer_id(attacker);
    auto cid = co_await t.raw_connect_to_node(attacker, victim);
    for (int i = 0; i < 3; i++) {
      co_await t.abandon_empty_stream(attacker, cid);
    }

    auto deadline = td::Timestamp::in(5.0);
    while (co_await t.node_has_peer_connection(victim, attacker_id)) {
      ASSERT_TRUE(!deadline.is_in_past());
      co_await td::actor::coro_sleep(td::Timestamp::in(0.05));
    }
    co_return td::Unit{};
  });
}

TEST(QuicServer, RefusesIPv6Bind) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    // IPv6 is refused: a /64 lets an attacker rotate addresses past every per-IP cap. IPv4 binds OK.
    auto key = make_quic_key(0x6ba0);
    auto local_id = ton::adnl::AdnlNodeIdFull(ton::PublicKey(ton::pubkeys::Ed25519(key.get_public_key().move_as_ok())))
                        .compute_short_id();
    auto make = [&](td::Slice host) {
      auto identity = ton::quic::ServerIdentity{.local_id = local_id, .key = clone_quic_key(key)};
      auto state = std::make_shared<RawQuicEndpointState>();
      return ton::quic::QuicServer::create(next_port(), std::make_unique<RawQuicCallback>(state), 4096,
                                           std::move(identity), "ton", host);
    };
    ASSERT_TRUE(make("::1").is_error());
    ASSERT_TRUE(make("127.0.0.1").is_ok());
    co_return td::Unit{};
  });
}

TEST(QuicFlood, UntrustedInactivityClose) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    // An untrusted connection kept transport-alive (keep-alive/ACK) but with no request activity is
    // closed at the app-level inactivity window, which the 15s QUIC idle timeout would never reap.
    ton::quic::FloodLimits limits;
    limits.untrusted_inactivity_timeout = 1.5;
    auto b = co_await t.create_node("iac-b", next_port(), std::nullopt, "127.0.0.1", limits);
    auto a = co_await t.create_raw_endpoint();  // a live QuicServer: it keeps the transport alive
    auto cid = co_await t.raw_connect_to_node(a, b);
    co_await t.wait_until([&] { return a.state->has_closed_connection(cid); }, 6.0);  // reaped by inactivity
    co_return td::Unit{};
  });
}

TEST(QuicFlood, DialBackoffFailsFast) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    // After a failed dial the peer is backed off: subsequent queries fail FAST (they are not queued
    // to wait out the backoff), so we neither hang the query nor re-dial a broken peer immediately.
    ton::quic::FloodLimits limits;
    limits.handshake_deadline = 0.5;  // a dead-port dial fails at the handshake deadline
    limits.dial_backoff_initial = 1.5;
    auto a = co_await t.create_node("db-a", next_port(), std::nullopt, "127.0.0.1", limits);
    auto b = co_await t.create_node("db-b", next_port());
    // a knows b's identity, but at a dead port where nothing listens, so every dial fails.
    auto dead_addr = make_addr_list("127.0.0.1", next_port());
    td::actor::send_closure(a.adnl, &ton::adnl::Adnl::add_peer, a.id,
                            ton::adnl::AdnlNodeIdFull{b.key.compute_public_key()}, dead_addr);

    auto t0 = td::Timestamp::now();
    ASSERT_TRUE((co_await t.send_query_ex(a, b, "x", 6.0, 1 << 20).wrap()).is_error());  // real dial, no backoff
    double first = td::Timestamp::now().at() - t0.at();
    ASSERT_TRUE(first > 0.3);  // it actually dialed (took ~the handshake deadline)
    auto t1 = td::Timestamp::now();
    ASSERT_TRUE((co_await t.send_query_ex(a, b, "x", 6.0, 1 << 20).wrap()).is_error());  // backed off
    double second = td::Timestamp::now().at() - t1.at();
    ASSERT_TRUE(second < 0.2);  // failed fast: no wait, no re-dial
    co_return td::Unit{};
  });
}

TEST(QuicFlood, DeadlineKilledStreamsDoNotChurn) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    // Streams failed by our own pending-stream DEADLINE (local policy) must NOT count as churn: a
    // peer whose streams all time out — but which never resets them itself — keeps its connection
    // and is never discouraged, even past churn_capacity such timeouts.
    ton::quic::FloodLimits limits;
    limits.stream_timeout = 0.5;  // pending streams die fast
    limits.churn_capacity = 2;    // small: churn WOULD trip if timeouts were wrongly charged
    limits.churn_rate = 0.01;
    auto victim = co_await t.create_node("dl-victim", next_port(), std::nullopt, "127.0.0.1", limits);
    td::actor::send_closure(victim.quic_sender, &ton::quic::QuicSender::set_default_mtu, td::uint64{1} << 20);

    auto attacker = co_await t.create_raw_endpoint();
    auto attacker_id = t.raw_peer_id(attacker);
    auto cid = co_await t.raw_connect_to_node(attacker, victim);
    auto up = td::Timestamp::in(5.0);
    while (!(co_await t.node_has_peer_connection(victim, attacker_id))) {
      ASSERT_TRUE(!up.is_in_past());
      co_await td::actor::coro_sleep(td::Timestamp::in(0.05));
    }

    for (int i = 0; i < 5; i++) {  // > churn_capacity abandonments, all by deadline (peer never RSTs)
      auto sid = co_await t.raw_open_stream(attacker, cid);
      co_await t.raw_send_partial(attacker, cid, sid, td::BufferSlice(td::Slice("junk")));
      co_await td::actor::coro_sleep(td::Timestamp::in(0.7));  // > stream_timeout: deadline fires
    }

    // Deadline kills never charged churn, so the connection is still up (no discourage/close).
    ASSERT_TRUE(co_await t.node_has_peer_connection(victim, attacker_id));
    co_return td::Unit{};
  });
}

TEST(QuicFlood, PendingStreamDeadlineHonorsTrustClass) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    // A pending inbound stream is deadlined per trust class: an untrusted peer's stuck stream dies
    // at the tight bound, a trusted peer's survives well past it — but is still deadlined, so it too
    // is eventually reclaimed. Both peers hold a partial (un-FIN'd) stream; we watch which is RST'd.
    ton::quic::FloodLimits limits;
    limits.stream_timeout = 0.5;          // untrusted pending streams die fast
    limits.trusted_stream_timeout = 2.5;  // trusted ones get far longer, but still bounded
    auto victim = co_await t.create_node("dltc-victim", next_port(), std::nullopt, "127.0.0.1", limits);
    td::actor::send_closure(victim.quic_sender, &ton::quic::QuicSender::set_default_mtu, td::uint64{1} << 20);

    // Bring victim's QuicServer up and reachable before the raw endpoints connect.
    auto helper = co_await t.create_node("dltc-helper", next_port());
    set_big_mtu(helper, victim);
    t.add_peer(helper, victim);
    t.add_peer(victim, helper);
    co_await t.warmup(helper, victim);

    auto connect_and_hold = [&](RawQuicEndpoint& ep) -> td::actor::Task<ton::quic::QuicStreamID> {
      auto peer_id = t.raw_peer_id(ep);
      auto cid = co_await t.raw_connect_to_node(ep, victim);
      auto up = td::Timestamp::in(5.0);
      while (!(co_await t.node_has_peer_connection(victim, peer_id))) {
        ASSERT_TRUE(!up.is_in_past());
        co_await td::actor::coro_sleep(td::Timestamp::in(0.05));
      }
      auto sid = co_await t.raw_open_stream(ep, cid);
      co_await t.raw_send_partial(ep, cid, sid, td::BufferSlice(td::Slice("junk")));  // held, no FIN
      co_return sid;
    };

    auto untrusted = co_await t.create_raw_endpoint();
    auto trusted = co_await t.create_raw_endpoint();
    // Register the trusted endpoint's key as a trusted peer of the victim before it connects.
    td::actor::send_closure(victim.quic_sender, &ton::adnl::AdnlSenderEx::add_peer_mtu, victim.id,
                            t.raw_peer_id(trusted), td::uint64{1} << 20, /* trusted = */ true);
    co_await td::actor::coro_sleep(td::Timestamp::in(0.2));  // let the trust registration land

    auto untrusted_sid = co_await connect_and_hold(untrusted);
    auto trusted_sid = co_await connect_and_hold(trusted);

    // Past the untrusted deadline, under the trusted one: only the untrusted stream is reclaimed.
    co_await td::actor::coro_sleep(td::Timestamp::in(1.3));
    ASSERT_TRUE(untrusted.state->has_closed_stream(untrusted_sid));
    ASSERT_TRUE(!trusted.state->has_closed_stream(trusted_sid));

    // Past the trusted deadline too: the trusted stream is bounded as well, so it is reclaimed.
    auto deadline = td::Timestamp::in(5.0);
    while (!trusted.state->has_closed_stream(trusted_sid)) {
      ASSERT_TRUE(!deadline.is_in_past());
      co_await td::actor::coro_sleep(td::Timestamp::in(0.1));
    }
    co_return td::Unit{};
  });
}

TEST(QuicFlood, AnswerOnRequestStreamIsRejected) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    // on_stream_complete is directional: a peer-initiated stream carries a request. An untrusted
    // peer that instead sends a well-formed quic_answer on such a stream must have it rejected —
    // silently accepting it (the old behavior) let a peer smuggle answers, and more importantly a
    // query on a reply stream would bypass the untrusted admission budget. We observe the rejection
    // as the victim resetting the attacker's stream.
    auto victim = co_await t.create_node("dir-victim", next_port());
    td::actor::send_closure(victim.quic_sender, &ton::quic::QuicSender::set_default_mtu, td::uint64{1} << 20);

    auto attacker = co_await t.create_raw_endpoint();
    auto attacker_id = t.raw_peer_id(attacker);
    auto cid = co_await t.raw_connect_to_node(attacker, victim);
    auto up = td::Timestamp::in(5.0);
    while (!(co_await t.node_has_peer_connection(victim, attacker_id))) {
      ASSERT_TRUE(!up.is_in_past());
      co_await td::actor::coro_sleep(td::Timestamp::in(0.05));
    }

    // Attacker (client) opens a stream — peer-initiated from the victim's view — and sends a valid
    // quic_answer with FIN. The victim only parses requests on such streams, so this is unexpected.
    auto sid = co_await t.raw_open_stream(attacker, cid);
    auto wire_answer = ton::create_serialize_tl_object<ton::ton_api::quic_answer>(td::BufferSlice("payload"));
    td::actor::send_closure(attacker.server, &ton::quic::QuicServer::send_stream, cid, sid, std::move(wire_answer),
                            true);

    co_await t.wait_until([&] { return attacker.state->has_closed_stream(sid); }, 5.0);
    ASSERT_TRUE(attacker.state->has_closed_stream(sid));
    co_return td::Unit{};
  });
}

TEST(QuicFlood, ReservedOnlyKeepsTrustedShedsUntrusted) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    // The emergency lever: reserved-only mode sheds live untrusted connections and refuses new ones,
    // while trusted peers keep working. Both peers connect first; after the flip the trusted peer
    // still queries and the untrusted peer can no longer get through.
    auto b = co_await t.create_node("ro-b", next_port());        // responder
    auto trusted = co_await t.create_node("ro-a", next_port());  // trusted
    auto untrusted = co_await t.create_node("ro-d", next_port());
    set_big_mtu(trusted, b);
    set_big_mtu(untrusted, b);
    td::actor::send_closure(b.quic_sender, &ton::adnl::AdnlSenderEx::add_peer_mtu, b.id, trusted.id,
                            td::uint64{1} << 20,
                            /* trusted = */ true);
    for (auto* p : {&trusted, &untrusted}) {
      t.add_peer(*p, b);
      t.add_peer(b, *p);
    }
    co_await t.warmup(trusted, b);
    co_await t.warmup(untrusted, b);

    ASSERT_EQ((co_await t.send_query(trusted, b, "x")).as_slice(), td::Slice("Qx"));

    td::actor::send_closure(b.quic_sender, &ton::quic::QuicSender::set_reserved_only, true);
    co_await td::actor::coro_sleep(td::Timestamp::in(0.3));

    ASSERT_EQ((co_await t.send_query(trusted, b, "y")).as_slice(), td::Slice("Qy"));  // trusted keeps working
    ASSERT_TRUE((co_await t.send_query_ex(untrusted, b, "z", 2.0, 1 << 20).wrap()).is_error());  // untrusted shed

    co_return td::Unit{};
  });
}

TEST(QuicFlood, ReservedOnlyClosesLiveTrustDowngrade) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    auto victim = co_await t.create_node("ro-down-victim", next_port());
    auto peer = co_await t.create_node("ro-down-peer", next_port());
    set_big_mtu(peer, victim);
    t.trust_peer(victim, peer, td::uint64{1} << 20);
    t.add_peer(peer, victim);
    t.add_peer(victim, peer);
    co_await t.warmup(peer, victim);

    td::actor::send_closure(victim.quic_sender, &ton::quic::QuicSender::set_reserved_only, true);
    co_await td::actor::coro_sleep(td::Timestamp::in(0.1));
    ASSERT_EQ((co_await t.send_query(peer, victim, "still-trusted")).as_slice(), td::Slice("Qstill-trusted"));

    // Removing the trusted registration reclassifies the live connection. refresh_peer_infos must
    // wake the server so the deferred reserved-only close is drained without unrelated traffic.
    t.untrust_peer(victim, peer, td::uint64{1} << 20);
    auto deadline = td::Timestamp::in(5.0);
    while (co_await t.node_has_peer_connection(victim, peer.id)) {
      ASSERT_TRUE(!deadline.is_in_past());
      co_await td::actor::coro_sleep(td::Timestamp::in(0.05));
    }
    co_return td::Unit{};
  });
}

TEST(QuicFlood, TrustedPeerKeepsWorkingUnderUntrustedFlood) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    // Holistic attack: a swarm of untrusted peers simultaneously pins every untrusted bound on the
    // victim at once — the in-flight query budget (slow queries parked in flight, non-reclaimable)
    // and the reassembly pool (raw partial streams held with no FIN). Trust exempts the trusted peer
    // from all of it, so its queries must keep succeeding throughout while untrusted admission fails.
    ton::quic::FloodLimits limits;
    limits.max_untrusted_inflight_bytes = 256 << 10;  // small untrusted in-flight budget
    limits.max_connection_buffered = 128 << 10;
    limits.max_untrusted_buffered = 512 << 10;  // small untrusted reassembly pool
    limits.max_untrusted_connections = 64;      // generous: let the whole swarm coexist
    auto b = co_await t.create_node("flood-b", next_port(), std::nullopt, "127.0.0.1", limits);
    td::actor::send_closure(b.quic_sender, &ton::quic::QuicSender::set_default_mtu, td::uint64{1} << 20);
    // The parked queries must still be parked when the assertions run. 3s was enough on an idle box
    // but not under load, and a parked query completing RELEASES in-flight budget — which is exactly
    // the budget the assertion below needs to be exhausted, so the test raced its own setup.
    t.set_slow_echo(b, 10.0);

    // Trusted peer: a trusted per-peer MTU makes b classify it as trusted.
    auto trusted = co_await t.create_node("flood-trusted", next_port());
    set_big_mtu(trusted, b);
    t.trust_peer(b, trusted, td::uint64{1} << 20);
    t.add_peer(trusted, b);
    t.add_peer(b, trusted);
    co_await t.warmup(trusted, b);

    // The untrusted swarm connects (kept alive in `attackers`).
    std::vector<TestNode> attackers;
    for (int i = 0; i < 8; i++) {
      auto a = co_await t.create_node("flood-a" + std::to_string(i), next_port());
      set_big_mtu(a, b);
      t.add_peer(a, b);
      t.add_peer(b, a);
      co_await t.warmup(a, b);
      attackers.push_back(std::move(a));
    }

    // Vector 1: each untrusted peer parks a slow query in flight until the untrusted budget is full.
    std::vector<td::actor::StartedTask<td::BufferSlice>> parked;
    for (auto& a : attackers) {
      parked.push_back(t.launch_slow_query_ex(a, b, std::string(48 << 10, 'x'), 30.0, 1 << 20));
    }

    // Wait for the in-flight pool to be observably saturated instead of sleeping a fixed time. Some
    // parked queries are themselves refused once the budget fills — which frees budget again — so
    // "enough time has passed" was never the same condition as "the budget is full".
    constexpr td::uint64 kProbeSize = 48 << 10;
    co_await t.wait_until_async(
        [&]() -> td::actor::Task<bool> {
          auto metrics = co_await t.collect_quic_metrics(b);
          auto inflight = metric_value(metrics, "ton_quic_flood_inflight_bytes{trust=\"untrusted\"}");
          auto refused = metric_value(metrics, "ton_quic_flood_streams_failed_total{reason=\"inflight\"}");
          // Capacity depends on packet fragmentation. Observe an actual refusal of one of these
          // identical parked requests instead of guessing its allocation from logical payload size.
          co_return inflight.has_value() && *inflight > 0 && refused.has_value() && *refused > 0;
        },
        10.0);

    // Vector 2: raw endpoints pin untrusted pool bytes with partial (un-FIN'd) reassembly.
    std::vector<RawQuicEndpoint> raw;
    for (int i = 0; i < 4; i++) {
      auto ep = co_await t.create_raw_endpoint();
      auto cid = co_await t.raw_connect_to_node(ep, b);
      auto sid = co_await t.raw_open_stream(ep, cid);
      co_await t.raw_send_partial(ep, cid, sid, td::BufferSlice(64 << 10));  // held, no FIN
      raw.push_back(std::move(ep));
    }
    co_await td::actor::coro_sleep(td::Timestamp::in(0.5));  // let the raw partial streams land

    // The untrusted budget is saturated: a fresh untrusted query is refused at admission.
    ASSERT_TRUE(
        (co_await t.send_query_ex(attackers[0], b, std::string(kProbeSize, 'x'), 3.0, 1 << 20).wrap()).is_error());

    // Under all that pressure, every trusted query still succeeds and echoes correctly.
    for (int i = 0; i < 20; i++) {
      auto payload = "t" + std::to_string(i);
      auto resp = co_await t.send_query(trusted, b, payload);
      ASSERT_EQ(resp.as_slice(), td::Slice("Q" + payload));
    }

    for (auto& p : parked) {
      co_await std::move(p).wrap();  // drain the parked queries so none is dropped mid-flight
    }
    co_await wait_for_receiver_inflight(t, b, 15.0);  // an evicted query may still be running on b
    co_return td::Unit{};
  });
}

TEST(QuicMemory, EvictsFattestUntrustedOverWatermark) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    // High-watermark eviction: reclaimable (in-reassembly) bytes over the 90% pool watermark, but
    // under the hard cap (so refusal would NOT fire), shed the fattest untrusted connection. Since
    // a completed stream now releases its charge at extract, only a stream stuck mid-reassembly
    // (partial data, no FIN) pins bytes — so we drive it with a raw attacker.
    ton::quic::FloodLimits limits{.max_connection_buffered = td::uint64{1} << 20, .max_untrusted_buffered = 100 << 10};
    auto b = co_await t.create_node("evict-b", next_port(), std::nullopt, "127.0.0.1", limits);
    td::actor::send_closure(b.quic_sender, &ton::quic::QuicSender::set_default_mtu, td::uint64{1} << 20);
    // Bring b's QuicServer up and reachable before the raw attacker connects. warmup's helper
    // connection holds 0 reassembly bytes, so it is never an eviction candidate.
    auto helper = co_await t.create_node("evict-helper", next_port());
    set_big_mtu(helper, b);
    t.add_peer(helper, b);
    t.add_peer(b, helper);
    co_await t.warmup(helper, b);

    auto attacker = co_await t.create_raw_endpoint();
    auto attacker_id = t.raw_peer_id(attacker);
    auto acid = co_await t.raw_connect_to_node(attacker, b);
    auto sid = co_await t.raw_open_stream(attacker, acid);

    // Wait until b registers the attacker's connection (still holding 0 bytes, so not yet a victim).
    auto up = td::Timestamp::in(5.0);
    while (!(co_await t.node_has_peer_connection(b, attacker_id))) {
      ASSERT_TRUE(!up.is_in_past());
      co_await td::actor::coro_sleep(td::Timestamp::in(0.05));
    }

    // Six pending stream states plus one byte apiece: just over 96 KB, between the 90 KB
    // watermark and 100 KB cap, independent of packet fragmentation/coalescing capacity.
    for (int i = 0; i < 6; ++i) {
      if (i != 0) {
        sid = co_await t.raw_open_stream(attacker, acid);
      }
      co_await t.raw_send_partial(attacker, acid, sid, td::BufferSlice(1));
    }

    // Each partial send makes b's server loop again; handle_timeouts then sees the pool over the
    // watermark and sheds the fattest untrusted connection (the attacker). drain_ingress charges a
    // whole burst in one loop, so eviction fires only on a later loop — nudge until b drops it.
    auto deadline = td::Timestamp::in(10.0);
    while (co_await t.node_has_peer_connection(b, attacker_id)) {
      ASSERT_TRUE(!deadline.is_in_past());
      co_await t.raw_send_partial(attacker, acid, sid, td::BufferSlice(1));
      co_await td::actor::coro_sleep(td::Timestamp::in(0.1));
    }

    co_return td::Unit{};
  });
}

TEST(QuicMemory, ZeroUntrustedBufferedPoolIsUnlimited) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    // 0 = unlimited for the untrusted reassembly pool: a partial stream pinning bytes must NOT be
    // evicted. Regression guard — bytes_to_reclaim() treated max_bytes==0 as "reclaim everything",
    // so any buffered byte shed the fattest untrusted connection on the next tick.
    ton::quic::FloodLimits limits{.max_untrusted_buffered = 0};  // 0 = unlimited pool (per-conn cap left at default)
    auto b = co_await t.create_node("zpool-b", next_port(), std::nullopt, "127.0.0.1", limits);
    td::actor::send_closure(b.quic_sender, &ton::quic::QuicSender::set_default_mtu, td::uint64{1} << 20);
    auto helper = co_await t.create_node("zpool-helper", next_port());
    set_big_mtu(helper, b);
    t.add_peer(helper, b);
    t.add_peer(b, helper);
    co_await t.warmup(helper, b);

    auto attacker = co_await t.create_raw_endpoint();
    auto attacker_id = t.raw_peer_id(attacker);
    auto acid = co_await t.raw_connect_to_node(attacker, b);
    auto sid = co_await t.raw_open_stream(attacker, acid);
    auto up = td::Timestamp::in(5.0);
    while (!(co_await t.node_has_peer_connection(b, attacker_id))) {
      ASSERT_TRUE(!up.is_in_past());
      co_await td::actor::coro_sleep(td::Timestamp::in(0.05));
    }

    co_await t.raw_send_partial(attacker, acid, sid, td::BufferSlice(200 << 10));  // pin 200 KB, no FIN
    // Nudge b's loop repeatedly; an over-budget pool would shed the fattest untrusted connection here.
    // With an unlimited pool the connection must survive the whole window.
    auto deadline = td::Timestamp::in(2.0);
    while (!deadline.is_in_past()) {
      co_await t.raw_send_partial(attacker, acid, sid, td::BufferSlice(1));
      co_await td::actor::coro_sleep(td::Timestamp::in(0.1));
    }
    ASSERT_TRUE(co_await t.node_has_peer_connection(b, attacker_id));  // never evicted
    co_return td::Unit{};
  });
}

TEST(QuicMemory, EvictsLeastRecentlyActiveOverConnectionCap) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    // Cap untrusted connections at 2 (byte pool huge, so only the count cap fires). A third
    // untrusted connection evicts the least-recently-active one; the newest survives.
    ton::quic::FloodLimits limits{.max_connection_buffered = td::uint64{1} << 20,
                                  .max_untrusted_buffered = td::uint64{1} << 30,
                                  .max_untrusted_connections = 2};
    auto b = co_await t.create_node("cap-b", next_port(), std::nullopt, "127.0.0.1", limits);
    auto p1 = co_await t.create_node("cap-p1", next_port());
    auto p2 = co_await t.create_node("cap-p2", next_port());
    auto p3 = co_await t.create_node("cap-p3", next_port());
    t.set_slow_echo(b, 5.0);
    for (auto* p : {&p1, &p2, &p3}) {
      set_big_mtu(*p, b);
      t.add_peer(*p, b);
      t.add_peer(b, *p);
    }

    // p1 connects first and then parks an in-flight query, so it is the least-recently-active.
    co_await t.warmup(p1, b);
    auto p1_query = t.launch_slow_query_ex(p1, b, std::string("hold"), 10.0, 1 << 20);
    co_await td::actor::coro_sleep(td::Timestamp::in(0.3));
    co_await t.warmup(p2, b);  // more recent
    co_await td::actor::coro_sleep(td::Timestamp::in(0.3));
    co_await t.warmup(p3, b);  // third untrusted connection -> LRU (p1) is evicted

    ASSERT_TRUE((co_await std::move(p1_query).wrap()).is_error());  // p1 was evicted

    // Eviction fails p1 immediately, but b's non-cancellable slow handler still owns the query.
    // Preserve the eviction assertion and let that handler finish before stopping the scheduler.
    co_await wait_for_receiver_inflight(t, b);

    co_return td::Unit{};
  });
}

TEST(QuicMemory, RunningQuerySurvivesStreamTimeout) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    // The stream deadline can cancel only PENDING streams; a dispatched (running) query runs to
    // completion regardless. With a 1s inbound deadline and a 2.5s handler, the query still
    // succeeds — it was already past reassembly (running), so the deadline leaves it alone.
    ton::quic::FloodLimits limits;
    limits.stream_timeout = 1.0;
    auto a = co_await t.create_node("to-a", next_port());
    auto b = co_await t.create_node("to-b", next_port(), std::nullopt, "127.0.0.1", limits);
    set_big_mtu(a, b);
    t.set_slow_echo(b, 2.5);  // handler answers well after the 1s deadline
    t.add_peer(a, b);
    t.add_peer(b, a);
    co_await t.warmup(a, b);

    auto resp = co_await t.send_slow_query_ex(a, b, std::string("work"), 10.0, 1 << 20);
    ASSERT_EQ(resp.as_slice(), td::Slice("Swork"));  // running query completed despite the deadline

    co_return td::Unit{};
  });
}

TEST(QuicSender, ReadyMetricsByDirectionAndTrust) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    auto a = co_await t.create_node("metrics-a", next_port());
    auto b = co_await t.create_node("metrics-b", next_port());
    // QuicSender::add_id completes asynchronously.
    co_await td::actor::coro_sleep(td::Timestamp::in(0.1));
    t.add_peer(a, b);
    t.add_peer(b, a);

    co_await t.send_query(a, b, "outbound");
    co_await t.send_query(b, a, "inbound");
    t.send_message(a, b, "outbound");
    t.send_message(b, a, "inbound");
    co_await t.wait_until([&] { return a.received_messages->size() == 1 && b.received_messages->size() == 1; }, 2.0);

    auto metrics = co_await t.collect_quic_metrics(a);
    assert_ready_connections(metrics, "in", "trusted", 0);
    assert_ready_connections(metrics, "in", "untrusted", 1);
    assert_ready_connections(metrics, "out", "trusted", 0);
    assert_ready_connections(metrics, "out", "untrusted", 1);

    constexpr td::uint64 mtu = 4'096;
    co_await td::actor::ask(a.quic_sender, &ton::adnl::AdnlSenderEx::add_peer_mtu, a.id, b.id, mtu, true);
    co_await t.send_query(a, b, "trusted-outbound");
    co_await t.send_query(b, a, "trusted-inbound");
    t.send_message(a, b, "trusted-outbound");
    t.send_message(b, a, "trusted-inbound");
    co_await t.wait_until([&] { return a.received_messages->size() == 2 && b.received_messages->size() == 2; }, 2.0);
    auto deadline = td::Timestamp::in(2.0);
    while (true) {
      metrics = co_await t.collect_quic_metrics(a);
      if (has_metric(metrics, "ton_quic_message_confirmation_seconds_count{trust=\"trusted\",tl=\"unknown\"}", 1) &&
          has_metric(metrics, "ton_quic_message_confirmation_seconds_count{trust=\"untrusted\",tl=\"unknown\"}", 1)) {
        break;
      }
      ASSERT_TRUE(!deadline.is_in_past());
      co_await td::actor::yield_on_current();
    }
    assert_ready_connections(metrics, "in", "trusted", 1);
    assert_ready_connections(metrics, "in", "untrusted", 0);
    assert_ready_connections(metrics, "out", "trusted", 1);
    assert_ready_connections(metrics, "out", "untrusted", 0);
    for (auto trust : {td::Slice("trusted"), td::Slice("untrusted")}) {
      assert_metric(metrics,
                    PSTRING() << "ton_quic_app_messages_total{trust=\"" << trust
                              << "\",kind=\"query\",direction=\"out\",tl=\"unknown\"}",
                    1);
      assert_metric(metrics,
                    PSTRING() << "ton_quic_app_messages_total{trust=\"" << trust
                              << "\",kind=\"query\",direction=\"in\",tl=\"unknown\"}",
                    1);
      assert_metric(metrics,
                    PSTRING() << "ton_quic_app_messages_total{trust=\"" << trust
                              << "\",kind=\"answer\",direction=\"out\",tl=\"unknown\"}",
                    1);
      assert_metric(metrics,
                    PSTRING() << "ton_quic_app_messages_total{trust=\"" << trust
                              << "\",kind=\"answer\",direction=\"in\",tl=\"unknown\"}",
                    1);
      assert_metric(metrics,
                    PSTRING() << "ton_quic_query_roundtrip_seconds_count{trust=\"" << trust << "\",tl=\"unknown\"}", 1);
      assert_metric(metrics,
                    PSTRING() << "ton_quic_app_messages_total{trust=\"" << trust
                              << "\",kind=\"message\",direction=\"out\",tl=\"unknown\"}",
                    1);
      assert_metric(metrics,
                    PSTRING() << "ton_quic_app_messages_total{trust=\"" << trust
                              << "\",kind=\"message\",direction=\"in\",tl=\"unknown\"}",
                    1);
      assert_metric(
          metrics, PSTRING() << "ton_quic_message_confirmation_seconds_count{trust=\"" << trust << "\",tl=\"unknown\"}",
          1);
    }

    co_await td::actor::ask(a.quic_sender, &ton::adnl::AdnlSenderEx::remove_peer_mtu, a.id, b.id, mtu, true);
    metrics = co_await t.collect_quic_metrics(a);
    assert_ready_connections(metrics, "in", "trusted", 0);
    assert_ready_connections(metrics, "in", "untrusted", 1);
    assert_ready_connections(metrics, "out", "trusted", 0);
    assert_ready_connections(metrics, "out", "untrusted", 1);
    co_return td::Unit{};
  });
}

TEST(QuicSender, QueryMetricsKeepStartingTrust) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    auto a = co_await t.create_node("snapshot-a", next_port());
    auto b = co_await t.create_node("snapshot-b", next_port());
    t.add_peer(a, b);
    t.add_peer(b, a);
    t.set_slow_echo(b, 0.05);
    // QuicSender::add_id and Adnl::subscribe complete asynchronously.
    co_await td::actor::coro_sleep(td::Timestamp::in(0.1));

    constexpr td::uint64 mtu = 4'096;
    co_await td::actor::ask(a.quic_sender, &ton::adnl::AdnlSenderEx::add_peer_mtu, a.id, b.id, mtu, true);
    auto pending = t.send_slow_query_ex(a, b, "snapshot", 2.0, 1 << 20).start_immediate();
    co_await td::actor::ask(a.quic_sender, &ton::adnl::AdnlSenderEx::remove_peer_mtu, a.id, b.id, mtu, true);

    auto response = co_await std::move(pending);
    ASSERT_EQ(response.as_slice(), td::Slice("Ssnapshot"));
    auto metrics = co_await t.collect_quic_metrics(a);
    assert_metric(metrics,
                  "ton_quic_app_messages_total{trust=\"trusted\",kind=\"query\",direction=\"out\",tl=\"unknown\"}", 1);
    assert_metric(metrics,
                  "ton_quic_app_messages_total{trust=\"trusted\",kind=\"answer\",direction=\"in\",tl=\"unknown\"}", 1);
    assert_metric(metrics, "ton_quic_query_roundtrip_seconds_count{trust=\"trusted\",tl=\"unknown\"}", 1);
    assert_metric(
        metrics, "ton_quic_app_messages_total{trust=\"untrusted\",kind=\"query\",direction=\"out\",tl=\"unknown\"}", 0);
    assert_metric(
        metrics, "ton_quic_app_messages_total{trust=\"untrusted\",kind=\"answer\",direction=\"in\",tl=\"unknown\"}", 0);
    assert_metric(metrics, "ton_quic_query_roundtrip_seconds_count{trust=\"untrusted\",tl=\"unknown\"}", 0);
    assert_ready_connections(metrics, "out", "trusted", 0);
    assert_ready_connections(metrics, "out", "untrusted", 1);
    co_return td::Unit{};
  });
}

TEST(QuicSender, ManyNodes) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    constexpr int n = 5;
    std::vector<TestNode> nodes;
    for (int i = 0; i < n; i++) {
      nodes.push_back(co_await t.create_node("n" + std::to_string(i), next_port()));
    }

    for (int i = 0; i < n; i++) {
      for (int j = 0; j < n; j++) {
        if (i != j) {
          t.add_peer(nodes[i], nodes[j]);
        }
      }
    }

    co_await td::actor::coro_sleep(td::Timestamp::in(0.1));

    std::vector<std::pair<td::actor::StartedTask<td::BufferSlice>, std::string>> tasks;
    for (int i = 0; i < n; i++) {
      for (int j = 0; j < n; j++) {
        if (i != j) {
          std::string msg = "Qfrom" + std::to_string(i) + "to" + std::to_string(j);
          auto [future, promise] = td::actor::StartedTask<td::BufferSlice>::make_bridge();
          td::actor::send_closure(nodes[i].quic_sender, &ton::quic::QuicSender::send_query, nodes[i].id, nodes[j].id,
                                  std::string("Q"), std::move(promise), td::Timestamp::in(10.0), td::BufferSlice(msg));
          tasks.emplace_back(std::move(future), msg);
        }
      }
    }

    for (auto& [task, expected] : tasks) {
      auto result = co_await std::move(task);
      ASSERT_EQ(result.as_slice(), td::Slice(expected));
    }

    co_return td::Unit{};
  });
}

TEST(QuicSender, RestartSender) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    int a_port = next_port();
    auto a_key = make_key(-2);
    auto a = co_await t.create_node("sa", a_port, a_key);

    auto b = co_await t.create_node("sb", next_port());

    t.add_peer(a, b);
    t.add_peer(b, a);

    auto resp1 = co_await t.send_query(a, b, "before");
    ASSERT_EQ(resp1.as_slice(), td::Slice("Qbefore"));

    auto a_id = a.id;
    a.quic_sender.reset();
    a.adnl.reset();
    a.network_manager.reset();
    a.keyring.reset();

    co_await td::actor::coro_sleep(td::Timestamp::in(1.0));

    a = co_await t.create_node("sa2", a_port, a_key);
    ASSERT_EQ(a.id, a_id);

    t.add_peer(a, b);
    t.add_peer(b, a);

    co_await td::actor::coro_sleep(td::Timestamp::in(0.2));

    auto resp2 = co_await t.send_query(a, b, "after");
    ASSERT_EQ(resp2.as_slice(), td::Slice("Qafter"));

    co_return td::Unit{};
  });
}

TEST(QuicSender, RestartResponder) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    LOG(ERROR) << "=== RestartResponder: creating nodes ===";
    auto a = co_await t.create_node("ra", next_port());
    int b_port = next_port();
    auto b_key = make_key(-1);

    auto b = co_await t.create_node("rb", b_port, b_key);

    t.add_peer(a, b);
    t.add_peer(b, a);

    LOG(ERROR) << "=== RestartResponder: sending first query ===";
    auto resp1 = co_await t.send_query(a, b, "before");
    ASSERT_EQ(resp1.as_slice(), td::Slice("Qbefore"));
    LOG(ERROR) << "=== RestartResponder: first query OK ===";

    auto b_id = b.id;
    LOG(ERROR) << "=== RestartResponder: destroying responder ===";
    b.quic_sender.reset();
    b.adnl.reset();
    b.network_manager.reset();
    b.keyring.reset();

    LOG(ERROR) << "=== RestartResponder: sleeping 3s ===";
    co_await td::actor::coro_sleep(td::Timestamp::in(3.0));

    LOG(ERROR) << "=== RestartResponder: recreating responder ===";
    b = co_await t.create_node("rb2", b_port, b_key);
    ASSERT_EQ(b.id, b_id);

    t.add_peer(a, b);
    t.add_peer(b, a);

    LOG(ERROR) << "=== RestartResponder: sleeping 0.2s ===";
    co_await td::actor::coro_sleep(td::Timestamp::in(0.2));

    LOG(ERROR) << "=== RestartResponder: sending query (expected to fail) ===";
    auto no_resp2 = co_await t.send_query_ex(a, b, "after", 50.0, 1024).wrap();
    LOG(ERROR) << "=== RestartResponder: query result: " << (no_resp2.is_ok() ? "OK" : no_resp2.error().to_string())
               << " ===";
    ASSERT_TRUE(no_resp2.is_error());
    // should fail because connection is lost

    // check that connection will be restored
    LOG(ERROR) << "=== RestartResponder: sending second query (should succeed) ===";
    auto resp2 = co_await t.send_query(a, b, "after");
    LOG(ERROR) << "=== RestartResponder: second query OK ===";
    ASSERT_EQ(resp2.as_slice(), td::Slice("Qafter"));

    co_return td::Unit{};
  });
}

TEST(QuicSender, RestartResponderRepeatedly) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    auto a = co_await t.create_node("rra", next_port());
    int b_port = next_port();
    auto b_key = make_key(-12);
    auto b = co_await t.create_node("rrb0", b_port, b_key);

    auto b_id = b.id;

    t.add_peer(a, b);
    t.add_peer(b, a);

    for (int round = 0; round < 2; round++) {
      auto before_query = "before-" + std::to_string(round);
      auto before_response = "Q" + before_query;
      auto resp1 = co_await t.send_query(a, b, before_query);
      ASSERT_EQ(resp1.as_slice(), td::Slice(before_response));

      b.quic_sender.reset();
      b.adnl.reset();
      b.network_manager.reset();
      b.keyring.reset();

      co_await td::actor::coro_sleep(td::Timestamp::in(3.0));

      b = co_await t.create_node("rrb" + std::to_string(round + 1), b_port, b_key);
      ASSERT_EQ(b.id, b_id);

      t.add_peer(a, b);
      t.add_peer(b, a);

      co_await td::actor::coro_sleep(td::Timestamp::in(0.2));

      auto stale_result = co_await t.send_query_ex(a, b, "stale-" + std::to_string(round), 50.0, 1024).wrap();
      ASSERT_TRUE(stale_result.is_error());

      auto after_query = "after-" + std::to_string(round);
      auto after_response = "Q" + after_query;
      auto resp2 = co_await t.send_query(a, b, after_query);
      ASSERT_EQ(resp2.as_slice(), td::Slice(after_response));
    }

    co_return td::Unit{};
  });
}

TEST(QuicSender, SameKey) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    auto shared_key = make_key(-3);

    auto client = co_await t.create_node("client", next_port());
    auto server1 = co_await t.create_node("srv1", next_port(), shared_key);
    auto server2 = co_await t.create_node("srv2", next_port(), shared_key);

    t.add_peer(client, server1);
    t.add_peer(server1, client);

    auto resp1 = co_await t.send_query(client, server1, "to-server1");
    ASSERT_EQ(resp1.as_slice(), td::Slice("Qto-server1"));

    t.add_peer(client, server2);
    t.add_peer(server2, client);

    auto resp2 = co_await t.send_query(client, server2, "to-server2");
    ASSERT_EQ(resp2.as_slice(), td::Slice("Qto-server2"));

    co_return td::Unit{};
  });
}

TEST(QuicSender, ManyStreams) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    constexpr int num_nodes = 4;
    constexpr int streams_per_pair = 50;

    std::vector<TestNode> nodes;
    for (int i = 0; i < num_nodes; i++) {
      nodes.push_back(co_await t.create_node("s" + std::to_string(i), next_port()));
    }

    for (int i = 0; i < num_nodes; i++) {
      for (int j = 0; j < num_nodes; j++) {
        if (i != j) {
          t.add_peer(nodes[i], nodes[j]);
        }
      }
    }

    co_await td::actor::coro_sleep(td::Timestamp::in(0.1));

    auto start = td::Timestamp::now();

    std::vector<td::actor::StartedTask<td::BufferSlice>> tasks;
    int total = 0;
    for (int round = 0; round < streams_per_pair; round++) {
      for (int i = 0; i < num_nodes; i++) {
        for (int j = 0; j < num_nodes; j++) {
          if (i != j) {
            td::BufferSlice data{65};
            data.as_slice()[0] = 'Q';
            td::Random::secure_bytes(data.as_slice().substr(1));
            auto [future, promise] = td::actor::StartedTask<td::BufferSlice>::make_bridge();
            td::actor::send_closure(nodes[i].quic_sender, &ton::quic::QuicSender::send_query, nodes[i].id, nodes[j].id,
                                    std::string("Q"), std::move(promise), td::Timestamp::in(30.0), std::move(data));
            tasks.push_back(std::move(future));
            total++;
          }
        }
      }
    }

    LOG(INFO) << "Launched " << total << " queries";

    int success = 0, errors = 0;
    for (auto& task : tasks) {
      auto result = co_await std::move(task).wrap();
      if (result.is_ok()) {
        success++;
      } else {
        LOG(ERROR) << result.error();
        errors++;
      }
    }

    auto elapsed = td::Timestamp::now().at() - start.at();
    LOG(INFO) << "Success: " << success << ", Errors: " << errors;
    LOG(INFO) << "Time: " << td::format::as_time(elapsed) << ", QPS: " << (success / elapsed);

    ASSERT_EQ(errors, 0);

    co_return td::Unit{};
  });
}

TEST(QuicSender, MoreThan1024SequentialQueries) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    constexpr size_t query_count = 1280;

    auto a = co_await t.create_node("seq-a", next_port());
    auto b = co_await t.create_node("seq-b", next_port());

    t.add_peer(a, b);
    t.add_peer(b, a);

    for (size_t i = 0; i < query_count; i++) {
      auto query = "seq-" + std::to_string(i);
      auto expected = "Q" + query;
      auto response = co_await t.send_query(a, b, query);
      ASSERT_EQ(response.as_slice(), td::Slice(expected));
    }

    co_return td::Unit{};
  });
}

TEST(QuicStreamLimits, LocalStreamCloseDoesNotExtendPeerCredit) {
  run_raw_quic_test([](RawQuicTestRunner& t) -> td::actor::Task<td::Unit> {
    auto options = small_stream_limit_options(1);
    auto client = co_await t.create_endpoint(options);
    auto server = co_await t.create_endpoint(options);
    auto [client_cid, server_cid] = co_await t.connect(client, server);

    for (size_t i = 0; i < 4; i++) {
      auto sid = co_await t.send_and_fin_stream(client, client_cid, static_cast<char>('a' + i));
      co_await t.wait_for_stream_close(client, sid);
      co_await td::actor::coro_sleep(td::Timestamp::in(0.05));
    }

    co_await td::actor::coro_sleep(td::Timestamp::in(0.1));

    auto opened = co_await t.open_streams_until_blocked(server, server_cid, 4);
    ASSERT_EQ(opened, static_cast<size_t>(1));
    co_return td::Unit{};
  });
}

TEST(QuicStreamLimits, PeerBidiCreditReturnsIndividually) {
  run_raw_quic_test([](RawQuicTestRunner& t) -> td::actor::Task<td::Unit> {
    constexpr size_t limit = 4;
    auto options = small_stream_limit_options(limit);
    auto client = co_await t.create_endpoint(options);
    auto server = co_await t.create_endpoint(options);
    auto [client_cid, server_cid] = co_await t.connect(client, server);
    static_cast<void>(server_cid);

    std::vector<ton::quic::QuicStreamID> streams;
    for (size_t i = 0; i < limit; ++i) {
      streams.push_back(co_await t.open_stream_with_retry(client, client_cid));
    }
    ASSERT_EQ(static_cast<size_t>(0), co_await t.open_streams_until_blocked(client, client_cid, 1));

    td::BufferSlice data(1);
    data.as_slice()[0] = 'x';
    auto sent = co_await td::actor::ask(client.server, &ton::quic::QuicServer::send_stream, client_cid, streams.front(),
                                        std::move(data), true);
    ASSERT_EQ(streams.front(), sent);
    co_await t.wait_for_stream_close(client, streams.front());

    static_cast<void>(co_await t.open_stream_with_retry(client, client_cid));
    ASSERT_EQ(static_cast<size_t>(0), co_await t.open_streams_until_blocked(client, client_cid, 1));
    co_return td::Unit{};
  });
}

TEST(QuicStreams, LocalUniResetIsNotClean) {
  run_raw_quic_test([](RawQuicTestRunner& t) -> td::actor::Task<td::Unit> {
    auto options = small_stream_limit_options(1);
    options.max_streams_uni = 1;
    auto client = co_await t.create_endpoint(options);
    auto server = co_await t.create_endpoint(options);
    auto [client_cid, server_cid] = co_await t.connect(client, server);

    auto sid = co_await t.open_stream_with_retry(client, client_cid, ton::quic::StreamDirection::Unidirectional);
    td::BufferSlice data(1);
    data.as_slice()[0] = 'x';
    ASSERT_EQ(sid, co_await td::actor::ask(client.server, &ton::quic::QuicServer::send_stream, client_cid, sid,
                                           std::move(data), false));
    // The guard holds partial chunks internally (the app sees only whole messages), so there is
    // no arrival signal to wait on; give the byte time to land so the server-side stream exists.
    co_await td::actor::coro_sleep(td::Timestamp::in(0.3));

    td::actor::send_closure(server.server, &ton::quic::QuicServer::shutdown_stream, server_cid, sid);
    co_await t.wait_for_stream_close(client, sid);
    auto event = client.state->get_close_event(sid);
    ASSERT_TRUE(event.has_value());
    ASSERT_EQ(ton::quic::StreamInitiator::Local, event->initiator);
    ASSERT_EQ(event->direction, ton::quic::StreamDirection::Unidirectional);
    ASSERT_TRUE(!event->clean);
    co_return td::Unit{};
  });
}

// A peer's unidirectional stream closes before the upper layer dispatches its payload. Its credit is
// held until that dispatch point, then returned immediately: ngtcp2 coalesces cumulative extensions
// when it builds the next MAX_STREAMS frame, so withholding a second upper-layer batch would
// only reduce usable concurrency and could strand a partial batch forever.
TEST(QuicStreamLimits, PeerUniCreditWaitsForDispatchAndReturnsIndividually) {
  run_raw_quic_test([](RawQuicTestRunner& t) -> td::actor::Task<td::Unit> {
    constexpr size_t limit = 4;

    auto options = small_stream_limit_options(1);
    options.max_streams_uni = limit;
    auto client = co_await t.create_endpoint(options);
    auto server = co_await t.create_endpoint(options);
    server.state->hold_peer_streams();
    auto [client_cid, server_cid] = co_await t.connect(client, server);
    static_cast<void>(server_cid);

    // Spend the whole budget, and let every stream run to completion: delivered, finished, closed on
    // both ends. Nothing is left for the transport to wait for.
    for (size_t i = 0; i < limit; i++) {
      auto sid = co_await t.send_and_fin_stream(client, client_cid, static_cast<char>('a' + i),
                                                ton::quic::StreamDirection::Unidirectional);
      co_await t.wait_for_stream_close(client, sid);
    }
    co_await t.wait_until([&] { return server.state->get_held_stream_count() == limit; }, 5.0);

    // The upper layer has not released anything, so no credit may have come back.
    auto opened = co_await t.open_streams_until_blocked(client, client_cid, limit + 4,
                                                        ton::quic::StreamDirection::Unidirectional);
    ASSERT_EQ(static_cast<size_t>(0), opened);

    auto held = server.state->take_held_streams();
    ASSERT_EQ(limit, held.size());

    // One returned credit must make exactly one slot usable; waiting for a second close here was the
    // liveness bug in the removed batch-of-two implementation.
    td::actor::send_closure(server.server.get(), &ton::quic::QuicServer::release_peer_uni_stream_credit, held.front());
    static_cast<void>(
        co_await t.open_stream_with_retry(client, client_cid, ton::quic::StreamDirection::Unidirectional));
    opened = co_await t.open_streams_until_blocked(client, client_cid, 1, ton::quic::StreamDirection::Unidirectional);
    ASSERT_EQ(static_cast<size_t>(0), opened);

    for (size_t i = 1; i < held.size(); ++i) {
      td::actor::send_closure(server.server.get(), &ton::quic::QuicServer::release_peer_uni_stream_credit, held[i]);
    }

    for (size_t i = 1; i < limit; ++i) {
      static_cast<void>(
          co_await t.open_stream_with_retry(client, client_cid, ton::quic::StreamDirection::Unidirectional));
    }
    co_return td::Unit{};
  });
}

TEST(QuicStreamLimits, MoreThan1024SequentialStreamsWorkWithSmallLimit) {
  run_raw_quic_test([](RawQuicTestRunner& t) -> td::actor::Task<td::Unit> {
    constexpr size_t stream_count = 1280;

    auto options = small_stream_limit_options(1);
    auto client = co_await t.create_endpoint(options);
    auto server = co_await t.create_endpoint(options);
    auto [client_cid, server_cid] = co_await t.connect(client, server);
    static_cast<void>(server_cid);

    for (size_t i = 0; i < stream_count; i++) {
      auto sid = co_await t.send_and_fin_stream(client, client_cid, static_cast<char>('A' + (i % 26)));
      co_await t.wait_for_stream_close(client, sid);
    }

    ASSERT_EQ(client.state->get_local_closed_stream_count(), stream_count);
    co_return td::Unit{};
  });
}

TEST(QuicStreamLimits, FailedInboundStreamsReleaseStreamCredit) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    // A stream failed server-side (per-connection reassembly cap here) is reset at the transport, so
    // it must not pin one of the peer's max_streams_bidi slots. With a credit of 1, every failed
    // query has to release its slot or the connection could never carry another stream.
    ton::quic::FloodLimits limits{.max_connection_buffered = 64 << 10, .max_untrusted_buffered = td::uint64{1} << 30};
    auto a = co_await t.create_node("cred-a", next_port());
    auto b =
        co_await t.create_node("cred-b", next_port(), std::nullopt, "127.0.0.1", limits, small_stream_limit_options(1));
    set_big_mtu(a, b);
    t.add_peer(a, b);
    t.add_peer(b, a);
    co_await t.warmup(a, b);

    for (int i = 0; i < 4; i++) {
      co_await td::actor::coro_sleep(td::Timestamp::in(0.1));  // let the previous slot recycle (MAX_STREAMS)
      auto started = td::Timestamp::now();
      ASSERT_TRUE((co_await t.send_query_ex(a, b, std::string(128 << 10, 'x'), 5.0, 1 << 20).wrap()).is_error());
      // The failure must come from b actively resetting the stream; anything near the client's own
      // 5s query deadline means b leaked the stream and the client had to clean up.
      ASSERT_TRUE(td::Timestamp::now().at() - started.at() < 2.0);
    }

    co_await td::actor::coro_sleep(td::Timestamp::in(0.25));
    auto ok = co_await t.send_query_ex(a, b, std::string(16 << 10, 'x'), 5.0, 1 << 20);
    ASSERT_EQ(ok.size(), (16u << 10) + 1);
    co_return td::Unit{};
  });
}

namespace {

ton::adnl::AdnlNodeIdShort short_id_from_key(const td::Ed25519::PrivateKey& key) {
  return ton::adnl::AdnlNodeIdFull(ton::PublicKey(ton::pubkeys::Ed25519(key.get_public_key().move_as_ok())))
      .compute_short_id();
}

}  // namespace

TEST(QuicSniDispatch, ClientSniSelectsAdditionalIdentity) {
  run_raw_quic_test([](RawQuicTestRunner& t) -> td::actor::Task<td::Unit> {
    auto options = quic_test_options();
    auto client = co_await t.create_endpoint(options);
    auto server = co_await t.create_endpoint(options);

    // server endpoint starts with one identity (created in create_endpoint); register a second
    // one. The first remains the default for SNI-less handshakes.
    auto extra_key = make_quic_key(server.port * 31 + 1);
    auto extra_local_id = short_id_from_key(extra_key);
    td::actor::send_closure(server.server, &ton::quic::QuicServer::add_identity, extra_local_id,
                            clone_quic_key(extra_key));
    td::actor::send_closure(server.server, &ton::quic::QuicServer::set_default_mtu, extra_local_id, td::uint64{4096});
    co_await td::actor::Yield{};

    auto extra_sni = ton::quic::ServerIdentity::sni(extra_local_id);
    auto [client_cid, server_cid] = co_await t.connect(client, server, td::Slice(extra_sni));
    static_cast<void>(client_cid);
    static_cast<void>(server_cid);

    auto inbound_local = server.state->get_inbound_local_id();
    ASSERT_TRUE(inbound_local.has_value());
    ASSERT_EQ(*inbound_local, short_id_from_key(extra_key));

    co_return td::Unit{};
  });
}

TEST(QuicSniDispatch, NoSniFallsBackToDefaultIdentity) {
  run_raw_quic_test([](RawQuicTestRunner& t) -> td::actor::Task<td::Unit> {
    auto options = quic_test_options();
    auto client = co_await t.create_endpoint(options);
    auto server = co_await t.create_endpoint(options);

    auto extra_key = make_quic_key(server.port * 31 + 2);
    auto extra_local_id = short_id_from_key(extra_key);
    td::actor::send_closure(server.server, &ton::quic::QuicServer::add_identity, extra_local_id, std::move(extra_key));
    co_await td::actor::Yield{};

    // No SNI — must hit the default identity (the one created with the endpoint).
    auto [client_cid, server_cid] = co_await t.connect(client, server);
    static_cast<void>(client_cid);
    static_cast<void>(server_cid);

    auto inbound_local = server.state->get_inbound_local_id();
    ASSERT_TRUE(inbound_local.has_value());
    ASSERT_EQ(*inbound_local, short_id_from_key(server.key));

    co_return td::Unit{};
  });
}

TEST(QuicSniDispatch, UnknownSniFailsHandshake) {
  run_raw_quic_test([](RawQuicTestRunner& t) -> td::actor::Task<td::Unit> {
    auto options = quic_test_options();
    auto client = co_await t.create_endpoint(options);
    auto server = co_await t.create_endpoint(options);

    // Single identity on the server. If the client sends an SNI for a key that isn't registered,
    // dispatch must reject the handshake rather than silently choosing the default key.
    auto bogus_key = make_quic_key(server.port * 31 + 3);
    auto bogus_local_id = short_id_from_key(bogus_key);

    auto bogus_sni = ton::quic::ServerIdentity::sni(bogus_local_id);
    auto outbound_cid_result =
        co_await td::actor::ask(client.server, &ton::quic::QuicServer::connect, td::Slice("127.0.0.1"), server.port,
                                clone_quic_key(client.key), td::Slice("ton"), td::Slice(bogus_sni))
            .wrap();
    LOG_CHECK(outbound_cid_result.is_ok()) << "connect failed before handshake: " << outbound_cid_result.error();
    auto outbound_cid = outbound_cid_result.move_as_ok();

    co_await t.wait_for_connection_close(client, outbound_cid);
    ASSERT_TRUE(!client.state->get_outbound_local_id().has_value());
    ASSERT_TRUE(!client.state->get_outbound_cid().has_value());
    ASSERT_TRUE(!server.state->get_inbound_local_id().has_value());
    ASSERT_TRUE(!server.state->get_inbound_cid().has_value());

    co_return td::Unit{};
  });
}

TEST(QuicSender, RestartSenderNewPort) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    int a_port = next_port();
    auto a_key = make_key(-10);
    auto a = co_await t.create_node("sa", a_port, a_key);

    auto b = co_await t.create_node("sb", next_port());

    t.add_peer(a, b);
    t.add_peer(b, a);

    auto resp1 = co_await t.send_query(a, b, "before");
    ASSERT_EQ(resp1.as_slice(), td::Slice("Qbefore"));

    auto a_id = a.id;
    a.quic_sender.reset();
    a.adnl.reset();
    a.network_manager.reset();
    a.keyring.reset();

    co_await td::actor::coro_sleep(td::Timestamp::in(1.0));

    // Restart on DIFFERENT port
    int new_port = next_port();
    a = co_await t.create_node("sa2", new_port, a_key);
    ASSERT_EQ(a.id, a_id);

    t.add_peer(a, b);
    t.add_peer(b, a);  // B learns A's new address

    co_await td::actor::coro_sleep(td::Timestamp::in(0.2));

    auto resp2 = co_await t.send_query(a, b, "after");
    ASSERT_EQ(resp2.as_slice(), td::Slice("Qafter"));

    co_return td::Unit{};
  });
}

TEST(QuicSender, RestartResponderNewPort) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    auto a = co_await t.create_node("ra", next_port());
    int b_port = next_port();
    auto b_key = make_key(-11);

    auto b = co_await t.create_node("rb", b_port, b_key);

    t.add_peer(a, b);
    t.add_peer(b, a);

    auto resp1 = co_await t.send_query(a, b, "before");
    ASSERT_EQ(resp1.as_slice(), td::Slice("Qbefore"));

    auto b_id = b.id;
    b.quic_sender.reset();
    b.adnl.reset();
    b.network_manager.reset();
    b.keyring.reset();

    co_await td::actor::coro_sleep(td::Timestamp::in(3.0));

    // Restart on DIFFERENT port
    int new_port = next_port();
    b = co_await t.create_node("rb2", new_port, b_key);
    ASSERT_EQ(b.id, b_id);

    t.add_peer(a, b);  // A learns B's new address - but QuicSender has cached old connection
    t.add_peer(b, a);

    co_await td::actor::coro_sleep(td::Timestamp::in(0.2));

    auto no_resp2 = co_await t.send_query(a, b, "after").wrap();
    ASSERT_TRUE(no_resp2.is_error());
    // Same logic as in RestartResponder

    auto resp2 = co_await t.send_query(a, b, "after");
    ASSERT_EQ(resp2.as_slice(), td::Slice("Qafter"));

    co_return td::Unit{};
  });
}

TEST(QuicSender, WrongPublicKey) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    auto a = co_await t.create_node("wa", next_port());
    auto b = co_await t.create_node("wb", next_port());

    // A knows about B with WRONG key (use different key than B actually has)
    auto wrong_key = make_key(-100);
    auto wrong_addr_list = make_addr_list(b.ip, b.port);
    td::actor::send_closure(a.adnl, &ton::adnl::Adnl::add_peer, a.id,
                            ton::adnl::AdnlNodeIdFull{wrong_key.compute_public_key()}, wrong_addr_list);

    t.add_peer(b, a);  // B knows correct A

    // Try to send query - A thinks it's talking to wrong_key but B has different key
    auto wrong_id = ton::adnl::AdnlNodeIdShort{wrong_key.compute_public_key().compute_short_id()};

    td::BufferSlice query(6);
    query.as_slice().copy_from(td::Slice("Qtest"));

    auto [task, promise] = td::actor::StartedTask<td::BufferSlice>::make_bridge();
    td::actor::send_closure(a.quic_sender, &ton::quic::QuicSender::send_query, a.id, wrong_id, std::string("Q"),
                            std::move(promise), td::Timestamp::in(5.0), std::move(query));
    auto result = co_await std::move(task).wrap();

    // Should fail - key mismatch
    LOG(INFO) << "WrongPublicKey result: " << (result.is_ok() ? "OK (unexpected)" : result.error().to_string());
    ASSERT_TRUE(result.is_error());

    co_await td::actor::coro_sleep(td::Timestamp::in(1));

    co_return td::Unit{};
  });
}

TEST(QuicSender, TlsKeyMismatchDoesNotPoisonOutboundPath) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    auto a = co_await t.create_node("poison-a", next_port());
    auto b = co_await t.create_node("poison-b", next_port());
    auto c = co_await t.create_node("poison-c", next_port());

    t.set_static_response(b, "P", "victim");
    t.set_static_response(c, "P", "attacker");

    auto wrong_addr_list = make_addr_list(c.ip, c.port);
    td::actor::send_closure(a.adnl, &ton::adnl::Adnl::add_peer, a.id,
                            ton::adnl::AdnlNodeIdFull{b.key.compute_public_key()}, wrong_addr_list);
    t.add_peer(b, a);
    t.add_peer(c, a);

    auto mismatch = co_await t.send_prefixed_query_ex(a, b, "P", "first", 5.0, 1024).wrap();
    ASSERT_TRUE(mismatch.is_error());

    // Allow on_closed to remove the mismatched outbound connection before retrying.
    co_await td::actor::coro_sleep(td::Timestamp::in(1));

    auto mismatch_again = co_await t.send_prefixed_query_ex(a, b, "P", "first", 5.0, 1024).wrap();
    ASSERT_TRUE(mismatch_again.is_error());

    auto correct_addr_list = make_addr_list(b.ip, b.port);
    correct_addr_list.set_version(wrong_addr_list.version() + 1);
    td::actor::send_closure(a.adnl, &ton::adnl::Adnl::add_peer, a.id,
                            ton::adnl::AdnlNodeIdFull{b.key.compute_public_key()}, correct_addr_list);
    co_await td::actor::ask(a.adnl, &ton::adnl::Adnl::get_peer_node, a.id, b.id);

    // The path was briefly backed off by the two failed dials; once the backoff window elapses the
    // corrected address connects — the path was never permanently poisoned. (Fail-fast retries don't
    // dial, so they don't grow the backoff.)
    auto deadline = td::Timestamp::in(10.0);
    while (true) {
      auto r = co_await t.send_prefixed_query_ex(a, b, "P", "second", 5.0, 1024).wrap();
      if (r.is_ok()) {
        ASSERT_EQ(r.ok().as_slice(), td::Slice("victim"));
        break;
      }
      ASSERT_TRUE(!deadline.is_in_past());
      co_await td::actor::coro_sleep(td::Timestamp::in(0.3));
    }
    co_return td::Unit{};
  });
}

TEST(QuicSender, LargeScale) {
  if (g_config.large_nodes == 0) {
    LOG(INFO) << "Skipping LargeScale test (use -N to set node count)";
    return;
  }
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    int num_nodes = g_config.large_nodes;
    int queries_per_dest = g_config.large_queries;
    int query_size = g_config.large_size;

    LOG(INFO) << "Creating " << num_nodes << " nodes...";
    std::vector<TestNode> nodes;
    for (int i = 0; i < num_nodes; i++) {
      nodes.push_back(co_await t.create_node("L" + std::to_string(i), next_port()));
      if ((i + 1) % 100 == 0) {
        LOG(INFO) << "Created " << (i + 1) << " nodes";
      }
      td::actor::send_closure(nodes[i].quic_sender, &ton::quic::QuicSender::set_default_mtu, 2 * query_size);
    }

    LOG(INFO) << "Setting up peer connections...";
    for (int i = 0; i < num_nodes; i++) {
      for (int j = 0; j < num_nodes; j++) {
        if (i != j) {
          t.add_peer(nodes[i], nodes[j]);
          // These are validator-like peers: each trusts the others, so their queries draw on the
          // unlimited trusted in-flight budget rather than the bounded untrusted one.
          t.trust_peer(nodes[i], nodes[j], 2 * query_size);
        }
      }
    }

    co_await td::actor::coro_sleep(td::Timestamp::in(0.5));

    auto start = td::Timestamp::now();
    int sender_idx = 0;

    std::vector<td::actor::StartedTask<td::BufferSlice>> tasks;
    td::uint64 total_bytes = 0;

    LOG(INFO) << "Launching queries: " << queries_per_dest << " x " << (num_nodes - 1) << " destinations x "
              << query_size << " bytes";

    for (int dest = 0; dest < num_nodes; dest++) {
      if (dest == sender_idx)
        continue;

      for (int q = 0; q < queries_per_dest; q++) {
        td::BufferSlice data(query_size);
        data.as_slice()[0] = 'Q';
        td::Random::secure_bytes(data.as_slice().substr(1));
        total_bytes += query_size;

        auto [future, promise] = td::actor::StartedTask<td::BufferSlice>::make_bridge();
        td::actor::send_closure(nodes[sender_idx].quic_sender, &ton::quic::QuicSender::send_query, nodes[sender_idx].id,
                                nodes[dest].id, std::string("Q"), std::move(promise), td::Timestamp::in(120.0),
                                std::move(data));
        tasks.push_back(std::move(future));
      }
    }

    int total = static_cast<int>(tasks.size());
    LOG(INFO) << "Launched " << total << " queries, " << (total_bytes / 1024 / 1024) << " MB total";

    int success = 0, errors = 0;
    for (auto& task : tasks) {
      auto result = co_await std::move(task).wrap();
      if (result.is_ok()) {
        success++;
      } else {
        LOG(ERROR) << result.error();
        errors++;
      }
      if ((success + errors) % 1000 == 0) {
        LOG(INFO) << "Progress: " << (success + errors) << "/" << total;
      }
    }

    auto elapsed = td::Timestamp::now().at() - start.at();
    double mbps = (static_cast<double>(total_bytes) / 1024.0 / 1024.0) / elapsed;

    LOG(INFO) << "Success: " << success << ", Errors: " << errors;
    LOG(INFO) << "Time: " << td::format::as_time(elapsed);
    LOG(INFO) << "QPS: " << (success / elapsed) << ", Throughput: " << mbps << " MB/s";

    ASSERT_EQ(errors, 0);

    co_return td::Unit{};
  });
}

TEST(QuicSender, EmptyMessage) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    auto a = co_await t.create_node("em-a", next_port());
    auto b = co_await t.create_node("em-b", next_port());

    t.add_peer(a, b);
    t.add_peer(b, a);

    t.send_message(a, b, "");

    co_await td::actor::coro_sleep(td::Timestamp::in(1.0));
    ASSERT_EQ(1u, b.received_messages->size());

    // If we get here without crashing, the test passes
    co_return td::Unit{};
  });
}

TEST(QuicSender, SingleDataChunkThenEmptyFin) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    auto receiver = co_await t.create_node("split-fin", next_port());
    auto raw = co_await RawQuicTestRunner::create_endpoint(quic_test_options());

    auto sni = ton::quic::ServerIdentity::sni(receiver.id);
    auto cid_result =
        co_await td::actor::ask(raw.server, &ton::quic::QuicServer::connect, td::Slice("127.0.0.1"),
                                receiver.port + 1000, clone_quic_key(raw.key), td::Slice("ton"), td::Slice(sni))
            .wrap();
    ASSERT_TRUE(cid_result.is_ok());
    auto cid = cid_result.move_as_ok();
    co_await t.wait_until([&] { return raw.state->get_outbound_cid().has_value(); }, 5.0);

    auto sid_result =
        co_await td::actor::ask(raw.server, &ton::quic::QuicServer::open_stream, cid, ton::quic::StreamOptions{})
            .wrap();
    ASSERT_TRUE(sid_result.is_ok());
    auto sid = sid_result.move_as_ok();
    raw.state->remember_local_stream(sid);

    auto wire = ton::create_serialize_tl_object<ton::ton_api::quic_message>(td::BufferSlice(td::Slice("Mx")));
    auto wire_size = wire.size();
    auto data_result =
        co_await td::actor::ask(raw.server, &ton::quic::QuicServer::send_stream, cid, sid, std::move(wire), false)
            .wrap();
    ASSERT_TRUE(data_result.is_ok());
    ASSERT_EQ(data_result.move_as_ok(), sid);

    auto ack_deadline = td::Timestamp::in(5.0);
    while (true) {
      auto stats = co_await td::actor::ask(raw.server, &ton::quic::QuicServer::collect);
      auto acked = stats.transport.summary.stats.stream_bytes.at(ton::metrics::Direction::out).value();
      if (acked >= wire_size) {
        break;
      }
      ASSERT_TRUE(!ack_deadline.is_in_past());
      co_await td::actor::coro_sleep(td::Timestamp::in(0.001));
    }
    ASSERT_EQ(0u, receiver.received_messages->size());

    auto fin_result =
        co_await td::actor::ask(raw.server, &ton::quic::QuicServer::send_stream, cid, sid, td::BufferSlice{}, true)
            .wrap();
    ASSERT_TRUE(fin_result.is_ok());
    ASSERT_EQ(fin_result.move_as_ok(), sid);

    co_await t.wait_until([&] { return receiver.received_messages->size() == 1; }, 5.0);
    ASSERT_EQ(receiver.received_messages->get(0).as_slice(), td::Slice("Mx"));
    // Delivery is acked back to the peer after the subscriber took the message, so the receipt is
    // still in flight here; let it land before the scheduler stops.
    co_await td::actor::coro_sleep(td::Timestamp::in(0.2));
    co_return td::Unit{};
  });
}

// A peer deployed before messages moved onto unidirectional streams advertises no unidirectional
// credit and never sends a MAX_STREAMS_UNI, so every one-way open would fail forever. The message
// must use the old bidirectional wire behavior instead.
TEST(QuicMessageStreams, PeerWithoutUniCreditGetsBidirectionalMessage) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    auto old_peer = quic_test_options();
    old_peer.max_streams_uni = 0;
    old_peer.max_streams_bidi = 1;

    auto a = co_await t.create_node("uniold-a", next_port());
    auto b = co_await t.create_node("uniold-b", next_port(), std::nullopt, "127.0.0.1", {}, old_peer);

    t.add_peer(a, b);
    t.add_peer(b, a);

    t.send_message(a, b, "bidi");
    co_await t.wait_until([&] { return b.received_messages->size() == 1; }, 30.0);

    co_return td::Unit{};
  });
}

// The both-new case, stated as the peer refusing everything else: with no bidirectional credit at
// all, a message can only arrive on a unidirectional stream.
TEST(QuicMessageStreams, PeerWithUniCreditGetsUnidirectionalMessage) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    auto uni_only = quic_test_options();
    uni_only.max_streams_bidi = 0;

    auto a = co_await t.create_node("uninew-a", next_port());
    auto b = co_await t.create_node("uninew-b", next_port(), std::nullopt, "127.0.0.1", {}, uni_only);

    t.add_peer(a, b);
    t.add_peer(b, a);

    t.send_message(a, b, "uni");
    co_await t.wait_until([&] { return b.received_messages->size() == 1; }, 30.0);

    co_return td::Unit{};
  });
}

TEST(QuicMessageStreams, ConfirmationDistinguishesAckFromReset) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    auto sender = co_await t.create_node("confirm-sender", next_port());
    auto peer = co_await RawQuicTestRunner::create_endpoint(quic_test_options());
    auto peer_full_id =
        ton::adnl::AdnlNodeIdFull(ton::PublicKey(ton::pubkeys::Ed25519(peer.key.get_public_key().move_as_ok())));
    auto peer_id = peer_full_id.compute_short_id();

    auto addresses = make_addr_list("127.0.0.1", peer.port);
    td::IPAddress quic_address;
    quic_address.init_host_port(PSTRING() << "127.0.0.1:" << peer.port).ensure();
    addresses.add_quic_addr(quic_address).ensure();
    td::actor::send_closure(sender.adnl, &ton::adnl::Adnl::add_peer, sender.id, peer_full_id, std::move(addresses));

    td::actor::send_closure(sender.quic_sender, &ton::quic::QuicSender::send_message, sender.id, peer_id,
                            td::BufferSlice(td::Slice("accepted")));
    co_await t.wait_until([&] { return peer.state->get_delivered_payloads() == 1; }, 5.0);

    // Over the raw endpoint's 4 KB unsolicited-stream bound (its default mtu): the peer's guard
    // fails the stream mid-reassembly and resets it, which the sender must record as a failed
    // confirmation — distinguishable from the accepted message's ack.
    td::BufferSlice rejected(32 << 10);
    rejected.as_slice().fill('x');
    td::actor::send_closure(sender.quic_sender, &ton::quic::QuicSender::send_message, sender.id, peer_id,
                            std::move(rejected));
    co_await t.wait_until([&] { return peer.state->get_rejected_streams() == 1; }, 5.0);

    std::string rendered;
    auto deadline = td::Timestamp::in(5.0);
    while (true) {
      ton::metrics::Sink sink;
      co_await td::actor::ask(sender.quic_sender, &ton::quic::QuicSender::collect, ton::metrics::Context(sink));
      rendered = std::move(sink).build().render();
      if (rendered.find("quic_message_confirmation_seconds_count{trust=\"untrusted\",tl=\"unknown\"} 2") !=
          std::string::npos) {
        break;
      }
      ASSERT_TRUE(!deadline.is_in_past());
      co_await td::actor::yield_on_current();
    }
    ASSERT_TRUE(rendered.find("quic_message_confirmation_failed_total{trust=\"untrusted\",tl=\"unknown\"} 1") !=
                std::string::npos);
    co_return td::Unit{};
  });
}

TEST(QuicDatagram, MessageTransportSelection) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    auto options = quic_test_options();
    options.max_datagram_frame_size = 64;
    auto a = co_await t.create_node("dg-a", next_port(), std::nullopt, "127.0.0.1", {}, options);
    auto b = co_await t.create_node("dg-b", next_port(), std::nullopt, "127.0.0.1", {}, options);
    auto c = co_await t.create_node("dg-c", next_port(), std::nullopt, "127.0.0.1", {}, quic_test_options());

    t.add_peer(a, b);
    t.add_peer(b, a);
    t.add_peer(a, c);
    t.add_peer(c, a);

    t.send_message(a, b, "small");
    t.send_message(a, b, std::string(200, 'L'));
    t.send_message(a, c, "stream");
    t.send_message(c, a, "stream");

    co_await t.wait_until(
        [&] {
          return a.received_messages->size() == 1 && b.received_messages->size() == 2 &&
                 c.received_messages->size() == 1;
        },
        30.0);

    auto sender = co_await t.quic_metrics(a);
    auto receiver = co_await t.quic_metrics(b);
    auto legacy_receiver = co_await t.quic_metrics(c);
    ASSERT_TRUE(sender.find("quic_transport_datagrams_total{direction=\"out\"} 1.000000\n") != std::string::npos);
    ASSERT_TRUE(sender.find("quic_transport_datagrams_total{direction=\"in\"} 0.000000\n") != std::string::npos);
    ASSERT_TRUE(receiver.find("quic_transport_datagrams_total{direction=\"in\"} 1.000000\n") != std::string::npos);
    ASSERT_TRUE(legacy_receiver.find("quic_transport_datagrams_total{direction=\"in\"} 0.000000\n") !=
                std::string::npos);

    co_return td::Unit{};
  });
}

TEST(QuicSender, ResponseSizeLimit) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    auto a = co_await t.create_node("lim-a", next_port());
    auto b = co_await t.create_node("lim-b", next_port());

    t.add_peer(a, b);
    t.add_peer(b, a);

    // First, verify normal query works
    auto resp1 = co_await t.send_query(a, b, "normal");
    ASSERT_EQ(resp1.as_slice(), td::Slice("Qnormal"));

    // Send large query with small response size limit
    // Query data is 10000 bytes, response will be same size (echo), but limit is 2000
    std::string large_data(10000, 'X');
    auto result = co_await t.send_query_ex(a, b, large_data, 10.0, 2000).wrap();

    LOG(INFO) << "ResponseSizeLimit result: " << (result.is_ok() ? "OK (unexpected)" : result.error().to_string());
    ASSERT_TRUE(result.is_error());

    // Verify connection still works after the failed query
    for (int i = 0; i < 5; i++) {
      auto resp = co_await t.send_query(a, b, "after-" + std::to_string(i));
      ASSERT_EQ(resp.as_slice(), td::Slice("Qafter-" + std::to_string(i)));
    }

    LOG(INFO) << "Connection still works after size limit exceeded";
    co_return td::Unit{};
  });
}

TEST(QuicSender, ResponseSizeLimitDoesNotWaitForTimeout) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    auto a = co_await t.create_node("lim-fast-a", next_port());
    auto b = co_await t.create_node("lim-fast-b", next_port());

    t.add_peer(a, b);
    t.add_peer(b, a);

    auto resp1 = co_await t.send_query(a, b, "normal");
    ASSERT_EQ(resp1.as_slice(), td::Slice("Qnormal"));

    // This regression guards the stream_close -> on_stream_closed path.
    // Without it, the query waits for its timeout instead of failing when the stream is closed.
    std::string large_data(10000, 'X');
    td::BufferSlice query(1 + large_data.size());
    query.as_slice()[0] = 'Q';
    query.as_slice().substr(1).copy_from(large_data);

    auto result = std::make_shared<std::optional<td::Result<td::BufferSlice>>>();
    td::actor::send_closure(
        a.quic_sender, &ton::quic::QuicSender::send_query_ex, a.id, b.id, std::string("Q"),
        td::make_promise([result](td::Result<td::BufferSlice> r) mutable { *result = std::move(r); }),
        td::Timestamp::in(20.0), std::move(query), 2000);

    co_await t.wait_until([result] { return result->has_value(); }, 3.0);

    ASSERT_TRUE(result->has_value());
    LOG(INFO) << "ResponseSizeLimitDoesNotWaitForTimeout result: "
              << (result->value().is_ok() ? "OK (unexpected)" : result->value().error().to_string());
    ASSERT_TRUE(result->value().is_error());
    ASSERT_TRUE(result->value().error().message().str().find("timeout") == std::string::npos);

    auto resp2 = co_await t.send_query(a, b, "after");
    ASSERT_EQ(resp2.as_slice(), td::Slice("Qafter"));
    co_return td::Unit{};
  });
}

TEST(QuicSender, SinglePacketResponseSizeLimitRemovesTimeout) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    auto a = co_await t.create_node("lim-whole-a", next_port());
    auto b = co_await t.create_node("lim-whole-b", next_port());

    t.add_peer(a, b);
    t.add_peer(b, a);

    auto warmup = co_await t.send_query(a, b, "warmup");
    ASSERT_EQ(warmup.as_slice(), td::Slice("Qwarmup"));

    std::string data = "one-packet";
    td::BufferSlice answer_data(1 + data.size());
    answer_data.as_slice()[0] = 'Q';
    answer_data.as_slice().substr(1).copy_from(data);
    auto wire_answer = ton::create_serialize_tl_object<ton::ton_api::quic_answer>(std::move(answer_data));

    auto result = co_await t.send_query_ex(a, b, data, 1.0, wire_answer.size() - 1).wrap();
    ASSERT_TRUE(result.is_error());
    ASSERT_TRUE(result.error().message().str().find("stream size limit exceeded") != std::string::npos);

    // The whole-stream path erased an armed timeout together with the stream state. Let its old
    // deadline pass to catch a stale heap pointer, then verify that the connection still works.
    co_await td::actor::coro_sleep(td::Timestamp::in(1.1));
    auto response = co_await t.send_query(a, b, "after");
    ASSERT_EQ(response.as_slice(), td::Slice("Qafter"));
    co_return td::Unit{};
  });
}

TEST(QuicSender, ResponseSizeLimitByOneDoesNotWaitForTimeout) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    auto a = co_await t.create_node("lim-plus-one-a", next_port());
    auto b = co_await t.create_node("lim-plus-one-b", next_port());
    td::actor::send_closure(a.quic_sender, &ton::quic::QuicSender::set_default_mtu, 1 << 20);
    td::actor::send_closure(b.quic_sender, &ton::quic::QuicSender::set_default_mtu, 1 << 20);

    t.add_peer(a, b);
    t.add_peer(b, a);

    auto resp1 = co_await t.send_query(a, b, "normal");
    ASSERT_EQ(resp1.as_slice(), td::Slice("Qnormal"));

    // Keep the response large enough to exercise the limit-by-one failure on a multi-packet answer.
    std::string data(300 << 10, 'X');
    td::BufferSlice answer_data(1 + data.size());
    answer_data.as_slice()[0] = 'Q';
    answer_data.as_slice().substr(1).copy_from(data);
    auto wire_answer = ton::create_serialize_tl_object<ton::ton_api::quic_answer>(answer_data.clone());
    auto max_answer_size = static_cast<td::uint64>(wire_answer.size() - 1);

    auto result = std::make_shared<std::optional<td::Result<td::BufferSlice>>>();
    td::BufferSlice query(1 + data.size());
    query.as_slice()[0] = 'Q';
    query.as_slice().substr(1).copy_from(data);
    td::actor::send_closure(
        a.quic_sender, &ton::quic::QuicSender::send_query_ex, a.id, b.id, std::string("Q"),
        td::make_promise([result](td::Result<td::BufferSlice> r) mutable { *result = std::move(r); }),
        td::Timestamp::in(20.0), std::move(query), max_answer_size);

    co_await t.wait_until([result] { return result->has_value(); }, 3.0);

    ASSERT_TRUE(result->has_value());
    LOG(INFO) << "ResponseSizeLimitByOneDoesNotWaitForTimeout result: "
              << (result->value().is_ok() ? "OK (unexpected)" : result->value().error().to_string());
    ASSERT_TRUE(result->value().is_error());
    ASSERT_TRUE(result->value().error().message().str().find("timeout") == std::string::npos);

    auto resp2 = co_await t.send_query(a, b, "after");
    ASSERT_EQ(resp2.as_slice(), td::Slice("Qafter"));
    co_return td::Unit{};
  });
}

TEST(QuicSender, LargeQueryWithSmallLimit) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    auto a = co_await t.create_node("lg-a", next_port());
    auto b = co_await t.create_node("lg-b", next_port());

    t.add_peer(a, b);
    t.add_peer(b, a);

    // First, verify normal query works
    auto resp1 = co_await t.send_query(a, b, "normal");
    ASSERT_EQ(resp1.as_slice(), td::Slice("Qnormal"));

    // Send very large query (1MB) with small response limit (100 bytes)
    // This should cause buffering and potentially trigger -219 when stream is shutdown
    std::string huge_data(1 << 20, 'X');  // 1MB
    auto result = co_await t.send_query_ex(a, b, huge_data, 30.0, 2000).wrap();

    LOG(INFO) << "LargeQueryWithSmallLimit result: "
              << (result.is_ok() ? "OK (unexpected)" : result.error().to_string());
    ASSERT_TRUE(result.is_error());

    // Verify connection still works after the failed query
    for (int i = 0; i < 5; i++) {
      auto resp = co_await t.send_query(a, b, "after-" + std::to_string(i));
      ASSERT_EQ(resp.as_slice(), td::Slice("Qafter-" + std::to_string(i)));
    }

    LOG(INFO) << "Connection still works after large query with small limit";
    co_return td::Unit{};
  });
}

TEST(QuicSender, ResponseTimeout) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    auto a = co_await t.create_node("to-a", next_port());
    auto b = co_await t.create_node("to-b", next_port());

    t.add_peer(a, b);
    t.add_peer(b, a);

    // Set up slow echo on node b (5 second delay)
    t.set_slow_echo(b, 5.0);

    // First, verify normal query works
    auto resp1 = co_await t.send_query(a, b, "normal");
    ASSERT_EQ(resp1.as_slice(), td::Slice("Qnormal"));

    // Send query to slow endpoint with short timeout (1 second timeout, 5 second response delay)
    // Use large max_answer_size so we test timeout, not size limit
    auto result = co_await t.send_slow_query_ex(a, b, "slow", 1.0, 1 << 20).wrap();

    LOG(INFO) << "ResponseTimeout result: " << (result.is_ok() ? "OK (unexpected)" : result.error().to_string());
    ASSERT_TRUE(result.is_error());

    // Wait for the actual server-side handler completion, which survives the client's timeout.
    co_await wait_for_receiver_inflight(t, b);

    // Verify connection still works after the timeout
    for (int i = 0; i < 5; i++) {
      auto resp = co_await t.send_query(a, b, "after-" + std::to_string(i));
      ASSERT_EQ(resp.as_slice(), td::Slice("Qafter-" + std::to_string(i)));
    }

    LOG(INFO) << "Connection still works after timeout";
    co_return td::Unit{};
  });
}

TEST(QuicSender, NoResponseTimeout) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    auto a = co_await t.create_node("nrt-a", next_port());
    auto b = co_await t.create_node("nrt-b", next_port());

    t.add_peer(a, b);
    t.add_peer(b, a);

    // Set up never-respond callback on node b
    t.set_never_respond(b);

    // First, verify normal query works
    auto resp1 = co_await t.send_query(a, b, "normal");
    ASSERT_EQ(resp1.as_slice(), td::Slice("Qnormal"));

    // Send query to never-respond endpoint with 2 second timeout
    auto start = td::Timestamp::now();
    auto result = co_await t.send_never_respond_query(a, b, "waiting", 2.0, 1 << 20).wrap();
    auto elapsed = td::Timestamp::now().at() - start.at();

    LOG(INFO) << "NoResponseTimeout result: " << (result.is_ok() ? "OK (unexpected)" : result.error().to_string());
    LOG(INFO) << "Elapsed: " << elapsed << "s (expected ~2s)";

    ASSERT_TRUE(result.is_error());
    auto msg = result.error().message().str();
    ASSERT_TRUE(msg.find("timeout") != std::string::npos);
    ASSERT_TRUE(elapsed >= 1.0 && elapsed < 10.0);

    // Verify connection still works after the timeout
    for (int i = 0; i < 3; i++) {
      auto resp = co_await t.send_query(a, b, "after-" + std::to_string(i));
      ASSERT_EQ(resp.as_slice(), td::Slice("Qafter-" + std::to_string(i)));
    }

    LOG(INFO) << "Connection still works after no-response timeout";
    // The assertion exercised a handler that never answers. Explicitly remove the test callback
    // while the scheduler is live so its held Promise fails and the receiver can release the work.
    co_await td::actor::ask(b.adnl, &ton::adnl::Adnl::unsubscribe, b.id, std::string("X"));
    co_await wait_for_receiver_inflight(t, b);
    co_return td::Unit{};
  });
}

TEST(QuicSender, QueryExpiredDeadline) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    auto a = co_await t.create_node("qed-a", next_port());
    auto b = co_await t.create_node("qed-b", next_port());

    td::Timer timer;
    auto cold = co_await t.send_query_ex(a, b, "cold", -0.1, 1 << 20).wrap();
    assert_timeout(cold);
    ASSERT_TRUE(timer.elapsed() < 1.0);

    t.add_peer(a, b);
    t.add_peer(b, a);
    auto resp = co_await t.send_query(a, b, "warm");
    ASSERT_EQ(resp.as_slice(), td::Slice("Qwarm"));

    auto warm = co_await t.send_query_ex(a, b, "warm", -0.1, 1 << 20).wrap();
    assert_timeout(warm);
    co_return td::Unit{};
  });
}

TEST(QuicSender, ExpiredWaiterReleasesOnlyUnstartedConnection) {
  run_test([](TestRunner&) -> td::actor::Task<> {
    auto sender = td::actor::create_actor<ton::quic::QuicSenderTest>("waiter-ownership");
    co_await td::actor::ask(sender, &ton::quic::QuicSenderTest::expired_waiter_ownership);
    co_return {};
  });
}

TEST(QuicSender, ConnectionWaiterDeadlines) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    auto a = co_await t.create_node("qwd-a", next_port());
    TestNode peer;
    peer.port = next_port();
    peer.key = make_key(peer.port);
    peer.id = ton::adnl::AdnlNodeIdShort{peer.key.compute_public_key().compute_short_id()};
    t.add_peer(a, peer);

    auto after_late_deadline = td::Timestamp::in(4.7);
    auto late = t.send_query_ex(a, peer, "late", 4.5, 1 << 20).start_immediate();
    auto middle = t.send_query_ex(a, peer, "middle", 0.8, 1 << 20).start_immediate();
    auto early = co_await t.send_query_ex(a, peer, "early", 0.3, 1 << 20).wrap();
    assert_timeout(early);

    auto middle_result = co_await std::move(middle).wrap();
    assert_timeout(middle_result);

    auto b = co_await t.create_node("qwd-b", peer.port);
    t.add_peer(a, b);
    t.add_peer(b, a);
    auto late_result = co_await std::move(late);
    ASSERT_EQ(late_result.as_slice(), td::Slice("Qlate"));

    co_await td::actor::coro_sleep(after_late_deadline);
    auto after = co_await t.send_query(a, b, "after");
    ASSERT_EQ(after.as_slice(), td::Slice("Qafter"));
    co_return td::Unit{};
  });
}

TEST(QuicSender, ConnectionWaiterLimitCountsLiveCallers) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    auto a = co_await t.create_node("qwl-a", next_port());
    TestNode peer;
    peer.port = next_port();
    peer.key = make_key(peer.port);
    peer.id = ton::adnl::AdnlNodeIdShort{peer.key.compute_public_key().compute_short_id()};
    t.add_peer(a, peer);

    auto oldest = t.send_query_ex(a, peer, "oldest", 30.0, 1 << 20).start_immediate();
    std::vector<td::actor::StartedTask<td::BufferSlice>> crowd;
    crowd.reserve(1023);
    for (size_t i = 0; i < 1023; i++) {
      crowd.push_back(t.send_query_ex(a, peer, "crowd", 2.0, 1 << 20).start_immediate());
    }
    co_await t.collect_quic_metrics(a);  // Sender mailbox barrier.
    ASSERT_TRUE(!oldest.await_ready());
    for (auto& task : crowd) {
      ASSERT_TRUE(!task.await_ready());
    }

    auto replacement = t.send_query_ex(a, peer, "replacement", 30.0, 1 << 20).start_immediate();
    auto evicted = co_await std::move(oldest).wrap();
    ASSERT_TRUE(evicted.is_error());
    ASSERT_EQ(evicted.error().message(), td::Slice("dropped waiting for a connection"));

    for (auto& task : crowd) {
      assert_timeout(co_await std::move(task).wrap());
    }
    auto late = t.send_query_ex(a, peer, "late", 30.0, 1 << 20).start_immediate();
    co_await t.collect_quic_metrics(a);  // Expired slots must have been reclaimed.
    ASSERT_TRUE(!replacement.await_ready());
    ASSERT_TRUE(!late.await_ready());

    auto b = co_await t.create_node("qwl-b", peer.port);
    t.add_peer(a, b);
    t.add_peer(b, a);
    auto replacement_result = co_await std::move(replacement);
    ASSERT_EQ(replacement_result.as_slice(), td::Slice("Qreplacement"));
    auto late_result = co_await std::move(late);
    ASSERT_EQ(late_result.as_slice(), td::Slice("Qlate"));
    co_return td::Unit{};
  });
}

// ============================================================================
// Fairness tests - verify fair bandwidth distribution across connections/streams
// Note: These tests verify round-robin scheduling but can't truly test fairness
// on localhost since the socket never blocks (fairness matters under contention)
// ============================================================================

// Test: Two connections sending large data should get approximately equal bandwidth
TEST(QuicFairness, TwoConnectionsFairBandwidth) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    // Both senders send many large queries simultaneously
    // Target: ~50MB per sender to saturate localhost (~200MB/s)
    constexpr int queries_per_sender = 100;
    constexpr int query_size = 1024 * 512;  // 0.5 MB each = 50 MB total per sender

    // Create a "hub" node that will receive from two senders
    auto hub = co_await t.create_node("hub", next_port());
    td::actor::send_closure(hub.quic_sender, &ton::quic::QuicSender::set_default_mtu, 2 * query_size);

    // Create two sender nodes
    auto sender1 = co_await t.create_node("s1", next_port());
    auto sender2 = co_await t.create_node("s2", next_port());
    td::actor::send_closure(sender1.quic_sender, &ton::quic::QuicSender::set_default_mtu, 2 * query_size);
    td::actor::send_closure(sender2.quic_sender, &ton::quic::QuicSender::set_default_mtu, 2 * query_size);

    // Set up peer connections
    t.add_peer(sender1, hub);
    t.add_peer(sender2, hub);
    t.add_peer(hub, sender1);
    t.add_peer(hub, sender2);

    co_await td::actor::coro_sleep(td::Timestamp::in(0.2));

    std::vector<td::actor::StartedTask<td::BufferSlice>> tasks1, tasks2;
    auto start = td::Timestamp::now();

    // Launch queries from sender1
    for (int i = 0; i < queries_per_sender; i++) {
      td::BufferSlice data(query_size);
      data.as_slice()[0] = 'Q';
      td::Random::secure_bytes(data.as_slice().substr(1));
      auto [future, promise] = td::actor::StartedTask<td::BufferSlice>::make_bridge();
      td::actor::send_closure(sender1.quic_sender, &ton::quic::QuicSender::send_query, sender1.id, hub.id,
                              std::string("Q"), std::move(promise), td::Timestamp::in(60.0), std::move(data));
      tasks1.push_back(std::move(future));
    }

    // Launch queries from sender2
    for (int i = 0; i < queries_per_sender; i++) {
      td::BufferSlice data(query_size);
      data.as_slice()[0] = 'Q';
      td::Random::secure_bytes(data.as_slice().substr(1));
      auto [future, promise] = td::actor::StartedTask<td::BufferSlice>::make_bridge();
      td::actor::send_closure(sender2.quic_sender, &ton::quic::QuicSender::send_query, sender2.id, hub.id,
                              std::string("Q"), std::move(promise), td::Timestamp::in(60.0), std::move(data));
      tasks2.push_back(std::move(future));
    }

    // Track completion times
    std::vector<double> completion1, completion2;

    // Wait for all to complete, tracking when each finishes
    for (size_t i = 0; i < tasks1.size() || i < tasks2.size(); i++) {
      if (i < tasks1.size()) {
        auto r = co_await std::move(tasks1[i]).wrap();
        if (r.is_ok()) {
          completion1.push_back(td::Timestamp::now().at() - start.at());
        }
      }
      if (i < tasks2.size()) {
        auto r = co_await std::move(tasks2[i]).wrap();
        if (r.is_ok()) {
          completion2.push_back(td::Timestamp::now().at() - start.at());
        }
      }
    }

    auto elapsed = td::Timestamp::now().at() - start.at();

    // Calculate average completion time for each sender
    double avg1 = 0, avg2 = 0;
    for (double t : completion1)
      avg1 += t;
    for (double t : completion2)
      avg2 += t;
    avg1 /= static_cast<double>(completion1.size());
    avg2 /= static_cast<double>(completion2.size());

    LOG(INFO) << "Fairness test: sender1 completed " << completion1.size() << " queries, avg time " << avg1 << "s";
    LOG(INFO) << "Fairness test: sender2 completed " << completion2.size() << " queries, avg time " << avg2 << "s";
    LOG(INFO) << "Total elapsed: " << td::format::as_time(elapsed);

    // Fairness assertion: both senders should complete all queries
    ASSERT_EQ(completion1.size(), static_cast<size_t>(queries_per_sender));
    ASSERT_EQ(completion2.size(), static_cast<size_t>(queries_per_sender));

    // Fairness assertion: average completion times should be within 50% of each other
    // (This is a loose bound; with perfect fairness they'd be nearly equal)
    double ratio = avg1 > avg2 ? avg1 / avg2 : avg2 / avg1;
    LOG(INFO) << "Completion time ratio: " << ratio;
    ASSERT_TRUE(ratio < 1.5);  // Unfair: one sender completed much faster than the other

    co_return td::Unit{};
  });
}

// Test: Multiple streams within one connection should interleave fairly
TEST(QuicFairness, MultipleStreamsFairBandwidth) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    // Open multiple streams (queries) simultaneously with large data
    constexpr int num_streams = 20;
    constexpr int query_size = 512 * 1024;  // 512 KB each = 10 MB total

    auto server = co_await t.create_node("srv", next_port());
    auto client = co_await t.create_node("cli", next_port());
    td::actor::send_closure(server.quic_sender, &ton::quic::QuicSender::set_default_mtu, 2 * query_size);
    td::actor::send_closure(client.quic_sender, &ton::quic::QuicSender::set_default_mtu, 2 * query_size);

    t.add_peer(client, server);
    t.add_peer(server, client);

    co_await td::actor::coro_sleep(td::Timestamp::in(0.2));

    std::vector<td::actor::StartedTask<td::BufferSlice>> tasks;
    auto start = td::Timestamp::now();

    for (int i = 0; i < num_streams; i++) {
      td::BufferSlice data(query_size);
      data.as_slice()[0] = 'Q';
      td::Random::secure_bytes(data.as_slice().substr(1));
      auto [future, promise] = td::actor::StartedTask<td::BufferSlice>::make_bridge();
      td::actor::send_closure(client.quic_sender, &ton::quic::QuicSender::send_query, client.id, server.id,
                              std::string("Q"), std::move(promise), td::Timestamp::in(30.0), std::move(data));
      tasks.push_back(std::move(future));
    }

    // Track completion order
    std::vector<int> completion_order;
    for (int i = 0; i < num_streams; i++) {
      auto r = co_await std::move(tasks[i]).wrap();
      if (r.is_ok()) {
        completion_order.push_back(i);
      }
    }

    auto elapsed = td::Timestamp::now().at() - start.at();

    LOG(INFO) << "Multi-stream test: " << completion_order.size() << "/" << num_streams << " completed in "
              << td::format::as_time(elapsed);

    // All streams should complete
    ASSERT_EQ(completion_order.size(), static_cast<size_t>(num_streams));

    // With fair scheduling, streams should complete in roughly the order they were opened
    // (since they're all the same size). Check that early streams don't all finish last.
    int early_in_first_half = 0;
    for (size_t i = 0; i < completion_order.size() / 2; i++) {
      if (completion_order[i] < num_streams / 2) {
        early_in_first_half++;
      }
    }

    LOG(INFO) << "Early streams in first half of completions: " << early_in_first_half;
    // At least some early streams should complete in the first half
    ASSERT_TRUE(early_in_first_half >= num_streams / 4);  // Unfair: early streams starved

    co_return td::Unit{};
  });
}

// Test: Many connections should all make progress (no starvation)
TEST(QuicFairness, ManyConnectionsNoStarvation) {
  run_test([](TestRunner& t) -> td::actor::Task<td::Unit> {
    constexpr int queries_per_sender = 20;
    constexpr int query_size = 256 * 1024;  // 256 KB each = 5 MB per sender = 50 MB total

    auto hub = co_await t.create_node("hub", next_port());
    td::actor::send_closure(hub.quic_sender, &ton::quic::QuicSender::set_default_mtu, 2 * query_size);

    constexpr int num_senders = 10;
    std::vector<TestNode> senders;

    for (int i = 0; i < num_senders; i++) {
      senders.push_back(co_await t.create_node("s" + std::to_string(i), next_port()));
      td::actor::send_closure(senders.back().quic_sender, &ton::quic::QuicSender::set_default_mtu, 2 * query_size);
      t.add_peer(senders.back(), hub);
      t.add_peer(hub, senders.back());
    }

    co_await td::actor::coro_sleep(td::Timestamp::in(0.3));

    std::vector<std::vector<td::actor::StartedTask<td::BufferSlice>>> all_tasks(num_senders);
    auto start = td::Timestamp::now();

    // Launch queries from all senders
    for (int s = 0; s < num_senders; s++) {
      for (int q = 0; q < queries_per_sender; q++) {
        td::BufferSlice data(query_size);
        data.as_slice()[0] = 'Q';
        td::Random::secure_bytes(data.as_slice().substr(1));
        auto [future, promise] = td::actor::StartedTask<td::BufferSlice>::make_bridge();
        td::actor::send_closure(senders[s].quic_sender, &ton::quic::QuicSender::send_query, senders[s].id, hub.id,
                                std::string("Q"), std::move(promise), td::Timestamp::in(60.0), std::move(data));
        all_tasks[s].push_back(std::move(future));
      }
    }

    // Wait for all and count successes per sender
    std::vector<int> success_counts(num_senders, 0);
    for (int s = 0; s < num_senders; s++) {
      for (auto& task : all_tasks[s]) {
        auto r = co_await std::move(task).wrap();
        if (r.is_ok()) {
          success_counts[s]++;
        }
      }
    }

    auto elapsed = td::Timestamp::now().at() - start.at();

    int total_success = 0;
    int min_success = queries_per_sender;
    int max_success = 0;
    for (int s = 0; s < num_senders; s++) {
      total_success += success_counts[s];
      min_success = std::min(min_success, success_counts[s]);
      max_success = std::max(max_success, success_counts[s]);
      LOG(INFO) << "Sender " << s << ": " << success_counts[s] << "/" << queries_per_sender << " completed";
    }

    LOG(INFO) << "Total: " << total_success << "/" << (num_senders * queries_per_sender) << " in "
              << td::format::as_time(elapsed);
    LOG(INFO) << "Min/Max per sender: " << min_success << "/" << max_success;

    // All senders should complete all queries (no starvation)
    ASSERT_EQ(total_success, num_senders * queries_per_sender);

    // No sender should be significantly behind others
    ASSERT_EQ(min_success, queries_per_sender);  // Some senders starved if this fails

    co_return td::Unit{};
  });
}

TEST(QuicRateLimiter, BurstAndRefillSemantics) {
  ton::adnl::RateLimiter limiter(3, 1.0);

  auto expect_take = [&](bool expected) { ASSERT_EQ(limiter.take(), expected); };

  expect_take(true);
  expect_take(true);
  expect_take(true);
  expect_take(false);

  jump_time_to(limiter.ready_at().at());
  expect_take(true);
  expect_take(false);

  jump_time_by(2.0);
  expect_take(true);
  expect_take(true);
  expect_take(false);

  jump_time_by(10.0);
  expect_take(true);
  expect_take(true);
  expect_take(true);
  expect_take(false);
}

TEST(QuicRateLimiter, CapacityOneDoesNotAllowExtraBurst) {
  ton::adnl::RateLimiter limiter(1, 1.0);

  auto expect_take = [&](bool expected) { ASSERT_EQ(limiter.take(), expected); };

  expect_take(true);
  expect_take(false);

  jump_time_by(1.0);
  expect_take(true);
  expect_take(false);
}

int main(int argc, char* argv[]) {
  SET_VERBOSITY_LEVEL(verbosity_INFO);
  td::set_default_failure_signal_handler().ensure();

  td::OptionParser p;
  p.add_checked_option('p', "port", "base port (default 21000)", [](td::Slice arg) {
    TRY_RESULT(v, td::to_integer_safe<int>(arg));
    g_config.port_counter = v;
    return td::Status::OK();
  });
  p.add_checked_option('t', "threads", "scheduler threads (default 4)", [](td::Slice arg) {
    TRY_RESULT(v, td::to_integer_safe<int>(arg));
    g_config.threads = v;
    return td::Status::OK();
  });
  p.add_option('T', "timeout", "test timeout in seconds (default 60)",
               [](td::Slice arg) { g_config.timeout = td::to_double(arg); });
  p.add_option('f', "filter", "run only tests matching filter",
               [](td::Slice arg) { td::TestsRunner::get_default().add_substr_filter(arg.str()); });
  p.add_checked_option('N', "nodes", "large scale test: number of nodes (default 5, 0 to skip)", [](td::Slice arg) {
    TRY_RESULT(v, td::to_integer_safe<int>(arg));
    g_config.large_nodes = v;
    return td::Status::OK();
  });
  p.add_checked_option('Q', "queries", "large scale test: queries per destination (default 200)", [](td::Slice arg) {
    TRY_RESULT(v, td::to_integer_safe<int>(arg));
    g_config.large_queries = v;
    return td::Status::OK();
  });
  p.add_checked_option('S', "size", "large scale test: query size in bytes (default 131072)", [](td::Slice arg) {
    TRY_RESULT(v, td::to_integer_safe<int>(arg));
    g_config.large_size = v;
    return td::Status::OK();
  });
  p.run(argc, argv).ensure();

  td::TestsRunner::get_default().run_all();
  return td::TestsRunner::get_default().any_test_failed() ? 1 : 0;
}
