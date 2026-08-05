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
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "adnl/adnl-network-manager.h"
#include "adnl/adnl-peer-table.h"
#include "adnl/adnl.h"
#include "auto/tl/ton_api.hpp"
#include "keys/keys.hpp"
#include "metrics/collectors.h"
#include "quic/quic-sender.h"
#include "td/actor/coro_task.h"
#include "td/actor/coro_utils.h"
#include "td/utils/OptionParser.h"
#include "td/utils/Slice.h"
#include "td/utils/Timer.h"
#include "td/utils/as.h"
#include "td/utils/misc.h"
#include "td/utils/port/path.h"
#include "td/utils/port/signals.h"

#if TD_PORT_POSIX
#include <sys/resource.h>
#endif

namespace {

// Process CPU time (user + system, all threads). Both endpoints live in this
// process, so this is the combined cost of sending and receiving a message.
double process_cpu_seconds() {
#if TD_PORT_POSIX
  rusage usage;
  if (getrusage(RUSAGE_SELF, &usage) != 0) {
    return 0.0;
  }
  auto to_seconds = [](const timeval& t) { return t.tv_sec + t.tv_usec / 1e6; };
  return to_seconds(usage.ru_utime) + to_seconds(usage.ru_stime);
#else
  return 0.0;
#endif
}

constexpr td::uint32 PAYLOAD_SIZE = 240;
constexpr td::uint32 PAYLOAD_DATA_SIZE = 232;
constexpr td::int32 PAYLOAD_MAGIC = 0x6d626d74;
constexpr td::uint64 DEFAULT_RECEIVER_LAG = 16 * 1024;

enum class Protocol { adnl, quic };

struct Config {
  Protocol protocol{Protocol::adnl};
  bool quic_datagrams{false};
  bool quic_bidi{false};
  // Put the two nodes on two scheduler nodes of the same scheduler group. Each
  // node owns an IO/poll worker thread of its own, so the two QuicServer actors
  // (both created with with_poll) stop sharing one poll thread.
  bool split_schedulers{false};
  // Outstanding messages the sender allows before it waits for the receiver.
  td::uint64 receiver_lag{DEFAULT_RECEIVER_LAG};
  double duration{5.0};
  td::uint32 threads{std::max(1u, std::thread::hardware_concurrency())};
  td::uint16 server_port{29200};
  td::uint16 client_port{29201};
  double timeout{120.0};
};

const char* protocol_name(Protocol protocol) {
  switch (protocol) {
    case Protocol::adnl:
      return "adnl";
    case Protocol::quic:
      return "quic";
  }
  return "unknown";
}

ton::PrivateKey make_private_key(td::uint8 seed) {
  td::uint8 data[32];
  std::memset(data, seed, sizeof(data));
  return ton::PrivateKey{ton::privkeys::Ed25519{td::Slice(data, sizeof(data))}};
}

ton::adnl::AdnlAddressList make_addr_list(td::uint16 port) {
  td::IPAddress addr;
  addr.init_host_port(PSTRING() << "127.0.0.1:" << port).ensure();

  ton::adnl::AdnlAddressList list;
  list.add_udp_adnl_address(addr).ensure();
  list.set_version(static_cast<td::int32>(td::Clocks::system()));
  list.set_reinit_date(ton::adnl::Adnl::adnl_start_time());
  return list;
}

td::BufferSlice make_payload() {
  // 4-byte constructor + TL bytes(232): one length byte, 232 data bytes and
  // three padding bytes. This is the size of boxed Plumtree IHAVE wrapped in
  // overlay.message, while receiver work stays at the minimum TL dispatch.
  td::BufferSlice payload{PAYLOAD_SIZE};
  auto data = payload.as_slice();
  std::memset(data.data(), 0, data.size());
  td::as<td::int32>(data.data()) = PAYLOAD_MAGIC;
  data[4] = static_cast<char>(PAYLOAD_DATA_SIZE);
  return payload;
}

class MessageCounter {
 public:
  td::actor::StartedTask<td::Unit> expect(td::uint64 target) {
    auto [task, promise] = td::actor::StartedTask<td::Unit>::make_bridge();
    td::Promise<td::Unit> already_reached;
    {
      std::lock_guard guard(promise_mutex_);
      CHECK(!reached_);
      reached_ = std::move(promise);
      // receive() reads target_ outside this mutex, so one that crossed `target` while target_ was
      // still 0 took its fast path and fired nothing. Publishing the target first and re-reading the
      // count after is what closes that window; the promise comes back here and is fired below.
      //
      // The four operations of that handshake -- this store and load, and receive()'s increment and
      // load -- are sequentially consistent, which is what makes it airtight. Under acquire/release
      // alone the two threads store to one variable and load the other with nothing ordering the
      // store against the load, so both loads may read the stale value and the promise is left
      // pending forever. Sequential consistency puts all four in one total order S: if receive()'s
      // load read target_ == 0 it precedes this store in S, and if this load missed the increment it
      // precedes it in S; with each thread's own two operations ordered by S as well, that closes a
      // cycle. So they cannot both miss, and whichever did not fires the promise.
      target_.store(target);
      if (received_.load() >= target) {
        target_.store(0);
        already_reached = std::move(reached_);
      }
    }
    if (already_reached) {
      already_reached.set_value(td::Unit{});
    }
    return std::move(task);
  }

  void receive(td::BufferSlice data) {
    auto slice = data.as_slice();
    if (slice.size() != PAYLOAD_SIZE || td::as<td::int32>(slice.data()) != PAYLOAD_MAGIC ||
        static_cast<td::uint8>(slice[4]) != PAYLOAD_DATA_SIZE) {
      invalid_.fetch_add(1, std::memory_order_relaxed);
      return;
    }

    // Sequentially consistent, and paired with expect()'s store/load: see the argument there.
    auto received = received_.fetch_add(1) + 1;
    auto target = target_.load();
    if (target == 0 || received < target) {
      return;
    }

    td::Promise<td::Unit> reached;
    {
      std::lock_guard guard(promise_mutex_);
      target = target_.load();
      if (target == 0 || received < target) {
        return;
      }
      target_.store(0);
      reached = std::move(reached_);
    }
    reached.set_value(td::Unit{});
  }

  td::uint64 received() const {
    return received_.load(std::memory_order_relaxed);
  }

  td::uint64 invalid() const {
    return invalid_.load(std::memory_order_relaxed);
  }

 private:
  std::atomic<td::uint64> received_{0};
  std::atomic<td::uint64> invalid_{0};
  std::atomic<td::uint64> target_{0};
  std::mutex promise_mutex_;
  td::Promise<td::Unit> reached_;
};

class MessageCallback final : public ton::adnl::Adnl::Callback {
 public:
  explicit MessageCallback(std::shared_ptr<MessageCounter> counter) : counter_(std::move(counter)) {
  }

  void receive_message(ton::adnl::AdnlNodeIdShort, ton::adnl::AdnlNodeIdShort, td::BufferSlice data) override {
    counter_->receive(std::move(data));
  }

 private:
  std::shared_ptr<MessageCounter> counter_;
};

// Everything the benchmark needs from a node once it is up. Actor ownership
// stays inside the NodeBuilder that created it, so the handles can be shipped
// to a runner living on another scheduler.
struct NodeRef {
  ton::adnl::AdnlNodeIdShort id;
  ton::PublicKey public_key;
  td::uint16 port{0};
  td::actor::ActorId<ton::adnl::Adnl> adnl;
  td::actor::ActorId<ton::adnl::AdnlSenderInterface> sender;
  // Empty for the ADNL protocol; the batching histograms hang off this actor.
  td::actor::ActorId<ton::quic::QuicSender> quic;
};

// Creates and owns one node. Actors inherit the scheduler of the actor that
// creates them, so pinning this builder pins the whole node -- including the
// QuicServer, whose with_poll(true) binds it to that scheduler's poll thread.
class NodeBuilder final : public td::actor::Actor {
 public:
  NodeBuilder(Config config, std::string db_root, std::string name, td::uint16 port, td::uint8 key_seed)
      : config_(config)
      , db_root_(std::move(db_root))
      , name_(std::move(name))
      , port_(port)
      , key_(make_private_key(key_seed)) {
  }

  td::actor::Task<NodeRef> build() {
    auto id = ton::adnl::AdnlNodeIdShort{key_.compute_public_key().compute_short_id()};

    auto db = db_root_ + "/" + name_;
    td::mkdir(db).ensure();

    keyring_ = ton::keyring::Keyring::create(db);
    network_manager_ = ton::adnl::AdnlNetworkManager::create(port_);
    adnl_ = ton::adnl::Adnl::create(db, keyring_.get());
    td::actor::send_closure(adnl_, &ton::adnl::Adnl::register_network_manager, network_manager_.get());

    ton::adnl::AdnlCategoryMask category_mask;
    category_mask[0] = true;
    td::IPAddress addr;
    addr.init_host_port(PSTRING() << "127.0.0.1:" << port_).ensure();
    td::actor::send_closure(network_manager_, &ton::adnl::AdnlNetworkManager::add_self_addr, addr,
                            std::move(category_mask), 0);

    co_await td::actor::ask(keyring_, &ton::keyring::Keyring::add_key, key_, true);
    td::actor::send_closure(adnl_, &ton::adnl::Adnl::add_id, ton::adnl::AdnlNodeIdFull{key_.compute_public_key()},
                            make_addr_list(port_), td::uint8(0));

    td::actor::ActorId<ton::adnl::AdnlSenderInterface> sender = adnl_.get();
    if (config_.protocol == Protocol::quic) {
      ton::quic::QuicServer::Options options;
      // Messages ride unidirectional streams; without this credit the lag window deadlocks.
      options.max_streams_bidi = 2 * config_.receiver_lag;
      options.max_streams_uni = 2 * config_.receiver_lag;
      if (config_.quic_datagrams) {
        // Room for an MTU-sized message plus its framing, so no message is pushed onto a stream by
        // the advertised limit rather than by the path.
        options.max_datagram_frame_size = 1400;
      }
      options.message_streams_bidi = config_.quic_bidi;
      quic_sender_ = td::actor::create_actor<ton::quic::QuicSender>(
          "quic-" + name_, td::actor::actor_dynamic_cast<ton::adnl::AdnlPeerTable>(adnl_.get()), keyring_.get(),
          options);
      td::actor::send_closure(quic_sender_, &ton::quic::QuicSender::add_id, id);
      sender = quic_sender_.get();
    }

    co_await td::actor::Yield{};
    co_return NodeRef{id, key_.compute_public_key(), port_, adnl_.get(), sender, quic_sender_.get()};
  }

 private:
  Config config_;
  std::string db_root_;
  std::string name_;
  td::uint16 port_{0};
  ton::PrivateKey key_;

  td::actor::ActorOwn<ton::keyring::Keyring> keyring_;
  td::actor::ActorOwn<ton::adnl::AdnlNetworkManager> network_manager_;
  td::actor::ActorOwn<ton::adnl::Adnl> adnl_;
  td::actor::ActorOwn<ton::quic::QuicSender> quic_sender_;
};

class BenchmarkRunner final : public td::actor::Actor {
 public:
  BenchmarkRunner(Config config, std::string db_root, std::shared_ptr<MessageCounter> counter,
                  td::actor::SchedulerId server_scheduler)
      : config_(config)
      , db_root_(std::move(db_root))
      , counter_(std::move(counter))
      , server_scheduler_(server_scheduler) {
  }

  void start_up() override {
    alarm_timestamp() = td::Timestamp::in(config_.timeout);
    run().start_immediate().detach("adnl-quic-message-benchmark");
  }

  void alarm() override {
    LOG(FATAL) << "Benchmark timeout after " << config_.timeout << "s: received=" << counter_->received()
               << " invalid=" << counter_->invalid();
  }

 private:
  Config config_;
  std::string db_root_;
  std::shared_ptr<MessageCounter> counter_;
  td::actor::SchedulerId server_scheduler_;
  std::vector<td::actor::ActorOwn<NodeBuilder>> builders_;

  td::actor::Task<NodeRef> create_node(std::string name, td::uint16 port, td::uint8 key_seed,
                                       td::actor::SchedulerId scheduler) {
    auto builder = td::actor::create_actor<NodeBuilder>(
        td::actor::ActorOptions().with_name("build-" + name).on_scheduler(scheduler), config_, db_root_, name, port,
        key_seed);
    auto node = co_await td::actor::ask(builder.get(), &NodeBuilder::build);
    builders_.push_back(std::move(builder));
    co_return node;
  }

  void add_peer(const NodeRef& from, const NodeRef& to) {
    td::actor::send_closure(from.adnl, &ton::adnl::Adnl::add_peer, from.id, ton::adnl::AdnlNodeIdFull{to.public_key},
                            make_addr_list(to.port));
  }

  bool channel_is_ready(const ton::ton_api::adnl_stats& stats, ton::adnl::AdnlNodeIdShort local_id,
                        ton::adnl::AdnlNodeIdShort peer_id) const {
    for (const auto& local : stats.local_ids_) {
      if (local->short_id_ != local_id.bits256_value()) {
        continue;
      }
      for (const auto& peer : local->peers_) {
        if (peer->peer_id_ == peer_id.bits256_value() && peer->channel_status_ == 2) {
          return true;
        }
      }
    }
    return false;
  }

  td::actor::Task<td::Unit> wait_for_adnl_channel(td::actor::ActorId<ton::adnl::Adnl> adnl,
                                                  ton::adnl::AdnlNodeIdShort local_id,
                                                  ton::adnl::AdnlNodeIdShort peer_id) {
    auto deadline = td::Timestamp::in(10.0);
    while (true) {
      auto stats = co_await td::actor::ask(adnl, &ton::adnl::Adnl::get_stats, true);
      if (stats && channel_is_ready(*stats, local_id, peer_id)) {
        co_return td::Unit{};
      }
      LOG_CHECK(!deadline.is_in_past()) << "ADNL channel did not become ready";
      co_await td::actor::Yield{};
    }
  }

  td::actor::Task<td::Unit> wait_for_quic_connection(td::actor::ActorId<ton::adnl::AdnlSenderInterface> sender,
                                                     ton::adnl::AdnlNodeIdShort local_id,
                                                     ton::adnl::AdnlNodeIdShort peer_id) {
    auto deadline = td::Timestamp::in(10.0);
    while (true) {
      auto [task, promise] = td::actor::StartedTask<std::string>::make_bridge();
      td::actor::send_closure(sender, &ton::adnl::AdnlSenderInterface::get_conn_ip_str, local_id, peer_id,
                              std::move(promise));
      auto result = co_await std::move(task).wrap();
      if (result.is_ok()) {
        co_return td::Unit{};
      }
      LOG_CHECK(!deadline.is_in_past()) << "QUIC connection did not become ready: " << result.error();
      co_await td::actor::coro_sleep(td::Timestamp::in(0.01));
    }
  }

  // The batching histograms in Prometheus text form, filtered to the wire and batching tiers: this
  // is the same collect() the exporter drives, so a stderr run and a scrape agree by construction.
  td::actor::Task<td::Unit> print_quic_metrics(const char* who, td::actor::ActorId<ton::quic::QuicSender> quic) {
    ton::metrics::Sink sink;
    auto root = ton::metrics::Context{sink}.with_name("ton");
    co_await td::actor::ask(quic, &ton::quic::QuicSender::collect, root);
    auto text = std::move(sink).build().render();
    for (auto line : td::full_split(td::Slice(text), '\n')) {
      if (td::begins_with(line, "ton_quic_batching_") || td::begins_with(line, "ton_quic_wire_") ||
          td::begins_with(line, "ton_quic_transport_")) {
        std::cout << who << ' ' << line.str() << '\n';
      }
    }
    co_return td::Unit{};
  }

  td::actor::Task<td::Unit> run() {
    auto server = co_await create_node("server", config_.server_port, 1, server_scheduler_);
    auto client = co_await create_node("client", config_.client_port, 2, td::actor::SchedulerId{0});
    add_peer(client, server);
    add_peer(server, client);

    td::actor::send_closure(server.adnl, &ton::adnl::Adnl::subscribe, server.id,
                            ton::adnl::Adnl::int_to_bytestring(PAYLOAD_MAGIC),
                            std::make_unique<MessageCallback>(counter_));
    co_await td::actor::Yield{};

    auto sender = client.sender;
    if (config_.protocol == Protocol::quic) {
      co_await wait_for_quic_connection(sender, client.id, server.id);
    }

    auto payload = make_payload();

    // Establish the ADNL channel or QUIC connection outside the measurement.
    auto warmup_target = counter_->received() + 1;
    auto warmup = counter_->expect(warmup_target);
    td::actor::send_closure(sender, &ton::adnl::AdnlSenderInterface::send_message, client.id, server.id,
                            payload.clone());
    co_await std::move(warmup);

    if (config_.protocol == Protocol::adnl) {
      co_await wait_for_adnl_channel(client.adnl, client.id, server.id);
    }

    auto received_at_start = counter_->received();
    auto cpu_at_start = process_cpu_seconds();
    td::uint64 submitted = 0;
    td::Timer timer;

    while (timer.elapsed() < config_.duration) {
      td::actor::send_closure(sender, &ton::adnl::AdnlSenderInterface::send_message, client.id, server.id,
                              payload.clone());
      ++submitted;

      auto received = counter_->received() - received_at_start;
      // Keep the asynchronous receiver saturated without allowing an unbounded
      // actor/stream queue. This is aggregate flow control, not a message reply.
      if (submitted - std::min(submitted, received) >= config_.receiver_lag) {
        co_await counter_->expect(received_at_start + submitted - config_.receiver_lag / 2);
      }
    }

    auto elapsed = timer.elapsed();
    auto received = counter_->received() - received_at_start;
    LOG_CHECK(counter_->invalid() == 0) << "Received " << counter_->invalid() << " invalid messages";

    // Keep the measurement boundary above, but finish application delivery so
    // process CPU covers all submitted messages and repeated runs are stable.
    // Not with datagrams: lost ones never arrive, and waiting for them would
    // turn any loss into a timeout. There delivery_percent is a real loss rate.
    if (!config_.quic_datagrams) {
      co_await counter_->expect(received_at_start + submitted);
    }
    auto cpu_seconds = process_cpu_seconds() - cpu_at_start;

    auto messages_per_second = static_cast<double>(received) / elapsed;
    auto payload_mb_per_second = messages_per_second * PAYLOAD_SIZE / 1e6;
    auto ns_per_message = received == 0 ? 0.0 : elapsed * 1e9 / static_cast<double>(received);
    auto cpu_us_per_message = submitted == 0 ? 0.0 : cpu_seconds * 1e6 / static_cast<double>(submitted);
    auto delivery_percent =
        submitted == 0 ? 0.0 : 100.0 * static_cast<double>(received) / static_cast<double>(submitted);

    std::cout << std::fixed << std::setprecision(3);
    std::cout << "protocol=" << protocol_name(config_.protocol) << '\n';
    std::cout << "threads=" << config_.threads << '\n';
    std::cout << "split_schedulers=" << (config_.split_schedulers ? 1 : 0) << '\n';
    std::cout << "receiver_lag=" << config_.receiver_lag << '\n';
    std::cout << "payload_bytes=" << PAYLOAD_SIZE << '\n';
    std::cout << "submitted=" << submitted << '\n';
    std::cout << "received=" << received << '\n';
    std::cout << "delivery_percent=" << delivery_percent << '\n';
    std::cout << "elapsed_seconds=" << elapsed << '\n';
    std::cout << "ns_per_message=" << ns_per_message << '\n';
    std::cout << "messages_per_second=" << messages_per_second << '\n';
    std::cout << "payload_MB_per_second=" << payload_mb_per_second << '\n';
    std::cout << "cpu_seconds=" << cpu_seconds << '\n';
    std::cout << "cpu_us_per_message=" << cpu_us_per_message << '\n';
    if (config_.protocol == Protocol::quic) {
      co_await print_quic_metrics("client", client.quic);
      co_await print_quic_metrics("server", server.quic);
    }
    std::cout.flush();

    // Transport teardown is outside the benchmark and may depend on timers.
    // The next run removes the temporary database.
    std::_Exit(EXIT_SUCCESS);
  }
};

int run_benchmark(Config config) {
  // The transport bounds its pending-datagram queue at 4096 and drops the oldest on overflow;
  // streams block instead. The closed loop must therefore keep fewer datagrams outstanding than
  // that bound, or a dropped message makes the next lag gate wait forever.
  if (config.quic_datagrams) {
    config.receiver_lag = std::min<td::uint64>(config.receiver_lag, 2048);
  }
  std::string db_root = "/tmp/ton-bench-adnl-quic-message";
  td::rmrf(db_root).ignore();
  td::mkdir(db_root).ensure();

  auto counter = std::make_shared<MessageCounter>();

  // One scheduler node per endpoint in split mode: a node is exactly one poll
  // thread plus its CPU workers, and NodeInfo::io_threads_ is not honoured.
  std::vector<td::actor::Scheduler::NodeInfo> nodes(config.split_schedulers ? 2 : 1, config.threads);
  td::actor::SchedulerId server_scheduler{static_cast<td::uint8>(config.split_schedulers ? 1 : 0)};

  td::actor::Scheduler scheduler(std::move(nodes));
  scheduler.run_in_context([&] {
    td::actor::create_actor<BenchmarkRunner>("message-benchmark", config, db_root, counter, server_scheduler).release();
  });
  scheduler.run();

  td::rmrf(db_root).ignore();
  return 1;
}

}  // namespace

int main(int argc, char* argv[]) {
  SET_VERBOSITY_LEVEL(VERBOSITY_NAME(FATAL));
  td::set_default_failure_signal_handler().ensure();

  Config config;
  td::OptionParser options;
  options.set_description("ADNL/QUIC loopback fire-and-forget message benchmark");
  options.add_option('h', "help", "print help", [&]() {
    char buffer[16384];
    td::StringBuilder builder(td::MutableSlice{buffer, sizeof(buffer)});
    builder << options;
    std::cout << builder.as_cslice().c_str();
    std::exit(0);
  });
  options.add_checked_option('\0', "adnl", "benchmark ADNL custom messages (default)", [&]() {
    config.protocol = Protocol::adnl;
    return td::Status::OK();
  });
  options.add_checked_option('\0', "quic", "benchmark QUIC messages, one stream per message", [&]() {
    config.protocol = Protocol::quic;
    return td::Status::OK();
  });
  options.add_checked_option('\0', "quic-datagrams", "with --quic: carry messages in unreliable DATAGRAM frames",
                             [&]() {
                               config.quic_datagrams = true;
                               return td::Status::OK();
                             });
  options.add_checked_option('\0', "quic-bidi", "with --quic: messages on bidirectional streams with empty receipts",
                             [&]() {
                               config.quic_bidi = true;
                               return td::Status::OK();
                             });
  options.add_checked_option('\0', "split-schedulers", "run the two nodes on two scheduler nodes, one poll thread each",
                             [&]() {
                               config.split_schedulers = true;
                               return td::Status::OK();
                             });
  options.add_checked_option('\0', "lag", "outstanding messages allowed before the sender waits (default: 16384)",
                             [&](td::Slice arg) {
                               TRY_RESULT(value, td::to_integer_safe<td::uint64>(arg));
                               if (value < 2) {
                                 return td::Status::Error("lag must be at least 2");
                               }
                               config.receiver_lag = value;
                               return td::Status::OK();
                             });
  options.add_option('d', "duration", "measurement duration in seconds (default: 5)",
                     [&](td::Slice arg) { config.duration = td::to_double(arg); });
  options.add_checked_option('t', "threads", "scheduler threads (default: logical CPU count)", [&](td::Slice arg) {
    TRY_RESULT(value, td::to_integer_safe<td::uint32>(arg));
    if (value == 0) {
      return td::Status::Error("threads must be positive");
    }
    config.threads = value;
    return td::Status::OK();
  });
  options.add_checked_option('\0', "server-port", "ADNL server UDP port (default: 29200)", [&](td::Slice arg) {
    TRY_RESULT(value, td::to_integer_safe<td::uint16>(arg));
    config.server_port = value;
    return td::Status::OK();
  });
  options.add_checked_option('\0', "client-port", "ADNL client UDP port (default: 29201)", [&](td::Slice arg) {
    TRY_RESULT(value, td::to_integer_safe<td::uint16>(arg));
    config.client_port = value;
    return td::Status::OK();
  });
  options.add_option('\0', "timeout", "whole benchmark timeout in seconds (default: 120)",
                     [&](td::Slice arg) { config.timeout = td::to_double(arg); });

  auto status = options.run(argc, argv);
  if (status.is_error()) {
    LOG(ERROR) << "Failed to parse options: " << status.error();
    return 1;
  }
  if (config.server_port == config.client_port || config.server_port > 64535 || config.client_port > 64535) {
    LOG(ERROR) << "Ports must be distinct and at most 64535 (QUIC uses ADNL port + 1000)";
    return 1;
  }
  if (config.duration <= 0 || config.timeout <= config.duration) {
    LOG(ERROR) << "Duration must be positive and shorter than timeout";
    return 1;
  }

  return run_benchmark(config);
}
