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
#include <string>
#include <vector>

#include "td/utils/Status.h"

namespace ton::overlay::plumtree_sim {

struct GraphNode {
  std::string graph_id;
  bool is_validator = false;
  bool fec_observed = false;
  bool public_ip = false;
  bool has_geo = false;
  double lat = 0.0;
  double lon = 0.0;
  double rtt_ms = 0.0;
  std::vector<std::size_t> recent_neighbours;
};

struct Graph {
  std::vector<GraphNode> nodes;
  std::size_t validator_count = 0;
  double geo_alpha_ms = 3.554;
  double geo_beta_ms_per_km = 0.008963;
};

td::Result<Graph> load_graph(const std::string &graph_path, std::size_t limit, bool rebroadcasting_only,
                             std::size_t recent_neighbour_limit);
Graph make_smoke_graph(std::size_t nodes_count);

std::vector<std::vector<std::size_t>> build_out_neighbours(const Graph &graph, std::size_t max_neighbours);
std::vector<std::vector<std::size_t>> build_known_peers_by_node(
    const std::vector<std::vector<std::size_t>> &out_neighbours);
std::vector<std::size_t> build_known_peers(const std::vector<std::vector<std::size_t>> &out_neighbours,
                                           std::size_t node_index);

}  // namespace ton::overlay::plumtree_sim
