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
#include <cmath>
#include <limits>
#include <memory>

#include "adnl/adnl-network-manager.h"
#include "adnl/adnl-peer-table.h"
#include "adnl/adnl.h"
#include "keys/keys.hpp"
#include "metrics/prometheus-exporter.h"
#include "quic/quic-sender.h"
#include "rldp2/rldp.h"
#include "td/utils/OptionParser.h"
#include "td/utils/as.h"
#include "td/utils/format.h"
#include "td/utils/port/path.h"
#include "td/utils/port/signals.h"

namespace {
// Deterministic keys, so a client can derive any server's identity without exchanging anything. The
// tag separates the two id spaces: a client and a server with the same index must not share a key.
ton::PrivateKey make_private_key(char tag, td::uint32 index) {
  td::uint8 data[32];
  std::memset(data, tag, 32);
  td::as<td::uint32>(data) = index;
  return ton::PrivateKey{ton::privkeys::Ed25519{td::Slice(data, 32)}};
}

ton::PrivateKey server_private_key(td::uint32 server_id = 0) {
  return make_private_key('S', server_id);
}
ton::PublicKey server_public_key(td::uint32 server_id = 0) {
  return server_private_key(server_id).compute_public_key();
}
ton::PrivateKey client_private_key(td::uint32 client_id = 0) {
  return make_private_key('C', client_id);
}
}  // namespace

enum class Mode { unset, server, client };
enum class Protocol { rldp2, quic, adnl };

struct Config {
  Mode mode = Mode::unset;
  Protocol protocol = Protocol::rldp2;
  td::uint32 client_id = 0;
  td::uint32 server_id = 0;
  td::uint32 servers = 1;  // client fans out over servers at consecutive ports from --server-addr
  // Peer sessions this client owns, spread round-robin over the servers. Each one is a distinct
  // local ADNL identity, which is what makes it a distinct QUIC connection to the same server.
  td::uint32 connections = 1;
  td::uint32 connect_inflight = 256;  // concurrent readiness probes during setup
  td::uint32 threads = 7;
  td::uint32 query_size = 1024;
  td::uint32 response_size = 1024;
  td::uint32 num_queries = 100;
  td::uint32 max_inflight = 0;  // 0 = unlimited
  double timeout = 60.0;
  double test_timeout = 5.0;
  bool enable_gso = true;
  bool enable_gro = true;
  bool enable_mmsg = true;
  ton::quic::CongestionControlAlgo cc_algo = ton::quic::CongestionControlAlgo::Bbr;
  int prometheus_port = 0;  // 0 = disabled
  bool messages = false;    // fire-and-forget instead of query/response
  td::uint32 rate = 0;      // messages/sec, message mode only
  td::uint32 message_burst = 1;
  bool stay_alive = false;
  bool no_limits = false;
  std::optional<size_t> ack_thresh;
  std::optional<double> max_ack_delay_seconds;

  td::IPAddress local_addr;
  td::IPAddress public_addr;
  td::IPAddress server_addr;
};

const char* protocol_name(Protocol p) {
  switch (p) {
    case Protocol::rldp2:
      return "rldp2";
    case Protocol::quic:
      return "quic";
    case Protocol::adnl:
      return "adnl";
  }
  return "unknown";
}

// --no-limits lifts the per-peer-address admission limits. A benchmark drives every connection from
// one address, so the defaults (10 new connections per 0.2s, 1000 concurrent) throttle the load
// generator rather than the code under test.
ton::quic::QuicServer::Options quic_options(const Config& config) {
  ton::quic::QuicServer::Options options{.enable_gso = config.enable_gso,
                                         .enable_gro = config.enable_gro,
                                         .enable_mmsg = config.enable_mmsg,
                                         .cc_algo = config.cc_algo};
  options.ack_thresh = config.ack_thresh;
  options.max_ack_delay_seconds = config.max_ack_delay_seconds;
  if (config.no_limits) {
    options.flood_control = std::nullopt;
    options.new_connection_rate_limit_capacity = 1000000;
    options.new_connection_rate_limit_period = 0.000001;
    // At 50ms RTT the default 4096-stream budget caps a connection near 80k msg/s; a ceiling
    // probe must be bounded by the transport, not by the advertised window.
    options.max_streams_bidi = 1 << 16;
    options.max_streams_uni = 1 << 16;
  }
  return options;
}

// The transport is the subject of the benchmark. Build deterministic payloads instead of charging
// secure randomness and a full-payload CRC to every timed message.
void prepare_payload(td::MutableSlice msg) {
  std::memset(msg.data(), 0, msg.size());
  msg[0] = 'B';
}

struct Node {
  td::actor::ActorOwn<ton::keyring::Keyring> keyring;
  td::actor::ActorOwn<ton::adnl::AdnlNetworkManager> network_manager;
  td::actor::ActorOwn<ton::adnl::Adnl> adnl;
  td::actor::ActorOwn<ton::rldp2::Rldp> rldp2;
  td::actor::ActorOwn<ton::quic::QuicSender> quic_sender;
  td::actor::ActorOwn<ton::PrometheusExporter> exporter;
  std::vector<ton::adnl::AdnlNodeIdShort> local_ids;

  ton::adnl::AdnlNodeIdShort local_id() const {
    CHECK(!local_ids.empty());
    return local_ids[0];
  }
};

td::actor::ActorId<ton::adnl::AdnlSenderInterface> pick_sender(Protocol protocol, const Node& node) {
  switch (protocol) {
    case Protocol::rldp2:
      CHECK(!node.rldp2.empty());
      return node.rldp2.get();
    case Protocol::quic:
      CHECK(!node.quic_sender.empty());
      return node.quic_sender.get();
    case Protocol::adnl:
      // Adnl implements AdnlSenderInterface itself, so raw ADNL queries need no extra layer.
      return td::actor::actor_dynamic_cast<ton::adnl::AdnlSenderInterface>(node.adnl.get());
  }
  UNREACHABLE();
}

Node make_node(const Config& config, const std::string& db_root, ton::PrivateKey private_key, char identity_tag = 'C',
               td::uint32 identity_index = 0) {
  Node node;
  if (config.prometheus_port) {
    td::IPAddress addr;
    addr.init_host_port(PSTRING() << "127.0.0.1:" << config.prometheus_port).ensure();
    node.exporter = ton::PrometheusExporter::create();
    td::actor::send_closure(node.exporter, &ton::PrometheusExporter::listen, addr);
  }

  node.keyring = ton::keyring::Keyring::create(db_root);
  node.network_manager = ton::adnl::AdnlNetworkManager::create(static_cast<td::uint16>(config.local_addr.get_port()));
  node.adnl = ton::adnl::Adnl::create(db_root, node.keyring.get());
  td::actor::send_closure(node.adnl, &ton::adnl::Adnl::register_network_manager, node.network_manager.get());

  ton::adnl::AdnlCategoryMask cat_mask;
  cat_mask[0] = true;
  const auto& self_addr = config.public_addr.is_valid() ? config.public_addr : config.local_addr;
  td::actor::send_closure(node.network_manager, &ton::adnl::AdnlNetworkManager::add_self_addr, self_addr,
                          std::move(cat_mask), 0);

  ton::adnl::AdnlAddressList addr_list;
  addr_list.add_udp_adnl_address(self_addr).ensure();
  addr_list.set_version(static_cast<td::int32>(td::Clocks::system()));
  addr_list.set_reinit_date(ton::adnl::Adnl::adnl_start_time());

  // One identity per session: `private_key` is the first, the rest are derived from its index space
  // so a client can hold many sessions with one server over one socket.
  for (td::uint32 i = 0; i < std::max<td::uint32>(config.connections, 1); i++) {
    auto key = i == 0 ? private_key : make_private_key(identity_tag, identity_index + i);
    auto public_key = key.compute_public_key();
    node.local_ids.push_back(ton::adnl::AdnlNodeIdShort{public_key.compute_short_id()});
    td::actor::send_closure(node.keyring, &ton::keyring::Keyring::add_key, std::move(key), true, [](td::Result<>) {});
    td::actor::send_closure(node.adnl, &ton::adnl::Adnl::add_id, ton::adnl::AdnlNodeIdFull{public_key}, addr_list,
                            td::uint8(0));
  }

  auto max_size = static_cast<td::uint64>(std::max(config.query_size, config.response_size)) + 1024;
  if (config.protocol == Protocol::rldp2) {
    node.rldp2 = ton::rldp2::Rldp::create(node.adnl.get());
    td::actor::send_closure(node.rldp2, &ton::rldp2::Rldp::set_default_mtu, max_size);
    for (auto id : node.local_ids) {
      td::actor::send_closure(node.rldp2, &ton::rldp2::Rldp::add_id, id);
    }
  } else if (config.protocol == Protocol::quic) {
    node.quic_sender = td::actor::create_actor<ton::quic::QuicSender>(
        "quic", td::actor::actor_dynamic_cast<ton::adnl::AdnlPeerTable>(node.adnl.get()), node.keyring.get(),
        quic_options(config));
    // QuicSender defaults to the ADNL MTU, which rejects any answer larger than one datagram.
    td::actor::send_closure(node.quic_sender, &ton::adnl::AdnlSenderEx::set_default_mtu, max_size);
    for (auto id : node.local_ids) {
      td::actor::send_closure(node.quic_sender, &ton::quic::QuicSender::add_id, id);
    }
  }

  if (config.prometheus_port) {
    td::actor::send_closure(node.exporter, &ton::PrometheusExporter::add<ton::adnl::AdnlNetworkManager>,
                            node.network_manager.get(), &ton::adnl::AdnlNetworkManager::collect);
    td::actor::send_closure(node.exporter, &ton::PrometheusExporter::add<ton::adnl::Adnl>, node.adnl.get(),
                            &ton::adnl::Adnl::collect);
    if (!node.rldp2.empty()) {
      td::actor::send_closure(node.exporter, &ton::PrometheusExporter::add<ton::rldp2::Rldp>, node.rldp2.get(),
                              &ton::rldp2::Rldp::collect);
    }
    if (!node.quic_sender.empty()) {
      td::actor::send_closure(node.exporter, &ton::PrometheusExporter::add<ton::quic::QuicSender>,
                              node.quic_sender.get(), &ton::quic::QuicSender::collect);
    }
  }
  return node;
}

class Server : public ton::adnl::Adnl::Callback {
 public:
  explicit Server(td::uint32 response_size) : response_(response_size) {
    std::memset(response_.as_slice().data(), 0, response_.size());
    response_.as_slice()[0] = 'R';
  }

  void receive_message(ton::adnl::AdnlNodeIdShort src, ton::adnl::AdnlNodeIdShort dst, td::BufferSlice data) override {
  }

  void receive_query(ton::adnl::AdnlNodeIdShort src, ton::adnl::AdnlNodeIdShort dst, td::BufferSlice data,
                     td::Promise<td::BufferSlice> promise) override {
    auto q = data.as_slice();
    if (q.empty() || q[0] != 'B') {
      return promise.set_error(td::Status::Error("bad query"));
    }
    if (q == td::Slice("BP")) {
      return promise.set_value(td::BufferSlice("P", 1));
    }
    promise.set_value(response_.clone());
  }

 private:
  td::BufferSlice response_;
};

class BenchmarkRunner : public td::actor::Actor {
 public:
  struct Session {
    ton::adnl::AdnlNodeIdShort src;
    ton::adnl::AdnlNodeIdShort dst;
  };

  BenchmarkRunner(Config config, td::actor::ActorId<ton::adnl::AdnlSenderInterface> sender,
                  std::vector<Session> sessions)
      : config_(config)
      , sender_(sender)
      , sessions_(std::move(sessions))
      , probe_ready_(sessions_.size())
      , payload_(config.query_size) {
    CHECK(!sessions_.empty());
    prepare_payload(payload_.as_slice());
  }

  void start_up() override {
    test_timeout_at_ = td::Timestamp::in(config_.test_timeout);
    alarm_timestamp() = td::Timestamp::now();
  }

  void alarm() override {
    if (test_timeout_at_.is_in_past()) {
      LOG(ERROR) << "Test timeout reached after " << config_.test_timeout << "s";
      LOG(ERROR) << "Sent: " << sent_ << ", Received: " << received_ << ", Errors: " << errors_
                 << ", Inflight: " << inflight_;
      std::_Exit(1);
    }

    if (!ready_) {
      if (probes_started_ == sessions_.size() && probes_pending_ == 0) {
        probes_started_ = 0;  // nothing outstanding and not done: sweep the unready ones again
      }
      send_probes();
      return;
    }
    if (start_time_ == 0) {
      start_time_ = td::Time::now();
    }
    if (config_.messages) {
      send_messages();
      auto interval = static_cast<double>(config_.message_burst) / config_.rate;
      next_message_at_ += interval;
      auto now = td::Time::now();
      if (now - next_message_at_ >= interval) {
        next_message_at_ = now + interval;
      }
      alarm_timestamp() = td::Timestamp::at(next_message_at_);
      alarm_timestamp().relax(test_timeout_at_);
      return;
    }
    send_queries();

    alarm_timestamp() = td::Timestamp::in(0.1);
    alarm_timestamp().relax(test_timeout_at_);
  }

 private:
  Config config_;
  td::actor::ActorId<ton::adnl::AdnlSenderInterface> sender_;
  // Round-robin: one client against many servers is the fan-out shape a node actually has, and it is
  // the only way to get distinct destination addresses, which is what egress batching sees. Several
  // sessions may share a destination -- they differ by local identity, and so by connection.
  std::vector<Session> sessions_;
  std::vector<bool> probe_ready_;

  double start_time_ = 0;
  double next_message_at_ = 0;
  td::uint32 sent_ = 0;
  td::uint32 received_ = 0;
  td::uint32 errors_ = 0;
  td::uint32 inflight_ = 0;
  td::uint32 probes_pending_ = 0;
  td::uint32 probes_ready_ = 0;
  td::uint32 probes_started_ = 0;
  std::vector<td::uint32> probes_retry_;
  td::Timestamp test_timeout_at_;
  bool ready_ = false;

  td::BufferSlice payload_;

  // Probes are windowed: a client may own 100k sessions, and firing every probe at once means
  // 100k simultaneous queries that all time out together. Keep `connect_inflight` outstanding and
  // start the next as each one lands.
  void send_probes() {
    auto self = actor_id(this);
    while (probes_pending_ < config_.connect_inflight && probes_started_ < sessions_.size()) {
      auto i = probes_started_++;
      if (probe_ready_[i]) {
        continue;
      }
      probes_pending_++;
      auto promise = td::PromiseCreator::lambda([self, i](td::Result<td::BufferSlice> result) {
        td::actor::send_closure(self, &BenchmarkRunner::on_probe, i, std::move(result));
      });
      auto timeout = td::Timestamp::in(std::min(config_.timeout, 5.0));
      timeout.relax(test_timeout_at_);
      td::actor::send_closure(sender_, &ton::adnl::AdnlSenderInterface::send_query_ex, sessions_[i].src,
                              sessions_[i].dst, std::string("bench-ready"), std::move(promise), timeout,
                              td::BufferSlice("BP", 2), 1024);
    }
    if (probes_pending_ == 0 && probes_ready_ == sessions_.size()) {
      on_all_probes_done();
      return;
    }
    alarm_timestamp() = td::Timestamp::in(0.5);
    alarm_timestamp().relax(test_timeout_at_);
  }

  void on_probe(td::uint32 peer, td::Result<td::BufferSlice> R) {
    probes_pending_--;
    if (R.is_error()) {
      LOG(DEBUG) << "Readiness probe to session " << peer << " failed: " << R.error();
      probes_retry_.push_back(peer);  // a busy setup ramp times probes out; try it again
    } else if (R.ok().as_slice() != td::Slice("P")) {
      LOG(ERROR) << "Readiness probe to session " << peer << " returned a bad response";
      std::_Exit(1);
    } else {
      CHECK(!probe_ready_[peer]);
      probe_ready_[peer] = true;
      probes_ready_++;
      if (probes_ready_ % 10000 == 0) {
        LOG(INFO) << "sessions ready: " << probes_ready_ << "/" << sessions_.size();
      }
    }
    if (probes_ready_ == sessions_.size()) {
      on_all_probes_done();
      return;
    }
    // Retries wait for the pass to drain, so a stuck session cannot spin ahead of fresh ones.
    if (probes_started_ == sessions_.size() && probes_pending_ == 0 && !probes_retry_.empty()) {
      for (auto index : probes_retry_) {
        probe_ready_[index] = false;
      }
      probes_started_ = 0;
      probes_retry_.clear();
    }
    send_probes();
  }

  void on_all_probes_done() {
    ready_ = true;
    if (config_.stay_alive && !config_.messages && config_.num_queries == 0) {
      keep_idle();
      return;
    }
    LOG(ERROR) << "Ready: 1";
    next_message_at_ = td::Time::now();
    alarm_timestamp() = td::Timestamp::now();
  }

  // One explicit burst per timer event. This makes fan-out/IHAVE burstiness a workload parameter
  // instead of an accidental consequence of how often the benchmark actor happened to wake up.
  void send_messages() {
    for (td::uint32 i = 0; i < config_.message_burst; ++i) {
      const auto& session = sessions_[sent_ % sessions_.size()];
      td::actor::send_closure(sender_, &ton::adnl::AdnlSenderInterface::send_message, session.src, session.dst,
                              payload_.clone());
      sent_++;
    }
  }

  void send_queries() {
    td::uint32 max_inflight = config_.max_inflight > 0 ? config_.max_inflight : config_.num_queries;
    while (sent_ < config_.num_queries && inflight_ < max_inflight) {
      auto query = payload_.clone();

      auto self = actor_id(this);
      auto promise = td::PromiseCreator::lambda([self, idx = sent_](td::Result<td::BufferSlice> result) {
        td::actor::send_closure(self, &BenchmarkRunner::on_response, idx, std::move(result));
      });

      const auto& session = sessions_[sent_ % sessions_.size()];
      td::actor::send_closure(sender_, &ton::adnl::AdnlSenderInterface::send_query_ex, session.src, session.dst,
                              std::string("bench"), std::move(promise), td::Timestamp::in(config_.timeout),
                              std::move(query), (td::uint64)config_.response_size + 1024);
      sent_++;
      inflight_++;
    }
  }

  void on_response(td::uint32 idx, td::Result<td::BufferSlice> R) {
    inflight_--;

    if (R.is_error()) {
      if (errors_ < 5) {
        LOG(WARNING) << "Query " << idx << " failed: " << R.error();
      }
      errors_++;
    } else {
      auto r = R.ok().as_slice();
      if (r.size() != config_.response_size || r[0] != 'R') {
        if (errors_ < 5) {
          LOG(ERROR) << "Query " << idx << ": bad response";
        }
        errors_++;
      } else {
        received_++;
      }
    }

    if (received_ + errors_ == config_.num_queries) {
      finish();
    } else {
      send_queries();
    }
  }

  void finish() {
    auto elapsed = td::Time::now() - start_time_;
    auto qps = received_ / elapsed;
    auto total_bytes =
        static_cast<td::uint64>(received_) * (static_cast<td::uint64>(config_.query_size) + config_.response_size);
    auto throughput_mbps = (static_cast<double>(total_bytes) / elapsed) / (1024 * 1024);

    LOG(ERROR) << "Benchmark complete:";
    LOG(ERROR) << "  Protocol: " << protocol_name(config_.protocol);
    LOG(ERROR) << "  Queries: " << config_.num_queries << " (errors: " << errors_ << ")";
    LOG(ERROR) << "  Query size: " << td::format::as_size(config_.query_size);
    LOG(ERROR) << "  Response size: " << td::format::as_size(config_.response_size);
    LOG(ERROR) << "  Time: " << td::format::as_time(elapsed);
    LOG(ERROR) << "  QPS: " << qps;
    LOG(ERROR) << "  Throughput: " << throughput_mbps << " MB/s";

    LOG(ERROR) << "Sent: " << sent_ << ", Received: " << received_ << ", Errors: " << errors_;
    if (errors_ != 0) {
      std::_Exit(1);
    }
    if (config_.stay_alive) {
      keep_idle();
      return;
    }
    std::_Exit(0);
  }

  void keep_idle() {
    LOG(ERROR) << "Ready: 1";
    alarm_timestamp() = test_timeout_at_;
  }
};

void run_server(Config config) {
  std::string db_root = "tmp-dir-bench-rldp-server-" + std::to_string(config.server_id);
  td::rmrf(db_root).ignore();
  td::mkdir(db_root).ensure();

  td::actor::Scheduler scheduler({config.threads});
  Node node;

  scheduler.run_in_context([&] {
    node = make_node(config, db_root, server_private_key(config.server_id), 'S', config.server_id);
    td::actor::send_closure(node.adnl, &ton::adnl::Adnl::subscribe, node.local_id(), "B",
                            std::make_unique<Server>(config.response_size));
    LOG(ERROR) << "Server listening on " << config.local_addr;
  });

  scheduler.run();
}

void run_client(Config config) {
  std::string db_root = "tmp-dir-bench-rldp-client-" + std::to_string(config.client_id);
  td::rmrf(db_root).ignore();
  td::mkdir(db_root).ensure();

  td::actor::Scheduler scheduler({config.threads});
  Node node;
  td::actor::ActorOwn<BenchmarkRunner> runner;

  scheduler.run_in_context([&] {
    // Each client owns a millionth of the identity space, so its derived session identities cannot
    // collide with another client's however many it holds.
    node = make_node(config, db_root, client_private_key(config.client_id * 1000000), 'C', config.client_id * 1000000);

    // Servers occupy consecutive ports from --server-addr, and their identities are derived from the
    // same index, so no address exchange is needed.
    ton::adnl::AdnlNodesList static_nodes;
    std::vector<ton::adnl::AdnlNodeIdShort> dsts;
    for (td::uint32 i = 0; i < config.servers; i++) {
      auto addr = config.server_addr;
      addr.set_port(static_cast<td::uint16>(config.server_addr.get_port() + i));
      ton::adnl::AdnlAddressList server_addr_list;
      server_addr_list.add_udp_adnl_address(addr).ensure();
      server_addr_list.set_version(static_cast<td::int32>(td::Clocks::system()));
      server_addr_list.set_reinit_date(0);
      auto server_id = config.server_id + i;
      auto server_key = server_public_key(server_id);
      static_nodes.push(ton::adnl::AdnlNode{ton::adnl::AdnlNodeIdFull{server_key}, server_addr_list});
      dsts.push_back(ton::adnl::AdnlNodeIdShort{server_key.compute_short_id()});
    }
    td::actor::send_closure(node.adnl, &ton::adnl::Adnl::add_static_nodes_from_config, std::move(static_nodes));

    std::vector<BenchmarkRunner::Session> sessions;
    for (size_t i = 0; i < node.local_ids.size(); i++) {
      sessions.push_back({node.local_ids[i], dsts[i % dsts.size()]});
    }
    runner = td::actor::create_actor<BenchmarkRunner>("runner", config, pick_sender(config.protocol, node),
                                                      std::move(sessions));
  });

  scheduler.run();
}

int main(int argc, char* argv[]) {
  SET_VERBOSITY_LEVEL(verbosity_INFO);
  td::set_default_failure_signal_handler().ensure();

  Config config;
  bool prometheus_default_port = false;

  td::OptionParser p;
  p.set_description("transport benchmark (adnl / rldp2 / quic)");
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
  p.add_checked_option('\0', "rldp2", "use RLDP v2 (default)", [&]() {
    config.protocol = Protocol::rldp2;
    return td::Status::OK();
  });
  p.add_checked_option('\0', "quic", "use QUIC", [&]() {
    config.protocol = Protocol::quic;
    return td::Status::OK();
  });
  p.add_checked_option('\0', "adnl", "use raw ADNL queries (no RLDP2, no QUIC)", [&]() {
    config.protocol = Protocol::adnl;
    return td::Status::OK();
  });
  p.add_checked_option('\0', "no-gso", "disable UDP GSO for QUIC", [&]() {
    config.enable_gso = false;
    return td::Status::OK();
  });
  p.add_checked_option('\0', "no-gro", "disable UDP GRO for QUIC", [&]() {
    config.enable_gro = false;
    return td::Status::OK();
  });
  p.add_checked_option('\0', "no-mmsg", "disable UDP sendmmsg/recvmmsg for QUIC", [&]() {
    config.enable_mmsg = false;
    return td::Status::OK();
  });
  p.add_checked_option('\0', "cc", "congestion control algorithm: cubic, reno, bbr (default: bbr)", [&](td::Slice arg) {
    if (arg == "cubic") {
      config.cc_algo = ton::quic::CongestionControlAlgo::Cubic;
    } else if (arg == "reno") {
      config.cc_algo = ton::quic::CongestionControlAlgo::Reno;
    } else if (arg == "bbr") {
      config.cc_algo = ton::quic::CongestionControlAlgo::Bbr;
    } else {
      return td::Status::Error("unknown congestion control algorithm, use: cubic, reno, bbr");
    }
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
  p.add_option('\0', "test-timeout", "global test timeout in seconds (default: 5)",
               [&](td::Slice arg) { config.test_timeout = td::to_double(arg); });
  p.add_checked_option('\0', "server", "run as server", [&]() {
    config.mode = Mode::server;
    return td::Status::OK();
  });
  p.add_checked_option('\0', "client", "run as client", [&]() {
    config.mode = Mode::client;
    return td::Status::OK();
  });
  p.add_checked_option('\0', "client-id", "client ID for unique key generation (default: 0)", [&](td::Slice arg) {
    TRY_RESULT(v, td::to_integer_safe<td::uint32>(arg));
    config.client_id = v;
    return td::Status::OK();
  });
  p.add_checked_option('a', "addr", "local address (ip:port)", [&](td::Slice arg) {
    TRY_STATUS(config.local_addr.init_host_port(arg.str()));
    return td::Status::OK();
  });
  p.add_checked_option('\0', "public-addr", "public address to advertise (ip:port)", [&](td::Slice arg) {
    TRY_STATUS(config.public_addr.init_host_port(arg.str()));
    return td::Status::OK();
  });
  p.add_checked_option('s', "server-addr", "server address (ip:port) for client mode", [&](td::Slice arg) {
    TRY_STATUS(config.server_addr.init_host_port(arg.str()));
    return td::Status::OK();
  });
  p.add_checked_option('\0', "ack-thresh", "ngtcp2 immediate-ack packet threshold (default 32)", [&](td::Slice arg) {
    TRY_RESULT(v, td::to_integer_safe<td::uint32>(arg));
    config.ack_thresh = v;
    return td::Status::OK();
  });
  p.add_checked_option('\0', "max-ack-delay-ms", "advertised max ack delay in ms (default 10)", [&](td::Slice arg) {
    config.max_ack_delay_seconds = td::to_double(arg) / 1000.0;
    return td::Status::OK();
  });
  p.add_checked_option('\0', "no-limits", "disable per-address connection rate/flood limits (bench only)", [&]() {
    config.no_limits = true;
    return td::Status::OK();
  });
  p.add_checked_option('p', "prometheus", "enable prometheus exporter on the mode default port", [&]() {
    prometheus_default_port = true;
    return td::Status::OK();
  });
  p.add_checked_option('\0', "server-id", "server identity index; servers must all differ (default 0)",
                       [&](td::Slice arg) {
                         TRY_RESULT(v, td::to_integer_safe<td::uint32>(arg));
                         config.server_id = v;
                         return td::Status::OK();
                       });
  p.add_checked_option('\0', "connections", "peer sessions this client owns, spread over the servers (default: 1)",
                       [&](td::Slice arg) {
                         TRY_RESULT(value, td::to_integer_safe<td::uint32>(arg));
                         if (value == 0) {
                           return td::Status::Error("connections must be positive");
                         }
                         config.connections = value;
                         return td::Status::OK();
                       });
  p.add_checked_option('\0', "connect-inflight", "concurrent session readiness probes during setup (default: 256)",
                       [&](td::Slice arg) {
                         TRY_RESULT(value, td::to_integer_safe<td::uint32>(arg));
                         if (value == 0) {
                           return td::Status::Error("connect-inflight must be positive");
                         }
                         config.connect_inflight = value;
                         return td::Status::OK();
                       });
  p.add_checked_option('\0', "servers", "client only: fan out over N servers at consecutive ports from --server-addr",
                       [&](td::Slice arg) {
                         TRY_RESULT(v, td::to_integer_safe<td::uint32>(arg));
                         config.servers = v;
                         return td::Status::OK();
                       });
  p.add_checked_option('\0', "messages", "send fire-and-forget messages instead of queries (needs --rate)", [&]() {
    config.messages = true;
    return td::Status::OK();
  });
  p.add_checked_option('\0', "rate", "messages/sec offered in --messages mode", [&](td::Slice arg) {
    TRY_RESULT(v, td::to_integer_safe<td::uint32>(arg));
    config.rate = v;
    return td::Status::OK();
  });
  p.add_checked_option('\0', "message-burst", "messages emitted per timer event (default: 1)", [&](td::Slice arg) {
    TRY_RESULT(v, td::to_integer_safe<td::uint32>(arg));
    config.message_burst = v;
    return td::Status::OK();
  });
  p.add_checked_option('\0', "stay-alive", "after readiness or finite queries, keep the established peer idle", [&]() {
    config.stay_alive = true;
    return td::Status::OK();
  });
  p.add_checked_option('\0', "prometheus-port", "exporter port (default 19777 server, 29777 client)",
                       [&](td::Slice arg) {
                         TRY_RESULT(v, td::to_integer_safe<int>(arg));
                         config.prometheus_port = v;
                         return td::Status::OK();
                       });

  auto S = p.run(argc, argv);
  if (S.is_error()) {
    LOG(ERROR) << "Failed to parse options: " << S.error();
    std::_Exit(1);
  }

  if (config.mode == Mode::unset) {
    LOG(FATAL) << "Choose --server or --client";
  }

  if (prometheus_default_port && !config.prometheus_port) {
    config.prometheus_port = config.mode == Mode::client ? 29777 : 19777;
  }
  if (config.prometheus_port < 0 || config.prometheus_port > 65535) {
    LOG(FATAL) << "--prometheus-port must be between 0 and 65535";
  }
  if (config.threads == 0) {
    LOG(FATAL) << "--threads must be at least 1";
  }
  if (!std::isfinite(config.timeout) || config.timeout <= 0) {
    LOG(FATAL) << "--timeout must be finite and positive";
  }
  if (!std::isfinite(config.test_timeout) || config.test_timeout <= 0) {
    LOG(FATAL) << "--test-timeout must be finite and positive";
  }
  if (config.max_ack_delay_seconds &&
      (!std::isfinite(*config.max_ack_delay_seconds) || *config.max_ack_delay_seconds < 0)) {
    LOG(FATAL) << "--max-ack-delay-ms must be finite and nonnegative";
  }
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

  const char* mode_str = "unknown";
  switch (config.mode) {
    case Mode::unset:
      UNREACHABLE();
    case Mode::server:
      mode_str = "server";
      break;
    case Mode::client:
      mode_str = "client";
      break;
  }
  if (config.messages && config.rate == 0) {
    LOG(FATAL) << "--messages needs --rate: an unpaced fire-and-forget loop measures the queue, not the transport";
  }
  if (config.message_burst == 0) {
    LOG(FATAL) << "--message-burst must be at least 1";
  }
  if (config.mode == Mode::client && !config.messages && config.num_queries == 0 && !config.stay_alive) {
    LOG(FATAL) << "zero queries require --stay-alive";
  }
  if (config.messages && config.stay_alive) {
    LOG(FATAL) << "--stay-alive is only meaningful in query mode";
  }
  if (config.query_size == 0 || config.response_size == 0) {
    LOG(FATAL) << "query and response sizes must be positive";
  }
  if (config.protocol == Protocol::adnl && config.query_size > ton::adnl::Adnl::huge_packet_max_size()) {
    LOG(FATAL) << "raw ADNL requests and messages are limited to " << ton::adnl::Adnl::huge_packet_max_size()
               << " bytes";
  }
  if (config.protocol == Protocol::adnl && config.response_size > ton::adnl::Adnl::get_mtu()) {
    LOG(FATAL) << "raw ADNL responses are limited to " << ton::adnl::Adnl::get_mtu() << " bytes";
  }
  if (config.servers == 0) {
    LOG(FATAL) << "--servers must be at least 1";
  }
  if (static_cast<td::uint64>(config.server_id) + config.servers - 1 > std::numeric_limits<td::uint32>::max()) {
    LOG(FATAL) << "server identity range exceeds uint32";
  }
  if (config.mode == Mode::client &&
      static_cast<td::uint64>(config.server_addr.get_port()) + config.servers - 1 > 65535) {
    LOG(FATAL) << "server port range exceeds 65535";
  }
  if (config.protocol == Protocol::quic) {
    constexpr td::uint64 port_offset = 1000;
    const auto& self_addr = config.public_addr.is_valid() ? config.public_addr : config.local_addr;
    auto self_port = static_cast<td::uint64>(self_addr.get_port());
    if (self_port == 0 || self_port + port_offset > 65535) {
      LOG(FATAL) << "QUIC local ADNL port must be between 1 and 64535";
    }
    if (config.mode == Mode::client) {
      auto first_port = static_cast<td::uint64>(config.server_addr.get_port());
      auto last_port = first_port + config.servers - 1;
      if (first_port == 0 || last_port + port_offset > 65535) {
        LOG(FATAL) << "QUIC remote ADNL port range must be between 1 and 64535";
      }
    }
  }
  LOG(ERROR) << "Starting benchmark (mode: " << mode_str << ", protocol: " << protocol_name(config.protocol) << ")";

  switch (config.mode) {
    case Mode::unset:
      UNREACHABLE();
    case Mode::server:
      run_server(config);
      break;
    case Mode::client:
      run_client(config);
      break;
  }

  return 0;
}
