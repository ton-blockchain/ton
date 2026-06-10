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
#include <map>
#include <set>
#include <string>
#include <vector>

#include "td/utils/JsonBuilder.h"
#include "td/utils/filesystem.h"
#include "td/utils/format.h"
#include "test/plumtree/graph.h"

namespace ton::overlay::plumtree_sim {
namespace {

const td::JsonValue *find_field(const td::JsonObject &object, td::Slice name) {
  for (const auto &field : object.field_values_) {
    if (field.first == name) {
      return &field.second;
    }
  }
  return nullptr;
}

std::string optional_node_id(const td::JsonObject &object) {
  for (td::Slice name : {td::Slice("adnl_id"), td::Slice("node"), td::Slice("id")}) {
    auto value = object.get_optional_string_field(name).move_as_ok();
    if (!value.empty()) {
      return value;
    }
  }
  return {};
}

bool public_ipv4(td::Slice ip) {
  td::uint32 parts[4] = {0, 0, 0, 0};
  std::size_t part_index = 0;
  td::uint32 current = 0;
  bool has_digit = false;
  for (char c : ip) {
    if (c >= '0' && c <= '9') {
      has_digit = true;
      current = current * 10 + static_cast<td::uint32>(c - '0');
      if (current > 255) {
        return false;
      }
    } else if (c == '.') {
      if (!has_digit || part_index >= 3) {
        return false;
      }
      parts[part_index++] = current;
      current = 0;
      has_digit = false;
    } else {
      return false;
    }
  }
  if (!has_digit || part_index != 3) {
    return false;
  }
  parts[part_index] = current;
  auto a = parts[0];
  auto b = parts[1];
  if (a == 0 || a == 10 || a == 127 || a >= 224) {
    return false;
  }
  if (a == 100 && b >= 64 && b <= 127) {
    return false;
  }
  if (a == 169 && b == 254) {
    return false;
  }
  if (a == 172 && b >= 16 && b <= 31) {
    return false;
  }
  if (a == 192 && b == 168) {
    return false;
  }
  if (a == 198 && (b == 18 || b == 19)) {
    return false;
  }
  return true;
}

bool peer_has_public_ip(const td::JsonObject &object) {
  auto field = find_field(object, "addresses");
  if (field == nullptr || field->type() != td::JsonValue::Type::Array) {
    return false;
  }
  for (const auto &address : field->get_array()) {
    if (address.type() != td::JsonValue::Type::Object) {
      continue;
    }
    auto ip = address.get_object().get_optional_string_field("ip").move_as_ok();
    if (!ip.empty() && public_ipv4(ip)) {
      return true;
    }
  }
  return false;
}

std::vector<std::string> parse_recent_neighbours(const td::JsonObject &object, std::size_t raw_limit) {
  std::vector<std::string> result;
  auto field = find_field(object, "recent_neighbours");
  if (field == nullptr || field->type() != td::JsonValue::Type::Array) {
    return result;
  }
  std::size_t raw_count = 0;
  for (const auto &value : field->get_array()) {
    if (raw_limit != 0 && raw_count >= raw_limit) {
      break;
    }
    ++raw_count;
    if (value.type() == td::JsonValue::Type::String) {
      result.push_back(value.get_string().str());
    }
  }
  return result;
}

bool fec_observed(const td::JsonObject &object) {
  auto field = find_field(object, "fec");
  return field != nullptr && field->type() == td::JsonValue::Type::Object;
}

void parse_geo(const td::JsonObject &object, GraphNode &node) {
  auto field = find_field(object, "geo");
  if (field == nullptr || field->type() != td::JsonValue::Type::Object) {
    return;
  }
  const auto &geo = field->get_object();
  auto lat = geo.get_optional_double_field("lat", 0.0).move_as_ok();
  auto lon = geo.get_optional_double_field("lon", 0.0).move_as_ok();
  if (std::isfinite(lat) && std::isfinite(lon)) {
    node.has_geo = true;
    node.lat = lat;
    node.lon = lon;
  }
}

void parse_geo_latency_model(const td::JsonObject &object, Graph &graph) {
  const td::JsonValue *field = find_field(object, "geo_latency_model");
  if (field == nullptr) {
    field = find_field(object, "geoLatencyModel");
  }
  if (field == nullptr || field->type() != td::JsonValue::Type::Object) {
    return;
  }
  const auto &model = field->get_object();
  auto alpha_ms = model.get_optional_double_field("alpha_ms", graph.geo_alpha_ms).move_as_ok();
  alpha_ms = model.get_optional_double_field("alphaMs", alpha_ms).move_as_ok();
  auto beta_ms_per_km = model.get_optional_double_field("beta_ms_per_km", graph.geo_beta_ms_per_km).move_as_ok();
  beta_ms_per_km = model.get_optional_double_field("betaMsPerKm", beta_ms_per_km).move_as_ok();
  if (std::isfinite(alpha_ms)) {
    graph.geo_alpha_ms = alpha_ms;
  }
  if (std::isfinite(beta_ms_per_km)) {
    graph.geo_beta_ms_per_km = beta_ms_per_km;
  }
}

}  // namespace

td::Result<Graph> load_graph(const std::string &graph_path, std::size_t limit, bool rebroadcasting_only,
                             std::size_t recent_neighbour_limit) {
  TRY_RESULT(data, td::read_file(graph_path));
  TRY_RESULT(json, td::json_decode(data.as_slice()));
  const td::JsonArray *peers = nullptr;
  const td::JsonObject *root_object = nullptr;
  if (json.type() == td::JsonValue::Type::Array) {
    peers = &json.get_array();
  } else if (json.type() == td::JsonValue::Type::Object) {
    root_object = &json.get_object();
    auto field = find_field(*root_object, "peers");
    if (field != nullptr && field->type() == td::JsonValue::Type::Array) {
      peers = &field->get_array();
    }
  }
  if (peers == nullptr) {
    return td::Status::Error("graph JSON must be an array or an object with a peers array");
  }

  std::vector<std::vector<std::string>> pending_neighbours;
  Graph graph;
  if (root_object != nullptr) {
    parse_geo_latency_model(*root_object, graph);
  }
  graph.nodes.reserve(limit == 0 ? peers->size() : limit);
  std::map<std::string, std::size_t> id_to_index;
  for (std::size_t i = 0; i < peers->size(); ++i) {
    const auto &value = (*peers)[i];
    if (value.type() != td::JsonValue::Type::Object) {
      return td::Status::Error("peer entry must be an object");
    }
    const auto &object = value.get_object();
    auto has_fec = fec_observed(object);
    auto has_public_ip = peer_has_public_ip(object);
    if (rebroadcasting_only && (!has_fec || !has_public_ip)) {
      continue;
    }
    if (limit != 0 && graph.nodes.size() >= limit) {
      break;
    }
    auto id = optional_node_id(object);
    if (id.empty()) {
      return td::Status::Error(PSTRING() << "peer #" << i << " has no adnl_id/node/id");
    }
    GraphNode node;
    node.graph_id = id;
    node.is_validator = object.get_optional_bool_field("is_validator", false).move_as_ok();
    node.fec_observed = has_fec;
    node.public_ip = has_public_ip;
    parse_geo(object, node);
    auto rtt_seconds = object.get_optional_double_field("median_success_latency", 0.0).move_as_ok();
    if (rtt_seconds > 0.0) {
      node.rtt_ms = rtt_seconds * 1000.0;
    }
    pending_neighbours.push_back(parse_recent_neighbours(object, recent_neighbour_limit));
    id_to_index.emplace(node.graph_id, graph.nodes.size());
    if (node.is_validator) {
      ++graph.validator_count;
    }
    graph.nodes.push_back(std::move(node));
  }

  for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
    std::set<std::size_t> unique;
    for (const auto &neighbour_id : pending_neighbours[i]) {
      auto it = id_to_index.find(neighbour_id);
      if (it == id_to_index.end() || it->second == i) {
        continue;
      }
      if (unique.insert(it->second).second) {
        graph.nodes[i].recent_neighbours.push_back(it->second);
      }
    }
  }

  return graph;
}

Graph make_smoke_graph(std::size_t nodes_count) {
  Graph graph;
  graph.validator_count = std::max<std::size_t>(1, nodes_count / 3);
  graph.nodes.resize(nodes_count);
  auto add_neighbour = [&](std::size_t node, std::size_t peer) {
    if (node == peer || node >= graph.nodes.size() || peer >= graph.nodes.size()) {
      return;
    }
    auto &neighbours = graph.nodes[node].recent_neighbours;
    if (std::find(neighbours.begin(), neighbours.end(), peer) == neighbours.end()) {
      neighbours.push_back(peer);
    }
  };
  for (std::size_t i = 0; i < nodes_count; ++i) {
    graph.nodes[i].graph_id = PSTRING() << "smoke-" << i;
    graph.nodes[i].is_validator = i < graph.validator_count;
    graph.nodes[i].fec_observed = true;
    graph.nodes[i].public_ip = true;
    graph.nodes[i].has_geo = true;
    graph.nodes[i].lat = static_cast<double>((i * 17) % 160) - 80.0;
    graph.nodes[i].lon = static_cast<double>((i * 31) % 360) - 180.0;
    graph.nodes[i].rtt_ms = 40.0 + static_cast<double>((i * 7) % 30);
  }

  for (std::size_t validator = 0; validator < graph.validator_count; ++validator) {
    for (std::size_t peer = graph.validator_count + validator; peer < nodes_count; peer += graph.validator_count) {
      add_neighbour(validator, peer);
    }
    add_neighbour(validator, (validator + 1) % graph.validator_count);
  }
  for (std::size_t i = graph.validator_count; i < nodes_count; ++i) {
    auto validator = (i - graph.validator_count) % graph.validator_count;
    add_neighbour(i, validator);
    for (std::size_t peer = graph.validator_count + validator; peer < nodes_count; peer += graph.validator_count) {
      add_neighbour(i, peer);
    }
  }
  return graph;
}

std::vector<std::vector<std::size_t>> build_out_neighbours(const Graph &graph, std::size_t max_neighbours) {
  std::vector<std::vector<std::size_t>> result(graph.nodes.size());
  for (std::size_t node_index = 0; node_index < graph.nodes.size(); ++node_index) {
    std::set<std::size_t> seen;
    for (auto peer : graph.nodes[node_index].recent_neighbours) {
      if (result[node_index].size() >= max_neighbours) {
        break;
      }
      if (peer != node_index && seen.insert(peer).second) {
        result[node_index].push_back(peer);
      }
    }
  }
  return result;
}

std::vector<std::vector<std::size_t>> build_known_peers_by_node(
    const std::vector<std::vector<std::size_t>> &out_neighbours) {
  std::vector<std::vector<std::size_t>> result(out_neighbours.size());
  std::vector<std::set<std::size_t>> seen(out_neighbours.size());
  auto add_peer = [&](std::size_t node_index, std::size_t peer) {
    if (peer != node_index && seen[node_index].insert(peer).second) {
      result[node_index].push_back(peer);
    }
  };

  for (std::size_t node_index = 0; node_index < out_neighbours.size(); ++node_index) {
    for (auto peer : out_neighbours[node_index]) {
      add_peer(node_index, peer);
      if (peer < out_neighbours.size()) {
        add_peer(peer, node_index);
      }
    }
  }
  return result;
}

std::vector<std::size_t> build_known_peers(const std::vector<std::vector<std::size_t>> &out_neighbours,
                                           std::size_t node_index) {
  std::vector<std::size_t> result;
  std::set<std::size_t> seen;
  auto add_peer = [&](std::size_t peer) {
    if (peer != node_index && seen.insert(peer).second) {
      result.push_back(peer);
    }
  };
  for (auto peer : out_neighbours[node_index]) {
    add_peer(peer);
  }
  for (std::size_t peer = 0; peer < out_neighbours.size(); ++peer) {
    if (std::find(out_neighbours[peer].begin(), out_neighbours[peer].end(), node_index) != out_neighbours[peer].end()) {
      add_peer(peer);
    }
  }
  return result;
}

}  // namespace ton::overlay::plumtree_sim
