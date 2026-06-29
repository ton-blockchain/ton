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
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "adnl/adnl-test-loopback-implementation.h"
#include "adnl/adnl.h"
#include "auto/tl/ton_api.h"
#include "common/checksum.h"
#include "common/errorlog.h"
#include "dht/dht.h"
#include "keys/keys.hpp"
#include "overlay/overlay-id.hpp"
#include "overlay/overlay-manager.h"
#include "overlay/overlay.h"
#include "overlay/overlays.h"
#include "td/actor/actor.h"
#include "td/utils/Time.h"
#include "td/utils/buffer.h"
#include "td/utils/filesystem.h"
#include "td/utils/format.h"
#include "td/utils/misc.h"
#include "td/utils/port/path.h"
#include "td/utils/port/signals.h"
#include "test/plumtree/graph.h"
#include "test/plumtree/scheduler.h"
#include "test/plumtree/transport.h"
#include "test/plumtree/util.h"
#include "tl-utils/common-utils.hpp"

namespace {

using namespace ton::overlay::plumtree_sim;

constexpr td::uint32 MAX_IDLE_REPAIR_ROUNDS = 20;

enum class BroadcastMode { Fec, Simple };

struct Settings {
  bool smoke = false;
  BroadcastMode broadcast_mode = BroadcastMode::Fec;
  std::string graph_path;
  std::size_t limit = 0;
  std::size_t payload_bytes = 32768;
  td::uint32 plumtree_neighbours = 20;
  double jitter = 0.1;
  double bandwidth_mb_s = 100.0;
  td::uint32 broadcast_count = 5;
  td::uint64 seed = 1;
};

struct SimNode {
  ton::PrivateKey validator_key;
  ton::PublicKeyHash validator_id;
  ton::PublicKey validator_id_full;
  ton::adnl::AdnlNodeIdShort adnl_id;
  ton::adnl::AdnlNodeIdFull adnl_id_full;
};

struct NodeTrafficSummary {
  double average_received_bytes = 0.0;
  double average_sent_bytes = 0.0;
  td::uint64 max_received_bytes = 0;
  td::uint64 max_sent_bytes = 0;
  std::string max_received_node;
  std::string max_sent_node;
};

struct DeliveryDiversity {
  std::size_t full_node_hashes = 0;
  std::size_t full_node_sources = 0;
};

NodeTrafficSummary summarize_node_traffic(const Graph &graph, const std::vector<td::uint64> &received_bytes,
                                          const std::vector<td::uint64> &sent_bytes) {
  NodeTrafficSummary summary;
  if (graph.nodes.empty()) {
    return summary;
  }
  td::uint64 total_received = 0;
  td::uint64 total_sent = 0;
  for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
    auto received = i < received_bytes.size() ? received_bytes[i] : 0;
    auto sent = i < sent_bytes.size() ? sent_bytes[i] : 0;
    total_received += received;
    total_sent += sent;
    if (i == 0 || received > summary.max_received_bytes) {
      summary.max_received_bytes = received;
      summary.max_received_node = graph.nodes[i].graph_id;
    }
    if (i == 0 || sent > summary.max_sent_bytes) {
      summary.max_sent_bytes = sent;
      summary.max_sent_node = graph.nodes[i].graph_id;
    }
  }
  summary.average_received_bytes = static_cast<double>(total_received) / static_cast<double>(graph.nodes.size());
  summary.average_sent_bytes = static_cast<double>(total_sent) / static_cast<double>(graph.nodes.size());
  return summary;
}

DeliveryDiversity summarize_full_node_diversity(const Graph &graph, const std::vector<bool> &delivered,
                                                const std::vector<td::Bits256> &payload_hashes,
                                                const std::vector<ton::PublicKeyHash> &sources) {
  std::set<td::Bits256> hashes;
  std::set<ton::PublicKeyHash> source_hashes;
  for (std::size_t i = 0;
       i < graph.nodes.size() && i < delivered.size() && i < payload_hashes.size() && i < sources.size(); ++i) {
    if (graph.nodes[i].is_validator || !delivered[i]) {
      continue;
    }
    hashes.insert(payload_hashes[i]);
    source_hashes.insert(sources[i]);
  }
  return DeliveryDiversity{hashes.size(), source_hashes.size()};
}

std::vector<td::uint64> subtract_counters(const std::vector<td::uint64> &after, const std::vector<td::uint64> &before) {
  std::vector<td::uint64> result(after.size(), 0);
  for (std::size_t i = 0; i < after.size(); ++i) {
    auto old_value = i < before.size() ? before[i] : 0;
    result[i] = after[i] >= old_value ? after[i] - old_value : 0;
  }
  return result;
}

std::vector<bool> reachable_nodes(const std::vector<std::vector<std::size_t>> &out_neighbours,
                                  const std::vector<std::size_t> &roots) {
  std::vector<bool> reachable(out_neighbours.size(), false);
  std::vector<std::size_t> queue;
  queue.reserve(out_neighbours.size());
  for (auto root : roots) {
    if (root < reachable.size() && !reachable[root]) {
      reachable[root] = true;
      queue.push_back(root);
    }
  }
  for (std::size_t cursor = 0; cursor < queue.size(); ++cursor) {
    auto node = queue[cursor];
    for (auto peer : out_neighbours[node]) {
      if (peer < reachable.size() && !reachable[peer]) {
        reachable[peer] = true;
        queue.push_back(peer);
      }
    }
  }
  return reachable;
}

std::size_t count_true(const std::vector<bool> &values) {
  return static_cast<std::size_t>(std::count(values.begin(), values.end(), true));
}

std::string format_ms(double value) {
  if (value < 0.0) {
    return "-";
  }
  std::ostringstream out;
  out << std::fixed << std::setprecision(1) << value;
  return out.str();
}

std::string format_bytes(double bytes) {
  static constexpr double KB = 1024.0;
  static constexpr double MB = 1024.0 * KB;
  static constexpr double GB = 1024.0 * MB;
  std::ostringstream out;
  if (bytes >= GB) {
    out << std::fixed << std::setprecision(2) << (bytes / GB) << "G";
  } else if (bytes >= MB) {
    out << std::fixed << std::setprecision(2) << (bytes / MB) << "M";
  } else if (bytes >= KB) {
    out << std::fixed << std::setprecision(1) << (bytes / KB) << "K";
  } else {
    out << static_cast<td::uint64>(bytes) << "B";
  }
  return out.str();
}

td::Result<BroadcastMode> parse_broadcast_mode(td::Slice value) {
  if (value == "fec") {
    return BroadcastMode::Fec;
  }
  if (value == "simple") {
    return BroadcastMode::Simple;
  }
  return td::Status::Error(PSTRING() << "unknown broadcast mode: " << value);
}

const char *broadcast_mode_name(BroadcastMode mode) {
  switch (mode) {
    case BroadcastMode::Fec:
      return "fec";
    case BroadcastMode::Simple:
      return "simple";
  }
  UNREACHABLE();
}

void print_table_header() {
  std::cout << std::right << std::setw(4) << "#"
            << " " << std::setw(7) << "senders"
            << " " << std::setw(11) << "delivered"
            << " " << std::setw(8) << "p90"
            << " " << std::setw(8) << "p95"
            << " " << std::setw(8) << "p99"
            << " " << std::setw(8) << "last"
            << " " << std::setw(10) << "avg in"
            << " " << std::setw(10) << "avg out"
            << " " << std::setw(10) << "max in"
            << " " << std::setw(10) << "max out"
            << " " << std::setw(9) << "sent"
            << " " << std::setw(8) << "dup"
            << " " << std::setw(7) << "dup%" << "\n";
}

void print_usage() {
  std::cout << "Usage: plumtree-graph-sim [--smoke | --graph PATH] [options]\n"
               "  --limit N                 Limit loaded graph nodes after filtering\n"
               "  --broadcast-mode MODE     Plumtree mode: fec or simple (default fec)\n"
               "  --payload-bytes N         Broadcast payload size (default 32768)\n"
               "  --plumtree-neighbours N   Plumtree active neighbour target (default 20)\n"
               "  --jitter N                Relative latency jitter, same as JS simulator (default 0.1)\n"
               "  --bandwidth-mb N          Per-message bandwidth model in MB/s (default 100)\n"
               "  --broadcast-count N       Sequential broadcasts to send on the same overlay graph (default 5)\n"
               "  --seed N                  Deterministic payload/key seed label (default 1)\n";
}

td::Result<Settings> parse_args(int argc, char **argv) {
  Settings settings;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    auto read_value = [&](td::Slice name) -> td::Result<std::string> {
      if (i + 1 >= argc) {
        return td::Status::Error(PSTRING() << "missing value for " << name);
      }
      return std::string(argv[++i]);
    };
    if (arg == "--help" || arg == "-h") {
      print_usage();
      std::cout.flush();
      std::_Exit(0);
    } else if (arg == "--smoke") {
      settings.smoke = true;
    } else if (arg == "--graph") {
      TRY_RESULT(value, read_value(arg));
      settings.graph_path = value;
    } else if (arg == "--broadcast-mode" || arg == "--mode") {
      TRY_RESULT(value, read_value(arg));
      TRY_RESULT(mode, parse_broadcast_mode(value));
      settings.broadcast_mode = mode;
    } else if (arg == "--limit") {
      TRY_RESULT(value, read_value(arg));
      settings.limit = static_cast<std::size_t>(std::stoull(value));
    } else if (arg == "--payload-bytes") {
      TRY_RESULT(value, read_value(arg));
      settings.payload_bytes = static_cast<std::size_t>(std::stoull(value));
    } else if (arg == "--plumtree-neighbours") {
      TRY_RESULT(value, read_value(arg));
      settings.plumtree_neighbours = static_cast<td::uint32>(std::stoul(value));
    } else if (arg == "--jitter") {
      TRY_RESULT(value, read_value(arg));
      settings.jitter = std::stod(value);
    } else if (arg == "--bandwidth-mb") {
      TRY_RESULT(value, read_value(arg));
      settings.bandwidth_mb_s = std::stod(value);
    } else if (arg == "--broadcast-count") {
      TRY_RESULT(value, read_value(arg));
      settings.broadcast_count = static_cast<td::uint32>(std::stoul(value));
    } else if (arg == "--seed") {
      TRY_RESULT(value, read_value(arg));
      settings.seed = static_cast<td::uint64>(std::stoull(value));
    } else {
      return td::Status::Error(PSTRING() << "unknown argument: " << arg);
    }
  }
  if (settings.graph_path.empty()) {
    settings.smoke = true;
  }
  return settings;
}

}  // namespace

int main(int argc, char **argv) {
  SET_VERBOSITY_LEVEL(verbosity_ERROR);
  td::set_default_failure_signal_handler().ensure();

  auto settings_r = parse_args(argc, argv);
  if (settings_r.is_error()) {
    std::cerr << settings_r.move_as_error().to_string() << "\n";
    print_usage();
    return 2;
  }
  auto settings = settings_r.move_as_ok();

  auto graph_r = settings.smoke ? td::Result<Graph>(make_smoke_graph(12))
                                : load_graph(settings.graph_path, settings.limit, settings.plumtree_neighbours);
  if (graph_r.is_error()) {
    std::cerr << graph_r.move_as_error().to_string() << "\n";
    return 2;
  }
  auto graph = graph_r.move_as_ok();
  if (graph.nodes.empty()) {
    std::cerr << "graph has no usable nodes\n";
    return 2;
  }

  std::vector<std::size_t> validators;
  for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
    if (graph.nodes[i].is_validator) {
      validators.push_back(i);
    }
  }
  if (validators.empty()) {
    for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
      validators.push_back(i);
      graph.nodes[i].is_validator = true;
    }
  }
  auto db_root = td::mkdtemp(".", PSTRING() << "tmp-dir-plumtree-graph-sim-" << settings.seed << "-").move_as_ok();

  td::Time::allow_freezes();
  td::Time::freeze();
  const double base_time = td::Time::now();

  auto network = std::make_shared<SimNetwork>();
  network->base_time = base_time;
  network->geo_alpha_ms = graph.geo_alpha_ms;
  network->geo_beta_ms_per_km = graph.geo_beta_ms_per_km;
  network->jitter = settings.jitter;
  network->bandwidth_bytes_s = std::max(1.0, settings.bandwidth_mb_s * 1000000.0);
  network->random_state = static_cast<td::uint32>(settings.seed);
  network->sent_bytes_by_node.assign(graph.nodes.size(), 0);
  network->received_bytes_by_node.assign(graph.nodes.size(), 0);
  network->tx_free_at_by_node.assign(graph.nodes.size(), 0.0);
  network->rx_free_at_by_node.assign(graph.nodes.size(), 0.0);
  network->geo_by_node.reserve(graph.nodes.size());
  for (const auto &node : graph.nodes) {
    network->geo_by_node.push_back(SimGeoPoint{node.has_geo, node.lat, node.lon});
  }

  auto delivery = std::make_shared<DeliveryState>(graph.nodes.size());
  td::actor::ActorOwn<ton::keyring::Keyring> keyring;
  td::actor::ActorOwn<ton::adnl::TestLoopbackNetworkManager> network_manager;
  td::actor::ActorOwn<ton::adnl::Adnl> adnl;
  td::actor::ActorOwn<SimulatedSender> plumtree_sender;
  td::actor::ActorOwn<ton::overlay::Overlays> overlay_manager;

  td::actor::Scheduler scheduler({0}, true);
  scheduler.run_in_context([&] {
    ton::errorlog::ErrorLog::create(db_root);
    keyring = ton::keyring::Keyring::create(db_root);
    network_manager = td::actor::create_actor<ton::adnl::TestLoopbackNetworkManager>("plumtree sim net");
    adnl = ton::adnl::Adnl::create(db_root, keyring.get());
    plumtree_sender = td::actor::create_actor<SimulatedSender>("plumtree sim sender", network);
    overlay_manager =
        ton::overlay::Overlays::create(db_root, keyring.get(), adnl.get(), td::actor::ActorId<ton::dht::Dht>{});
    network->overlay_manager = td::actor::actor_dynamic_cast<ton::overlay::OverlayManager>(overlay_manager.get());
    td::actor::send_closure(adnl, &ton::adnl::Adnl::register_network_manager, network_manager.get());
  });
  pump_scheduler(scheduler, 64);

  std::vector<SimNode> nodes(graph.nodes.size());
  std::vector<ton::adnl::AdnlNodeIdShort> adnl_ids;
  std::vector<ton::PublicKeyHash> validator_ids;
  adnl_ids.reserve(graph.nodes.size());
  validator_ids.reserve(validators.size());

  auto overlay_id_full =
      ton::create_serialize_tl_object<ton::ton_api::pub_overlay>(td::BufferSlice("PLUMTREE-GRAPH-SIM"));
  ton::overlay::OverlayIdFull overlay_id(overlay_id_full.clone());
  auto overlay_id_short = overlay_id.compute_short_id();
  auto out_neighbours = build_out_neighbours(graph, settings.plumtree_neighbours);
  auto known_peers = build_known_peers_by_node(out_neighbours);
  std::vector<std::size_t> validator_roots;
  validator_roots.reserve(validators.size());
  for (auto node_index : validators) {
    validator_roots.push_back(node_index);
  }
  auto expected_delivery = reachable_nodes(out_neighbours, validator_roots);
  auto expected_delivered = count_true(expected_delivery);

  ton::overlay::OverlayOptions opts;
  opts.enable_plumtree_broadcast_ = true;
  opts.plumtree_broadcast_sender_ = plumtree_sender.get();
  opts.max_neighbours_ = 5;

  scheduler.run_in_context([&] {
    auto addr = ton::adnl::TestLoopbackNetworkManager::generate_dummy_addr_list();
    for (std::size_t i = 0; i < nodes.size(); ++i) {
      auto adnl_key = make_key(PSTRING() << "plumtree-sim-adnl-" << settings.seed << "-" << i);
      auto adnl_public = adnl_key.compute_public_key();
      nodes[i].adnl_id_full = ton::adnl::AdnlNodeIdFull{adnl_public};
      nodes[i].adnl_id = ton::adnl::AdnlNodeIdShort{adnl_public.compute_short_id()};
      network->node_by_adnl.emplace(nodes[i].adnl_id, i);
      td::actor::send_closure(keyring, &ton::keyring::Keyring::add_key, std::move(adnl_key), true, [](td::Result<>) {});
      td::actor::send_closure(adnl, &ton::adnl::Adnl::add_id, ton::adnl::AdnlNodeIdFull{adnl_public}, addr,
                              static_cast<td::uint8>(0));
      td::actor::send_closure(network_manager, &ton::adnl::TestLoopbackNetworkManager::add_node_id, nodes[i].adnl_id,
                              true, true);

      nodes[i].validator_key = make_key(PSTRING() << "plumtree-sim-validator-" << settings.seed << "-" << i);
      nodes[i].validator_id_full = nodes[i].validator_key.compute_public_key();
      nodes[i].validator_id = nodes[i].validator_id_full.compute_short_id();
      auto validator_key = nodes[i].validator_key;
      td::actor::send_closure(keyring, &ton::keyring::Keyring::add_key, std::move(validator_key), true,
                              [](td::Result<>) {});

      adnl_ids.push_back(nodes[i].adnl_id);
      if (graph.nodes[i].is_validator) {
        validator_ids.push_back(nodes[i].validator_id);
      }
    }
  });
  pump_scheduler(scheduler, 64);

  std::map<ton::PublicKeyHash, td::uint32> authorized_keys;
  for (const auto &key : validator_ids) {
    authorized_keys.emplace(key, ton::overlay::Overlays::max_fec_broadcast_size());
  }
  ton::overlay::OverlayPrivacyRules rules{ton::overlay::Overlays::max_fec_broadcast_size(),
                                          ton::overlay::CertificateFlags::AllowFec, std::move(authorized_keys)};

  scheduler.run_in_context([&] {
    for (std::size_t i = 0; i < nodes.size(); ++i) {
      std::vector<ton::adnl::AdnlNodeIdShort> peer_ids;
      for (auto peer_index : known_peers[i]) {
        peer_ids.push_back(nodes[peer_index].adnl_id);
      }
      std::vector<ton::adnl::AdnlNodeIdShort> plumtree_peer_ids;
      for (auto peer_index : out_neighbours[i]) {
        plumtree_peer_ids.push_back(nodes[peer_index].adnl_id);
      }
      td::actor::send_closure(overlay_manager, &ton::overlay::Overlays::create_private_overlay_ex, nodes[i].adnl_id,
                              ton::overlay::OverlayIdFull(overlay_id_full.clone()), std::move(peer_ids),
                              std::make_unique<DeliveryCallback>(delivery, i), rules, "", opts);
      td::actor::send_closure(network->overlay_manager, &ton::overlay::OverlayManager::set_test_plumtree_neighbours,
                              nodes[i].adnl_id, overlay_id_short, std::move(plumtree_peer_ids));
    }
  });
  pump_scheduler(scheduler, 128);

  double sim_time_s = 0.0;
  bool all_expected_delivered = true;
  std::cout << "Plumtree graph simulation: mode=" << broadcast_mode_name(settings.broadcast_mode)
            << ", all-validators-send, nodes=" << graph.nodes.size() << ", validators=" << validators.size()
            << ", senders=" << validators.size() << ", tree_slots=" << opts.plumtree_fec_options_.tree_slots_
            << ", expected=" << expected_delivered << ", unreachable=" << (graph.nodes.size() - expected_delivered);
  std::cout << ", broadcasts=" << settings.broadcast_count
            << ", payload=" << format_bytes(static_cast<double>(settings.payload_bytes)) << "\n";
  print_table_header();
  std::cout.flush();
  for (td::uint32 broadcast_index = 0; broadcast_index < settings.broadcast_count; ++broadcast_index) {
    td::BufferSlice payload(settings.payload_bytes);
    fill_payload(payload.as_slice(), settings.seed + static_cast<td::uint64>(broadcast_index) * 0x9e3779b97f4a7c15ULL);
    auto payload_hash = td::sha256_bits256(payload.as_slice());
    auto broadcast_id = payload_hash;
    delivery->start_broadcast(payload_hash, expected_delivery, td::Time::now(),
                              settings.broadcast_mode == BroadcastMode::Simple);
    auto sent_bytes_before = network->sent_bytes_count();
    auto payload_deliveries_before = network->payload_deliveries_count();
    auto prune_messages_before = network->prune_messages_count();
    auto received_by_node_before = network->received_bytes_by_node_snapshot();
    auto sent_by_node_before = network->sent_bytes_by_node_snapshot();

    scheduler.run_in_context([&] {
      for (std::size_t local_index = 0; local_index < validators.size(); ++local_index) {
        auto node_index = validators[local_index];
        td::BufferSlice payload_for_sender;
        if (settings.broadcast_mode == BroadcastMode::Simple) {
          payload_for_sender = td::BufferSlice(settings.payload_bytes);
          fill_payload(payload_for_sender.as_slice(),
                       settings.seed + static_cast<td::uint64>(broadcast_index) * 0x9e3779b97f4a7c15ULL +
                           static_cast<td::uint64>(local_index + 1) * 0xbf58476d1ce4e5b9ULL);
        } else {
          payload_for_sender = local_index + 1 == validators.size() ? std::move(payload) : payload.clone();
        }
        if (settings.broadcast_mode == BroadcastMode::Fec) {
          td::actor::send_closure(overlay_manager, &ton::overlay::Overlays::send_broadcast_plumtree_fec,
                                  nodes[node_index].adnl_id, overlay_id_short, nodes[node_index].validator_id,
                                  ton::overlay::Overlays::BroadcastFlagAnySender(), std::move(payload_for_sender));
        } else {
          td::actor::send_closure(overlay_manager, &ton::overlay::Overlays::send_broadcast_plumtree,
                                  nodes[node_index].adnl_id, overlay_id_short, nodes[node_index].validator_id,
                                  ton::overlay::Overlays::BroadcastFlagAnySender(), broadcast_id,
                                  std::move(payload_for_sender));
        }
      }
    });
    pump_scheduler(scheduler, 64);

    td::uint32 idle_repair_rounds = 0;
    auto repair_step_s = std::max(0.001, static_cast<double>(opts.plumtree_fec_options_.repair_timeout_ms_) / 1000.0);
    while (delivery->expected_remaining_count() != 0) {
      auto now_s = std::max(0.0, td::Time::now() - base_time);
      double next_time_s = now_s;
      if (auto event_time = network->next_event_time()) {
        next_time_s = std::max(now_s, event_time.value());
        idle_repair_rounds = 0;
      } else {
        if (++idle_repair_rounds > MAX_IDLE_REPAIR_ROUNDS) {
          break;
        }
        next_time_s = now_s + repair_step_s;
      }
      td::Time::jump_in_future(base_time + next_time_s);
      scheduler.run(0);
      sim_time_s = next_time_s;

      auto due_events = network->pop_due_events(sim_time_s);
      if (!due_events.empty()) {
        scheduler.run_in_context([&] {
          for (auto &event : due_events) {
            switch (event.kind) {
              case SimEventKind::Message:
                td::actor::send_closure(network->overlay_manager, &ton::overlay::OverlayManager::receive_message,
                                        event.src, event.dst, std::move(event.data));
                break;
              case SimEventKind::Query:
                td::actor::send_closure(network->overlay_manager, &ton::overlay::OverlayManager::receive_query,
                                        event.src, event.dst, std::move(event.data), std::move(event.promise));
                break;
              case SimEventKind::Response:
                event.promise.set_value(std::move(event.data));
                break;
            }
          }
        });
        pump_scheduler(scheduler);
      }
    }

    auto delivered = delivery->delivered_count();
    auto expected_remaining = delivery->expected_remaining_count();
    all_expected_delivered = all_expected_delivered && expected_remaining == 0;
    auto delivery_ms = delivery->delivery_times();
    auto delivered_flags = delivery->delivered_flags();
    auto delivered_hashes = delivery->delivered_hashes();
    auto delivered_sources = delivery->delivered_source_hashes();
    auto sent_bytes = network->sent_bytes_count() - sent_bytes_before;
    auto payload_deliveries = network->payload_deliveries_count() - payload_deliveries_before;
    auto prune_messages = network->prune_messages_count() - prune_messages_before;
    auto duplicate_percent =
        payload_deliveries == 0 ? 0.0
                                : 100.0 * static_cast<double>(prune_messages) / static_cast<double>(payload_deliveries);
    auto received_by_node = subtract_counters(network->received_bytes_by_node_snapshot(), received_by_node_before);
    auto sent_by_node = subtract_counters(network->sent_bytes_by_node_snapshot(), sent_by_node_before);
    auto traffic_summary = summarize_node_traffic(graph, received_by_node, sent_by_node);
    auto p90_ms = percentile(delivery_ms, 0.90);
    auto p95_ms = percentile(delivery_ms, 0.95);
    auto p99_ms = percentile(delivery_ms, 0.99);
    auto last_decoded_ms = percentile(std::move(delivery_ms), 1.00);

    std::cout << std::right << std::setw(4) << broadcast_index << " ";
    std::cout << std::setw(7) << validators.size();
    std::cout << " " << std::setw(5) << delivered << "/" << std::left << std::setw(5) << expected_delivered
              << std::right << " " << std::setw(8) << format_ms(p90_ms) << " " << std::setw(8) << format_ms(p95_ms)
              << " " << std::setw(8) << format_ms(p99_ms) << " " << std::setw(8) << format_ms(last_decoded_ms) << " "
              << std::setw(10) << format_bytes(traffic_summary.average_received_bytes) << " " << std::setw(10)
              << format_bytes(traffic_summary.average_sent_bytes) << " " << std::setw(10)
              << format_bytes(static_cast<double>(traffic_summary.max_received_bytes)) << " " << std::setw(10)
              << format_bytes(static_cast<double>(traffic_summary.max_sent_bytes)) << " " << std::setw(9)
              << format_bytes(static_cast<double>(sent_bytes)) << " " << std::setw(8) << prune_messages << " "
              << std::setw(6) << std::fixed << std::setprecision(2) << duplicate_percent << "%" << "\n";
    if (settings.broadcast_mode == BroadcastMode::Simple) {
      auto diversity = summarize_full_node_diversity(graph, delivered_flags, delivered_hashes, delivered_sources);
      std::cout << "     divergent simple: full_node_payload_hashes=" << diversity.full_node_hashes
                << ", full_node_sources=" << diversity.full_node_sources << "\n";
      if (diversity.full_node_hashes < 2 || diversity.full_node_sources < 2) {
        all_expected_delivered = false;
      }
    }
    std::cout.flush();
  }

  td::rmrf(db_root).ignore();
  std::_Exit(all_expected_delivered ? 0 : 1);
}
