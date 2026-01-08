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
#include <memory>
#include <thread>

#include "adnl/adnl-network-manager.h"
#include "adnl/adnl-peer-table.h"
#include "adnl/adnl-test-loopback-implementation.h"
#include "adnl/adnl.h"
#include "keys/keys.hpp"
#include "quic/quic-sender.h"
#include "rldp/rldp.h"
#include "rldp2/rldp.h"
#include "td/utils/OptionParser.h"
#include "td/utils/Random.h"
#include "td/utils/base64.h"
#include "td/utils/format.h"
#include "td/utils/port/path.h"
#include "td/utils/port/signals.h"

namespace {
// Create deterministic Ed25519 private key from a seed byte
ton::PrivateKey make_private_key(td::uint8 seed) {
  td::uint8 data[32];
  std::memset(data, seed, 32);
  return ton::PrivateKey{ton::privkeys::Ed25519{td::Slice(data, 32)}};
}

// Fixed keys for benchmarking (lazy initialization)
const ton::PrivateKey& server_private_key() {
  static auto key = make_private_key(1);
  return key;
}
const ton::PublicKey& server_public_key() {
  static auto key = server_private_key().compute_public_key();
  return key;
}
const ton::PrivateKey& client_private_key() {
  static auto key = make_private_key(2);
  return key;
}
const ton::PublicKey& client_public_key() {
  static auto key = client_private_key().compute_public_key();
  return key;
}
}  // namespace

enum class Mode { loopback, server, client, both };
enum class Protocol { rldp1, rldp2, quic };

struct Config {
  Mode mode = Mode::loopback;
  Protocol protocol = Protocol::rldp2;
  td::uint32 threads = 7;
  td::uint32 query_size = 1024;
  td::uint32 response_size = 1024;
  td::uint32 num_queries = 100;
  td::uint32 max_inflight = 0;  // 0 = unlimited
  double timeout = 60.0;

  // Network mode options
  td::IPAddress local_addr;
  td::IPAddress server_addr;
};

const char* protocol_name(Protocol p) {
  switch (p) {
    case Protocol::rldp1:
      return "rldp1";
    case Protocol::rldp2:
      return "rldp2";
    case Protocol::quic:
      return "quic";
  }
  return "unknown";
}

class Server : public ton::adnl::Adnl::Callback {
 public:
  Server(td::uint32 response_size) : response_size_(response_size) {
  }

  void receive_message(ton::adnl::AdnlNodeIdShort src, ton::adnl::AdnlNodeIdShort dst, td::BufferSlice data) override {
  }

  void receive_query(ton::adnl::AdnlNodeIdShort src, ton::adnl::AdnlNodeIdShort dst, td::BufferSlice data,
                     td::Promise<td::BufferSlice> promise) override {
    td::BufferSlice response{response_size_};
    td::Random::secure_bytes(response.as_slice());
    promise.set_value(std::move(response));
  }

 private:
  td::uint32 response_size_;
};

class BenchmarkRunner : public td::actor::Actor {
 public:
  BenchmarkRunner(Config config, td::actor::ActorId<ton::adnl::AdnlSenderInterface> rldp,
                  ton::adnl::AdnlNodeIdShort src, ton::adnl::AdnlNodeIdShort dst)
      : config_(config), rldp_(rldp), src_(src), dst_(dst) {
    query_start_times_.resize(config.num_queries);
  }

  void start_up() override {
    alarm_timestamp() = td::Timestamp::in(0.5);
  }

  void alarm() override {
    if (start_time_ == 0) {
      start_time_ = td::Clocks::system();
    }
    send_queries();
  }

 private:
  Config config_;
  td::actor::ActorId<ton::adnl::AdnlSenderInterface> rldp_;
  ton::adnl::AdnlNodeIdShort src_;
  ton::adnl::AdnlNodeIdShort dst_;

  double start_time_ = 0;
  td::uint32 sent_ = 0;
  td::uint32 received_ = 0;
  td::uint32 errors_ = 0;
  td::uint32 inflight_ = 0;

  std::vector<double> query_start_times_;
  std::vector<double> latencies_;

  void send_queries() {
    td::uint32 max_inflight = config_.max_inflight > 0 ? config_.max_inflight : config_.num_queries;
    while (sent_ < config_.num_queries && inflight_ < max_inflight) {
      td::BufferSlice query{config_.query_size};
      query.as_slice()[0] = 'B';
      if (config_.query_size > 1) {
        td::Random::secure_bytes(query.as_slice().remove_prefix(1));
      }

      query_start_times_[sent_] = td::Clocks::system();

      auto self = actor_id(this);
      auto promise = td::PromiseCreator::lambda([self, idx = sent_](td::Result<td::BufferSlice> R) {
        td::actor::send_closure(self, &BenchmarkRunner::on_response, idx, std::move(R));
      });

      td::actor::send_closure(rldp_, &ton::adnl::AdnlSenderInterface::send_query_ex, src_, dst_, std::string("bench"),
                              std::move(promise), td::Timestamp::in(config_.timeout), std::move(query),
                              (td::uint64)config_.response_size + 1024);
      sent_++;
      inflight_++;
    }
  }

  void on_response(td::uint32 idx, td::Result<td::BufferSlice> R) {
    double latency = td::Clocks::system() - query_start_times_[idx];
    inflight_--;

    if (R.is_error()) {
      LOG(WARNING) << "Query " << idx << " failed: " << R.error();
      errors_++;
    } else {
      received_++;
      latencies_.push_back(latency);
    }

    if (received_ + errors_ == config_.num_queries) {
      finish();
    } else {
      send_queries();
    }
  }

  double percentile(std::vector<double>& sorted, double p) {
    if (sorted.empty())
      return 0;
    size_t idx = static_cast<size_t>(p * (sorted.size() - 1));
    return sorted[idx];
  }

  void finish() {
    auto elapsed = td::Clocks::system() - start_time_;
    auto qps = config_.num_queries / elapsed;
    auto total_bytes = (td::uint64)config_.num_queries * (config_.query_size + config_.response_size);
    auto throughput_mbps = (total_bytes / elapsed) / (1024 * 1024);

    LOG(ERROR) << "Benchmark complete:";
    LOG(ERROR) << "  Protocol: " << protocol_name(config_.protocol);
    LOG(ERROR) << "  Queries: " << config_.num_queries << " (errors: " << errors_ << ")";
    LOG(ERROR) << "  Query size: " << td::format::as_size(config_.query_size);
    LOG(ERROR) << "  Response size: " << td::format::as_size(config_.response_size);
    LOG(ERROR) << "  Time: " << td::format::as_time(elapsed);
    LOG(ERROR) << "  QPS: " << qps;
    LOG(ERROR) << "  Throughput: " << throughput_mbps << " MB/s";

    if (!latencies_.empty()) {
      std::sort(latencies_.begin(), latencies_.end());
      double sum = 0;
      for (auto l : latencies_)
        sum += l;
      double avg = sum / latencies_.size();

      LOG(ERROR) << "  Latency:";
      LOG(ERROR) << "    min: " << td::format::as_time(latencies_.front());
      LOG(ERROR) << "    avg: " << td::format::as_time(avg);
      LOG(ERROR) << "    p50: " << td::format::as_time(percentile(latencies_, 0.50));
      LOG(ERROR) << "    p90: " << td::format::as_time(percentile(latencies_, 0.90));
      LOG(ERROR) << "    p99: " << td::format::as_time(percentile(latencies_, 0.99));
      LOG(ERROR) << "    max: " << td::format::as_time(latencies_.back());
    }

    std::_Exit(0);
  }
};

void run_loopback(Config config) {
  std::string db_root = "tmp-dir-bench-rldp";
  td::rmrf(db_root).ignore();
  td::mkdir(db_root).ensure();

  td::actor::Scheduler scheduler({config.threads});

  td::actor::ActorOwn<ton::keyring::Keyring> keyring;
  td::actor::ActorOwn<ton::adnl::TestLoopbackNetworkManager> network_manager;
  td::actor::ActorOwn<ton::adnl::Adnl> adnl;
  td::actor::ActorOwn<ton::rldp::Rldp> rldp1;
  td::actor::ActorOwn<ton::rldp2::Rldp> rldp2;
  td::actor::ActorOwn<ton::quic::QuicSender> quic_sender;
  td::actor::ActorOwn<BenchmarkRunner> runner;

  ton::adnl::AdnlNodeIdShort src;
  ton::adnl::AdnlNodeIdShort dst;

  scheduler.run_in_context([&] {
    keyring = ton::keyring::Keyring::create(db_root);
    network_manager = td::actor::create_actor<ton::adnl::TestLoopbackNetworkManager>("net");
    adnl = ton::adnl::Adnl::create(db_root, keyring.get());
    td::actor::send_closure(adnl, &ton::adnl::Adnl::register_network_manager, network_manager.get());

    auto max_size = std::max(config.query_size, config.response_size) + 1024;

    rldp1 = ton::rldp::Rldp::create(adnl.get());
    td::actor::send_closure(rldp1, &ton::rldp::Rldp::set_default_mtu, (td::uint64)max_size);

    rldp2 = ton::rldp2::Rldp::create(adnl.get());
    td::actor::send_closure(rldp2, &ton::rldp2::Rldp::set_default_mtu, (td::uint64)max_size);

    auto pk1 = ton::PrivateKey{ton::privkeys::Ed25519::random()};
    auto pub1 = pk1.compute_public_key();
    src = ton::adnl::AdnlNodeIdShort{pub1.compute_short_id()};
    td::actor::send_closure(keyring, &ton::keyring::Keyring::add_key, std::move(pk1), true, [](td::Unit) {});

    auto pk2 = ton::PrivateKey{ton::privkeys::Ed25519::random()};
    auto pub2 = pk2.compute_public_key();
    dst = ton::adnl::AdnlNodeIdShort{pub2.compute_short_id()};
    td::actor::send_closure(keyring, &ton::keyring::Keyring::add_key, std::move(pk2), true, [](td::Unit) {});

    auto addr = ton::adnl::TestLoopbackNetworkManager::generate_dummy_addr_list();

    td::actor::send_closure(adnl, &ton::adnl::Adnl::add_id, ton::adnl::AdnlNodeIdFull{pub1}, addr, td::uint8(0));
    td::actor::send_closure(adnl, &ton::adnl::Adnl::add_id, ton::adnl::AdnlNodeIdFull{pub2}, addr, td::uint8(0));

    td::actor::send_closure(rldp1, &ton::rldp::Rldp::add_id, src);
    td::actor::send_closure(rldp1, &ton::rldp::Rldp::add_id, dst);
    td::actor::send_closure(rldp2, &ton::rldp2::Rldp::add_id, src);
    td::actor::send_closure(rldp2, &ton::rldp2::Rldp::add_id, dst);

    td::actor::send_closure(adnl, &ton::adnl::Adnl::add_peer, src, ton::adnl::AdnlNodeIdFull{pub2}, addr);

    td::actor::send_closure(network_manager, &ton::adnl::TestLoopbackNetworkManager::add_node_id, src, true, true);
    td::actor::send_closure(network_manager, &ton::adnl::TestLoopbackNetworkManager::add_node_id, dst, true, true);

    // Create QUIC sender for loopback testing
    quic_sender = td::actor::create_actor<ton::quic::QuicSender>(
        "quic", td::actor::actor_dynamic_cast<ton::adnl::AdnlPeerTable>(adnl.get()), keyring.get());
    // Add both local IDs to QUIC sender
    td::actor::send_lambda(quic_sender, [src, dst]() {
      auto& sender = td::actor::detail::current_actor<ton::quic::QuicSender>();
      sender.add_local_id(src).start().detach("add_local_id_src");
      sender.add_local_id(dst).start().detach("add_local_id_dst");
    });

    td::actor::send_closure(adnl, &ton::adnl::Adnl::subscribe, dst, "B",
                            std::make_unique<Server>(config.response_size));

    td::actor::ActorId<ton::adnl::AdnlSenderInterface> sender_id;
    switch (config.protocol) {
      case Protocol::rldp1:
        sender_id = rldp1.get();
        break;
      case Protocol::rldp2:
        sender_id = rldp2.get();
        break;
      case Protocol::quic:
        sender_id = quic_sender.get();
        break;
    }
    runner = td::actor::create_actor<BenchmarkRunner>("runner", config, sender_id, src, dst);
  });

  scheduler.run();
  td::rmrf(db_root).ignore();
}

void run_server(Config config) {
  std::string db_root = "tmp-dir-bench-rldp-server";
  td::rmrf(db_root).ignore();
  td::mkdir(db_root).ensure();

  td::actor::Scheduler scheduler({config.threads});

  td::actor::ActorOwn<ton::keyring::Keyring> keyring;
  td::actor::ActorOwn<ton::adnl::AdnlNetworkManager> network_manager;
  td::actor::ActorOwn<ton::adnl::Adnl> adnl;
  td::actor::ActorOwn<ton::rldp::Rldp> rldp1;
  td::actor::ActorOwn<ton::rldp2::Rldp> rldp2;
  td::actor::ActorOwn<ton::quic::QuicSender> quic_sender;

  scheduler.run_in_context([&] {
    keyring = ton::keyring::Keyring::create(db_root);
    network_manager = ton::adnl::AdnlNetworkManager::create(static_cast<td::uint16>(config.local_addr.get_port()));
    adnl = ton::adnl::Adnl::create(db_root, keyring.get());
    td::actor::send_closure(adnl, &ton::adnl::Adnl::register_network_manager, network_manager.get());

    ton::adnl::AdnlCategoryMask cat_mask;
    cat_mask[0] = true;
    td::actor::send_closure(network_manager, &ton::adnl::AdnlNetworkManager::add_self_addr, config.local_addr,
                            std::move(cat_mask), 0);

    auto local_id = ton::adnl::AdnlNodeIdShort{server_public_key().compute_short_id()};
    td::actor::send_closure(keyring, &ton::keyring::Keyring::add_key, server_private_key(), true, [](td::Unit) {});

    ton::adnl::AdnlAddressList addr_list;
    addr_list.add_udp_address(config.local_addr).ensure();
    addr_list.set_version(static_cast<td::int32>(td::Clocks::system()));
    addr_list.set_reinit_date(ton::adnl::Adnl::adnl_start_time());

    td::actor::send_closure(adnl, &ton::adnl::Adnl::add_id, ton::adnl::AdnlNodeIdFull{server_public_key()}, addr_list,
                            td::uint8(0));

    auto max_size = std::max(config.query_size, config.response_size) + 1024;

    // Start RLDP v1 and v2
    rldp1 = ton::rldp::Rldp::create(adnl.get());
    td::actor::send_closure(rldp1, &ton::rldp::Rldp::set_default_mtu, (td::uint64)max_size);
    td::actor::send_closure(rldp1, &ton::rldp::Rldp::add_id, local_id);

    rldp2 = ton::rldp2::Rldp::create(adnl.get());
    td::actor::send_closure(rldp2, &ton::rldp2::Rldp::set_default_mtu, (td::uint64)max_size);
    td::actor::send_closure(rldp2, &ton::rldp2::Rldp::add_id, local_id);

    // Start QUIC sender (uses ADNL keys for TLS via RPK)
    quic_sender = td::actor::create_actor<ton::quic::QuicSender>(
        "quic", td::actor::actor_dynamic_cast<ton::adnl::AdnlPeerTable>(adnl.get()), keyring.get());
    // Use send_lambda to properly start the coroutine task
    td::actor::send_lambda(quic_sender, [local_id]() {
      td::actor::detail::current_actor<ton::quic::QuicSender>().add_local_id(local_id).start().detach("add_local_id");
    });

    td::actor::send_closure(adnl, &ton::adnl::Adnl::subscribe, local_id, "B",
                            std::make_unique<Server>(config.response_size));

    LOG(ERROR) << "Server listening on " << config.local_addr;
  });

  scheduler.run();
}

void run_client(Config config) {
  std::string db_root = "tmp-dir-bench-rldp-client";
  td::rmrf(db_root).ignore();
  td::mkdir(db_root).ensure();

  td::actor::Scheduler scheduler({config.threads});

  td::actor::ActorOwn<ton::keyring::Keyring> keyring;
  td::actor::ActorOwn<ton::adnl::AdnlNetworkManager> network_manager;
  td::actor::ActorOwn<ton::adnl::Adnl> adnl;
  td::actor::ActorOwn<ton::rldp::Rldp> rldp1;
  td::actor::ActorOwn<ton::rldp2::Rldp> rldp2;
  td::actor::ActorOwn<ton::quic::QuicSender> quic_sender;
  td::actor::ActorOwn<BenchmarkRunner> runner;

  ton::adnl::AdnlNodeIdShort src;
  ton::adnl::AdnlNodeIdShort dst;

  scheduler.run_in_context([&] {
    keyring = ton::keyring::Keyring::create(db_root);
    network_manager = ton::adnl::AdnlNetworkManager::create(static_cast<td::uint16>(config.local_addr.get_port()));
    adnl = ton::adnl::Adnl::create(db_root, keyring.get());
    td::actor::send_closure(adnl, &ton::adnl::Adnl::register_network_manager, network_manager.get());

    ton::adnl::AdnlCategoryMask cat_mask;
    cat_mask[0] = true;
    td::actor::send_closure(network_manager, &ton::adnl::AdnlNetworkManager::add_self_addr, config.local_addr,
                            std::move(cat_mask), 0);

    src = ton::adnl::AdnlNodeIdShort{client_private_key().compute_public_key().compute_short_id()};
    td::actor::send_closure(keyring, &ton::keyring::Keyring::add_key, client_private_key(), true, [](td::Unit) {});

    ton::adnl::AdnlAddressList local_addr_list;
    local_addr_list.add_udp_address(config.local_addr).ensure();
    local_addr_list.set_version(static_cast<td::int32>(td::Clocks::system()));
    local_addr_list.set_reinit_date(ton::adnl::Adnl::adnl_start_time());

    td::actor::send_closure(adnl, &ton::adnl::Adnl::add_id,
                            ton::adnl::AdnlNodeIdFull{client_private_key().compute_public_key()}, local_addr_list,
                            td::uint8(0));

    auto max_size = std::max(config.query_size, config.response_size) + 1024;

    rldp1 = ton::rldp::Rldp::create(adnl.get());
    td::actor::send_closure(rldp1, &ton::rldp::Rldp::set_default_mtu, (td::uint64)max_size);
    td::actor::send_closure(rldp1, &ton::rldp::Rldp::add_id, src);

    rldp2 = ton::rldp2::Rldp::create(adnl.get());
    td::actor::send_closure(rldp2, &ton::rldp2::Rldp::set_default_mtu, (td::uint64)max_size);
    td::actor::send_closure(rldp2, &ton::rldp2::Rldp::add_id, src);

    quic_sender = td::actor::create_actor<ton::quic::QuicSender>(
        "quic", td::actor::actor_dynamic_cast<ton::adnl::AdnlPeerTable>(adnl.get()), keyring.get());
    // Use send_lambda to properly start the coroutine task
    td::actor::send_lambda(quic_sender, [src]() {
      td::actor::detail::current_actor<ton::quic::QuicSender>().add_local_id(src).start().detach("add_local_id");
    });

    // Add server as static node
    dst = ton::adnl::AdnlNodeIdShort{server_public_key().compute_short_id()};
    ton::adnl::AdnlAddressList server_addr_list;
    server_addr_list.add_udp_address(config.server_addr).ensure();
    server_addr_list.set_version(static_cast<td::int32>(td::Clocks::system()));
    server_addr_list.set_reinit_date(0);

    ton::adnl::AdnlNodesList static_nodes;
    static_nodes.push(ton::adnl::AdnlNode{ton::adnl::AdnlNodeIdFull{server_public_key()}, server_addr_list});
    td::actor::send_closure(adnl, &ton::adnl::Adnl::add_static_nodes_from_config, std::move(static_nodes));

    td::actor::ActorId<ton::adnl::AdnlSenderInterface> sender_id;
    switch (config.protocol) {
      case Protocol::rldp1:
        sender_id = rldp1.get();
        break;
      case Protocol::rldp2:
        sender_id = rldp2.get();
        break;
      case Protocol::quic:
        sender_id = quic_sender.get();
        break;
    }
    runner = td::actor::create_actor<BenchmarkRunner>("runner", config, sender_id, src, dst);
  });

  scheduler.run();
  td::rmrf(db_root).ignore();
}

void run_both(Config config) {
  // Create separate configs for server and client
  Config server_config = config;
  Config client_config = config;

  // Server uses local_addr as its listening address
  // Client uses a different port for its own address
  client_config.local_addr.init_host_port("127.0.0.1:19201").ensure();
  client_config.server_addr = server_config.local_addr;

  // Run server and client in separate threads
  std::thread server_thread([server_config]() { run_server(server_config); });

  // Give server time to start
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  std::thread client_thread([client_config]() { run_client(client_config); });

  client_thread.join();
  server_thread.detach();  // Server runs forever, let it die with process
}

int main(int argc, char* argv[]) {
  SET_VERBOSITY_LEVEL(verbosity_INFO);
  td::set_default_failure_signal_handler().ensure();

  Config config;

  td::OptionParser p;
  p.set_description("RLDP benchmark");
  p.add_option('h', "help", "print help", [&]() {
    char b[10240];
    td::StringBuilder sb(td::MutableSlice{b, 10000});
    sb << p;
    std::cout << sb.as_cslice().c_str();
    std::exit(0);
  });
  p.add_option('v', "verbosity", "set verbosity level", [&](td::Slice arg) {
    int v = VERBOSITY_NAME(FATAL) + td::to_integer<int>(arg);
    SET_VERBOSITY_LEVEL(v);
  });
  p.add_checked_option('\0', "rldp1", "use RLDP v1", [&]() {
    config.protocol = Protocol::rldp1;
    return td::Status::OK();
  });
  p.add_checked_option('\0', "rldp2", "use RLDP v2 (default)", [&]() {
    config.protocol = Protocol::rldp2;
    return td::Status::OK();
  });
  p.add_checked_option('\0', "quic", "use QUIC", [&]() {
    config.protocol = Protocol::quic;
    return td::Status::OK();
  });
  p.add_checked_option('t', "threads", "number of threads (default: 7)", [&](td::Slice arg) {
    TRY_RESULT(v, td::to_integer_safe<td::uint32>(arg));
    config.threads = v;
    return td::Status::OK();
  });
  p.add_checked_option('n', "num-queries", "number of queries (default: 100)", [&](td::Slice arg) {
    TRY_RESULT(v, td::to_integer_safe<td::uint32>(arg));
    config.num_queries = v;
    return td::Status::OK();
  });
  p.add_checked_option('q', "query-size", "query size in bytes (default: 1024)", [&](td::Slice arg) {
    TRY_RESULT(v, td::to_integer_safe<td::uint32>(arg));
    config.query_size = v;
    return td::Status::OK();
  });
  p.add_checked_option('r', "response-size", "response size in bytes (default: 1024)", [&](td::Slice arg) {
    TRY_RESULT(v, td::to_integer_safe<td::uint32>(arg));
    config.response_size = v;
    return td::Status::OK();
  });
  p.add_checked_option('c', "max-inflight", "max concurrent queries (default: unlimited)", [&](td::Slice arg) {
    TRY_RESULT(v, td::to_integer_safe<td::uint32>(arg));
    config.max_inflight = v;
    return td::Status::OK();
  });
  p.add_option('\0', "timeout", "query timeout in seconds (default: 60)",
               [&](td::Slice arg) { config.timeout = td::to_double(arg); });
  p.add_checked_option('\0', "server", "run as server", [&]() {
    config.mode = Mode::server;
    return td::Status::OK();
  });
  p.add_checked_option('\0', "client", "run as client", [&]() {
    config.mode = Mode::client;
    return td::Status::OK();
  });
  p.add_checked_option('\0', "both", "run server and client in same process", [&]() {
    config.mode = Mode::both;
    return td::Status::OK();
  });
  p.add_checked_option('a', "addr", "local address (ip:port)", [&](td::Slice arg) {
    TRY_STATUS(config.local_addr.init_host_port(arg.str()));
    return td::Status::OK();
  });
  p.add_checked_option('s', "server-addr", "server address (ip:port) for client mode", [&](td::Slice arg) {
    TRY_STATUS(config.server_addr.init_host_port(arg.str()));
    return td::Status::OK();
  });

  auto S = p.run(argc, argv);
  if (S.is_error()) {
    LOG(ERROR) << "Failed to parse options: " << S.error();
    std::_Exit(1);
  }

  // Set default addresses
  if (config.mode == Mode::server && !config.local_addr.is_valid()) {
    config.local_addr.init_host_port("127.0.0.1:19200").ensure();
  }
  if (config.mode == Mode::client) {
    if (!config.local_addr.is_valid()) {
      config.local_addr.init_host_port("127.0.0.1:19201").ensure();
    }
    if (!config.server_addr.is_valid()) {
      config.server_addr.init_host_port("127.0.0.1:19200").ensure();
    }
  }
  if (config.mode == Mode::both) {
    // For both mode, set server and client addresses appropriately
    config.local_addr.init_host_port("127.0.0.1:19200").ensure();   // Server port
    config.server_addr.init_host_port("127.0.0.1:19200").ensure();  // Client connects to server
  }

  const char* mode_str = "unknown";
  switch (config.mode) {
    case Mode::loopback:
      mode_str = "loopback";
      break;
    case Mode::server:
      mode_str = "server";
      break;
    case Mode::client:
      mode_str = "client";
      break;
    case Mode::both:
      mode_str = "both";
      break;
  }
  LOG(ERROR) << "Starting benchmark (mode: " << mode_str << ", protocol: " << protocol_name(config.protocol) << ")";
  LOG(ERROR) << "Server public key: " << td::base64_encode(server_public_key().ed25519_value().raw().as_slice());
  LOG(ERROR) << "Client public key: " << td::base64_encode(client_public_key().ed25519_value().raw().as_slice());

  switch (config.mode) {
    case Mode::loopback:
      run_loopback(config);
      break;
    case Mode::server:
      run_server(config);
      break;
    case Mode::client:
      run_client(config);
      break;
    case Mode::both:
      run_both(config);
      break;
  }

  return 0;
}
