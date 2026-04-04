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
#include <atomic>
#include <mutex>
#include <optional>
#include <unordered_set>

#include "adnl/adnl-network-manager.h"
#include "adnl/adnl-peer-table.h"
#include "adnl/adnl.h"
#include "adnl/utils.hpp"
#include "auto/tl/ton_api.hpp"
#include "keyring/keyring.h"
#include "keys/keys.hpp"
#include "quic/quic-sender.h"
#include "quic/quic-server.h"
#include "td/actor/coro_task.h"
#include "td/actor/coro_utils.h"
#include "td/utils/OptionParser.h"
#include "td/utils/Random.h"
#include "td/utils/Time.h"
#include "td/utils/crypto.h"
#include "td/utils/format.h"
#include "td/utils/port/path.h"
#include "td/utils/port/signals.h"
#include "td/utils/tests.h"
#include "tl-utils/common-utils.hpp"

namespace {

struct Config {
  std::atomic<int> port_counter{21000};
  int threads{4};
  double timeout{60.0};
  int large_nodes{5};
  int large_queries{1000};
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
  ton::quic::QuicServer::Options options;
  options.flood_control.reset();
  options.new_connection_rate_limit_capacity = 0;
  return options;
}

class EchoCallback : public ton::adnl::Adnl::Callback {
 public:
  std::shared_ptr<std::vector<td::BufferSlice>> received_messages;

  explicit EchoCallback(std::shared_ptr<std::vector<td::BufferSlice>> msgs = nullptr)
      : received_messages(std::move(msgs)) {
  }

  void receive_message(ton::adnl::AdnlNodeIdShort, ton::adnl::AdnlNodeIdShort, td::BufferSlice data) override {
    LOG(ERROR) << "receive message message";
    if (received_messages) {
      received_messages->push_back(std::move(data));
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
  std::shared_ptr<std::vector<td::BufferSlice>> received_messages;

  TestNode() = default;
  TestNode(TestNode&&) = default;
  TestNode& operator=(TestNode&&) = default;
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

  td::actor::Task<TestNode> create_node(std::string name, int port, std::optional<ton::PrivateKey> key = std::nullopt,
                                        std::string ip = "127.0.0.1") {
    TestNode node;
    node.ip = ip;
    node.port = port;
    node.key = key.value_or(make_key(port));
    node.id = ton::adnl::AdnlNodeIdShort{node.key.compute_public_key().compute_short_id()};

    std::string db = db_root_ + "/" + name;
    td::rmrf(db).ignore();
    td::mkdir(db).ensure();

    node.keyring = ton::keyring::Keyring::create(db);
    node.network_manager = ton::adnl::AdnlNetworkManager::create(static_cast<td::uint16>(port));
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

    node.received_messages = std::make_shared<std::vector<td::BufferSlice>>();
    td::actor::send_closure(node.adnl, &ton::adnl::Adnl::subscribe, node.id, "M",
                            std::make_unique<EchoCallback>(node.received_messages));

    node.quic_sender = td::actor::create_actor<ton::quic::QuicSender>(
        "quic-" + name, td::actor::actor_dynamic_cast<ton::adnl::AdnlPeerTable>(node.adnl.get()), node.keyring.get(),
        quic_test_options());

    td::actor::send_closure(node.quic_sender, &ton::quic::QuicSender::add_id, node.id);

    co_await td::actor::Yield{};
    co_return std::move(node);
  }

  void add_peer(TestNode& from, const TestNode& to) {
    auto addr_list = make_addr_list(to.ip, to.port);
    td::actor::send_closure(from.adnl, &ton::adnl::Adnl::add_peer, from.id,
                            ton::adnl::AdnlNodeIdFull{to.key.compute_public_key()}, addr_list);
  }

  void set_slow_echo(TestNode& node, double delay) {
    td::actor::send_closure(node.adnl, &ton::adnl::Adnl::subscribe, node.id, "S",
                            std::make_unique<SlowEchoCallback>(delay));
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

td::Ed25519::PrivateKey make_quic_key(int seed) {
  td::Bits256 hash;
  auto seed_str = std::to_string(seed);
  td::sha256(td::Slice(seed_str), hash.as_slice());
  return td::Ed25519::PrivateKey(td::SecureString(hash.as_slice()));
}

td::Ed25519::PrivateKey clone_quic_key(const td::Ed25519::PrivateKey& key) {
  return td::Ed25519::PrivateKey(key.as_octet_string());
}

ton::quic::QuicServer::Options small_stream_limit_options(size_t max_streams_bidi) {
  auto options = quic_test_options();
  options.enable_gso = false;
  options.enable_gro = false;
  options.enable_mmsg = false;
  options.max_streams_bidi = max_streams_bidi;
  return options;
}

struct RawQuicEndpointState {
  void remember_connection(ton::quic::QuicConnectionId cid, bool is_outbound) {
    std::lock_guard guard(mutex);
    if (is_outbound) {
      outbound_cid = cid;
    } else {
      inbound_cid = cid;
    }
  }

  void remember_local_stream(ton::quic::QuicStreamID sid) {
    std::lock_guard guard(mutex);
    locally_opened_streams.insert(sid);
  }

  bool is_local_stream(ton::quic::QuicStreamID sid) const {
    std::lock_guard guard(mutex);
    return locally_opened_streams.contains(sid);
  }

  void remember_closed_stream(ton::quic::QuicStreamID sid) {
    std::lock_guard guard(mutex);
    auto was_local = locally_opened_streams.erase(sid) > 0;
    closed_streams.insert(sid);
    closed_stream_count++;
    if (was_local) {
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
  std::unordered_set<ton::quic::QuicStreamID> locally_opened_streams;
  std::unordered_set<ton::quic::QuicStreamID> closed_streams;
  size_t closed_stream_count = 0;
  size_t local_closed_stream_count = 0;
};

class RawQuicCallback final : public ton::quic::QuicServer::Callback {
 public:
  explicit RawQuicCallback(std::shared_ptr<RawQuicEndpointState> state) : state_(std::move(state)) {
  }

  void set_server(td::actor::ActorId<ton::quic::QuicServer> server) {
    server_ = server;
  }

  td::Status on_connected(ton::quic::QuicConnectionId cid, td::SecureString, bool is_outbound) override {
    state_->remember_connection(cid, is_outbound);
    return td::Status::OK();
  }

  td::Status on_stream(ton::quic::QuicConnectionId cid, ton::quic::QuicStreamID sid, td::BufferSlice,
                       bool is_end) override {
    if (is_end && !state_->is_local_stream(sid)) {
      td::actor::send_closure(server_, &ton::quic::QuicServer::send_stream_end, cid, sid);
    }
    return td::Status::OK();
  }

  void on_closed(ton::quic::QuicConnectionId) override {
  }

  void on_stream_closed(ton::quic::QuicConnectionId, ton::quic::QuicStreamID sid) override {
    state_->remember_closed_stream(sid);
  }

  void set_peer_mtu_callback(std::function<td::uint64(ton::adnl::AdnlNodeIdShort)>) override {
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

  td::actor::Task<RawQuicEndpoint> create_endpoint(ton::quic::QuicServer::Options options) {
    auto port = next_port();
    auto key = make_quic_key(port);
    auto state = std::make_shared<RawQuicEndpointState>();

    auto callback = std::make_unique<RawQuicCallback>(state);
    auto* callback_ptr = callback.get();
    auto server_result = ton::quic::QuicServer::create(port, clone_quic_key(key), std::move(callback), 4096, "ton",
                                                       "127.0.0.1", options);
    ASSERT_TRUE(server_result.is_ok());
    auto server = server_result.move_as_ok();
    callback_ptr->set_server(server.get());

    co_await td::actor::Yield{};
    co_return RawQuicEndpoint{port, std::move(key), std::move(server), std::move(state)};
  }

  td::actor::Task<std::pair<ton::quic::QuicConnectionId, ton::quic::QuicConnectionId>> connect(
      RawQuicEndpoint& client, RawQuicEndpoint& server) {
    auto outbound_cid_result =
        co_await td::actor::ask(client.server, &ton::quic::QuicServer::connect, td::Slice("127.0.0.1"), server.port,
                                clone_quic_key(client.key), td::Slice("ton"))
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

  td::actor::Task<ton::quic::QuicStreamID> open_stream_with_retry(RawQuicEndpoint& endpoint,
                                                                  ton::quic::QuicConnectionId cid,
                                                                  double timeout = 5.0) {
    auto deadline = td::Timestamp::in(timeout);
    while (true) {
      auto sid_result =
          co_await td::actor::ask(endpoint.server, &ton::quic::QuicServer::open_stream, cid, ton::quic::StreamOptions{})
              .wrap();
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

  td::actor::Task<ton::quic::QuicStreamID> send_and_fin_stream(RawQuicEndpoint& endpoint,
                                                               ton::quic::QuicConnectionId cid, char byte) {
    auto sid = co_await open_stream_with_retry(endpoint, cid);

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

  td::actor::Task<size_t> open_streams_until_blocked(RawQuicEndpoint& endpoint, ton::quic::QuicConnectionId cid,
                                                     size_t max_attempts) {
    size_t opened = 0;
    for (size_t i = 0; i < max_attempts; i++) {
      auto sid_result =
          co_await td::actor::ask(endpoint.server, &ton::quic::QuicServer::open_stream, cid, ton::quic::StreamOptions{})
              .wrap();
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

 private:
  template <class Predicate>
  td::actor::Task<td::Unit> wait_until(Predicate&& predicate, double timeout) {
    auto deadline = td::Timestamp::in(timeout);
    while (!predicate()) {
      ASSERT_TRUE(!deadline.is_in_past());
      co_await td::actor::yield_on_current();
    }
    co_return td::Unit{};
  }

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

}  // namespace

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

    auto response = co_await t.send_prefixed_query_ex(a, b, "P", "second", 5.0, 1024);
    ASSERT_EQ(response.as_slice(), td::Slice("victim"));

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

    // Wait for the slow response to complete on server side
    co_await td::actor::coro_sleep(td::Timestamp::in(5.0));

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
  p.add_checked_option('Q', "queries", "large scale test: queries per destination (default 1000)", [](td::Slice arg) {
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
