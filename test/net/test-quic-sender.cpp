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
#include <optional>

#include "adnl/adnl-network-manager.h"
#include "adnl/adnl-peer-table.h"
#include "adnl/adnl.h"
#include "keyring/keyring.h"
#include "keys/keys.hpp"
#include "quic/quic-sender.h"
#include "td/actor/coro_task.h"
#include "td/actor/coro_utils.h"
#include "td/utils/OptionParser.h"
#include "td/utils/Random.h"
#include "td/utils/crypto.h"
#include "td/utils/format.h"
#include "td/utils/port/path.h"
#include "td/utils/port/signals.h"
#include "td/utils/tests.h"

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
  list.add_udp_address(ip).ensure();
  list.set_version(static_cast<td::int32>(td::Clocks::system()));
  list.set_reinit_date(ton::adnl::Adnl::adnl_start_time());
  return list;
}

class EchoCallback : public ton::adnl::Adnl::Callback {
 public:
  void receive_message(ton::adnl::AdnlNodeIdShort, ton::adnl::AdnlNodeIdShort, td::BufferSlice) override {
  }

  void receive_query(ton::adnl::AdnlNodeIdShort, ton::adnl::AdnlNodeIdShort, td::BufferSlice data,
                     td::Promise<td::BufferSlice> promise) override {
    promise.set_value(std::move(data));
  }
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

    node.quic_sender = td::actor::create_actor<ton::quic::QuicSender>(
        "quic-" + name, td::actor::actor_dynamic_cast<ton::adnl::AdnlPeerTable>(node.adnl.get()), node.keyring.get());

    td::actor::send_closure(node.quic_sender, &ton::quic::QuicSender::add_local_id, node.id);

    co_await td::actor::Yield{};
    co_return std::move(node);
  }

  void add_peer(TestNode& from, const TestNode& to) {
    auto addr_list = make_addr_list(to.ip, to.port);
    td::actor::send_closure(from.adnl, &ton::adnl::Adnl::add_peer, from.id,
                            ton::adnl::AdnlNodeIdFull{to.key.compute_public_key()}, addr_list);
  }

  td::actor::Task<td::BufferSlice> send_query(TestNode& from, TestNode& to, td::Slice data) {
    td::BufferSlice query(1 + data.size());
    query.as_slice()[0] = 'Q';
    query.as_slice().substr(1).copy_from(data);

    auto [future, promise] = td::actor::StartedTask<td::BufferSlice>::make_bridge();
    td::actor::send_closure(from.quic_sender, &ton::quic::QuicSender::send_query, from.id, to.id, std::string("Q"),
                            std::move(promise), td::Timestamp::in(10.0), std::move(query));
    co_return co_await std::move(future);
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
    auto a = co_await t.create_node("ra", next_port());
    int b_port = next_port();
    auto b_key = make_key(-1);

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

    b = co_await t.create_node("rb2", b_port, b_key);
    ASSERT_EQ(b.id, b_id);

    t.add_peer(a, b);
    t.add_peer(b, a);

    co_await td::actor::coro_sleep(td::Timestamp::in(0.2));

    auto no_resp2 = co_await t.send_query(a, b, "after").wrap();
    ASSERT_TRUE(no_resp2.is_error());
    // should fail because connection is lost

    // check that connection will be restored
    auto resp2 = co_await t.send_query(a, b, "after");
    ASSERT_EQ(resp2.as_slice(), td::Slice("Qafter"));

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
    double mbps = (total_bytes / 1024.0 / 1024.0) / elapsed;

    LOG(INFO) << "Success: " << success << ", Errors: " << errors;
    LOG(INFO) << "Time: " << td::format::as_time(elapsed);
    LOG(INFO) << "QPS: " << (success / elapsed) << ", Throughput: " << mbps << " MB/s";

    ASSERT_EQ(errors, 0);

    co_return td::Unit{};
  });
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
