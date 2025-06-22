/*
    This file is part of TON Blockchain Library.

    TON Blockchain Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    TON Blockchain Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with TON Blockchain Library.  If not, see <http://www.gnu.org/licenses/>.

    Copyright 2017-2020 Telegram Systems LLP
*/
#include "boc-compression.h"

#include <algorithm>
#include "vm/boc.h"
#include "vm/boc-writers.h"
#include "vm/cells.h"
#include "vm/cellslice.h"
#include "td/utils/Slice-decl.h"
#include "td/utils/lz4.h"

namespace vm {

td::Result<td::BufferSlice> boc_compress_baseline_lz4(const std::vector<td::Ref<vm::Cell>>& boc_roots) {
  TRY_RESULT(data, vm::std_boc_serialize_multi(std::move(boc_roots), 2));
  td::BufferSlice compressed = td::lz4_compress(data);

  // Add decompressed size at the beginning
  td::BufferSlice compressed_with_size(compressed.size() + 4);
  td::BitSliceWrite(compressed_with_size.as_slice().ubegin(), 32).bits().store_uint(data.size(), 32);
  memcpy(compressed_with_size.data() + 4, compressed.data(), compressed.size());

  return compressed_with_size;
}

td::Result<std::vector<td::Ref<vm::Cell>>> boc_decompress_baseline_lz4(td::Slice compressed) {
  // Read decompressed size
  int decompressed_size = td::BitSlice(compressed.ubegin(), 32).bits().get_uint(32);
  compressed.remove_prefix(4);

  TRY_RESULT(decompressed, td::lz4_decompress(compressed, decompressed_size));
  TRY_RESULT(roots, vm::std_boc_deserialize_multi(decompressed));
  return roots;
}

td::Result<td::BufferSlice> boc_compress_improved_structure_lz4(const std::vector<td::Ref<vm::Cell>>& boc_roots) {
  // Initialize data structures for graph representation
  td::HashMap<vm::Cell::Hash, int> cell_hashes;
  std::vector<std::vector<int>> boc_graph;
  std::vector<td::BitSlice> cell_data;
  std::vector<int> cell_type;
  std::vector<int> prunned_branch_level;
  std::vector<int> root_indexes;
  int total_size_estimate = 0;

  // Build graph representation using recursive lambda
  const auto build_graph = [&](auto&& self, td::Ref<vm::Cell> cell) -> int {
    auto cell_hash = cell->get_hash();
    auto it = cell_hashes.find(cell_hash);
    if (it != cell_hashes.end()) {
      return it->second;
    }

    int current_cell_id = boc_graph.size();
    cell_hashes.emplace(cell_hash, current_cell_id);

    bool is_special = false;
    vm::CellSlice cell_slice = vm::load_cell_slice_special(cell, is_special);
    td::BitSlice cell_bitslice = cell_slice.as_bitslice();

    // Initialize new cell in graph
    boc_graph.emplace_back();
    cell_type.emplace_back(int(cell_slice.special_type()));
    prunned_branch_level.push_back(0);

    // Process special cell of type PrunnedBranch
    if (cell_slice.special_type() == vm::CellTraits::SpecialType::PrunnedBranch) {
      cell_data.emplace_back(cell_bitslice.subslice(16, cell_bitslice.size() - 16));
      prunned_branch_level.back() = cell_slice.data()[1];
    } else {
      cell_data.emplace_back(cell_bitslice);
    }
    total_size_estimate += cell_bitslice.size();

    // Process cell references
    for (int i = 0; i < cell_slice.size_refs(); ++i) {
      int child_id = self(self, cell_slice.prefetch_ref(i));
      boc_graph[current_cell_id].push_back(child_id);
    }

    return current_cell_id;
  };

  // Build the graph starting from roots
  for (auto root : boc_roots) {
    root_indexes.push_back(boc_graph.size());
    build_graph(build_graph, root);
  }

  // Calculate graph properties
  const int node_count = boc_graph.size();
  std::vector<std::vector<int>> reverse_graph(node_count);
  int edge_count = 0;

  // Build reverse graph
  for (int i = 0; i < node_count; ++i) {
    for (int child : boc_graph[i]) {
      ++edge_count;
      reverse_graph[child].push_back(i);
    }
  }

  // Process cell data sizes
  std::vector<int> is_data_small(node_count, 0);
  for (int i = 0; i < node_count; ++i) {
    if (cell_type[i] != 1) {
      is_data_small[i] = cell_data[i].size() < 128;
    }
  }

  // Perform topological sort
  std::vector<int> topo_order, rank(node_count);
  const auto topological_sort = [&]() {
    std::vector<std::tuple<int, int, int>> queue;
    queue.reserve(node_count);
    std::vector<int> in_degree(node_count);

    // Calculate in-degrees and initialize queue
    for (int i = 0; i < node_count; ++i) {
      in_degree[i] = boc_graph[i].size();
      if (in_degree[i] == 0) {
        queue.emplace_back(cell_type[i] == 0, -int(cell_data[i].size()), -i);
      }
    }

    std::sort(queue.begin(), queue.end());

    // Process queue
    while (!queue.empty()) {
      int node = -std::get<2>(queue.back());
      queue.pop_back();
      topo_order.push_back(node);

      for (int parent : reverse_graph[node]) {
        if (--in_degree[parent] == 0) {
          queue.emplace_back(0, 0, -parent);
        }
      }
    }
    std::reverse(topo_order.begin(), topo_order.end());
  };
  topological_sort();

  // Calculate index of vertices in topsort
  for (int i = 0; i < node_count; ++i) {
    rank[topo_order[i]] = i;
  }

  // Build compressed representation
  td::BitString result;
  total_size_estimate += (node_count * 10 * 8);
  result.reserve_bits(total_size_estimate);
  result.reserve_bitslice(16).bits().store_uint(root_indexes.size(), 16);
  for (int root_ind : root_indexes) {
    result.reserve_bitslice(16).bits().store_uint(rank[root_ind], 16);
  }

  // Store node count
  result.reserve_bitslice(16).bits().store_uint(node_count, 16);

  // Store cell types and sizes
  for (int i = 0; i < node_count; ++i) {
    int node = topo_order[i];
    int currrent_cell_type = bool(cell_type[node]) + prunned_branch_level[node];
    result.reserve_bitslice(4).bits().store_uint(currrent_cell_type, 4);
    result.reserve_bitslice(4).bits().store_uint(boc_graph[node].size(), 4);

    if (cell_type[node] != 1) {
      if (is_data_small[node]) {
        result.reserve_bitslice(1).bits().store_uint(1, 1);
        result.reserve_bitslice(7).bits().store_uint(cell_data[node].size(), 7);
      } else {
        result.reserve_bitslice(1).bits().store_uint(0, 1);
        result.reserve_bitslice(7).bits().store_uint(1 + cell_data[node].size() / 8, 7);
      }
    }
  }

  // Store edge information
  auto edge_bits = result.reserve_bitslice(edge_count).bits();
  for (int i = 0; i < node_count; ++i) {
    int node = topo_order[i];
    for (int child : boc_graph[node]) {
      edge_bits.store_uint(rank[child] == i + 1, 1);
      ++edge_bits;
    }
  }

  // Store cell data
  for (int node : topo_order) {
    if (cell_type[node] != 1 && !is_data_small[node]) {
      continue;
    }
    result.append(cell_data[node].subslice(0, cell_data[node].size() % 8));
  }

  // Store BOC graph
  for (int i = 0; i < node_count; ++i) {
    int node = topo_order[i];
    if (node_count - i - 3 <= 0)
      continue;

    for (int j = 0; j < boc_graph[node].size(); ++j) {
      if (rank[boc_graph[node][j]] <= i + 1)
        continue;

      int delta = rank[boc_graph[node][j]] - i - 2;
      size_t required_bits = 1 + (31 ^ __builtin_clz(node_count - i - 3));

      if (required_bits < 8 - (result.size() + 1) % 8 + 1) {
        result.reserve_bitslice(required_bits).bits().store_uint(delta, required_bits);
      } else if (delta < (1 << (8 - (result.size() + 1) % 8))) {
        size_t available_bits = 8 - (result.size() + 1) % 8;
        result.reserve_bitslice(1).bits().store_uint(1, 1);
        result.reserve_bitslice(available_bits).bits().store_uint(delta, available_bits);
      } else {
        result.reserve_bitslice(1).bits().store_uint(0, 1);
        result.reserve_bitslice(required_bits).bits().store_uint(delta, required_bits);
      }
    }
  }

  // Pad result to byte boundary
  while (result.size() % 8) {
    result.reserve_bitslice(1).bits().store_uint(0, 1);
  }

  // Store remaining cell data
  for (int node : topo_order) {
    if (cell_type[node] == 1 || is_data_small[node]) {
      int prefix_size = cell_data[node].size() % 8;
      result.append(cell_data[node].subslice(prefix_size, cell_data[node].size() - prefix_size));
    } else {
      int data_size = cell_data[node].size() + 1;
      int padding = (8 - data_size % 8) % 8;

      if (padding) {
        result.reserve_bitslice(padding).bits().store_uint(0, padding);
      }
      result.reserve_bitslice(1).bits().store_uint(1, 1);
      result.append(cell_data[node]);
    }
  }

  // Final padding
  while (result.size() % 8) {
    result.reserve_bitslice(1).bits().store_uint(0, 1);
  }

  // Create final compressed buffer
  td::BufferSlice serialized((const char*)result.bits().get_byte_ptr(), result.size() / 8);

  td::BufferSlice compressed = td::lz4_compress(serialized);

  // Add decompressed size at the beginning
  td::BufferSlice compressed_with_size(compressed.size() + 4);
  td::BitSliceWrite(compressed_with_size.as_slice().ubegin(), 32).bits().store_uint(serialized.size(), 32);
  memcpy(compressed_with_size.data() + 4, compressed.data(), compressed.size());

  return compressed_with_size;
}

td::Result<std::vector<td::Ref<vm::Cell>>> boc_decompress_improved_structure_lz4(td::Slice compressed) {
  // Read decompressed size
  int decompressed_size = td::BitSlice(compressed.ubegin(), 32).bits().get_uint(32);
  compressed.remove_prefix(4);

  // Decompress LZ4 data with 2MB max size
  td::BufferSlice serialized = td::lz4_decompress(compressed, decompressed_size).move_as_ok();

  // Initialize bit reader
  td::BitSlice bit_reader(serialized.as_slice().ubegin(), serialized.as_slice().size() * 8);
  int orig_size = bit_reader.size();

  int root_count = bit_reader.bits().get_uint(16);
  bit_reader.advance(16);
  std::vector<int> root_indexes(root_count);
  for (int i = 0; i < root_count; ++i) {
    root_indexes[i] = bit_reader.bits().get_uint(16);
    bit_reader.advance(16);
  }

  // Read number of nodes from header
  int node_count = bit_reader.bits().get_uint(16);
  bit_reader.advance(16);

  // Initialize data structures
  std::vector<int> cell_data_length(node_count), is_data_small(node_count);
  std::vector<int> is_special(node_count), cell_refs_cnt(node_count);
  std::vector<int> prunned_branch_level(node_count, 0);

  std::vector<vm::CellBuilder> cell_builders(node_count);
  std::vector<std::vector<int>> boc_graph(node_count);

  // Read cell metadata
  for (int i = 0; i < node_count; ++i) {
    int cell_type = bit_reader.bits().get_uint(4);
    is_special[i] = bool(cell_type);
    if (is_special[i]) {
      prunned_branch_level[i] = cell_type - 1;
    }
    bit_reader.advance(4);
    cell_refs_cnt[i] = bit_reader.bits().get_uint(4);
    bit_reader.advance(4);

    if (prunned_branch_level[i]) {
      int coef = prunned_branch_level[i] == 3 ? 2 : 1;
      cell_data_length[i] = (256 + 16) * coef;
    } else {
      is_data_small[i] = bit_reader.bits().get_uint(1);
      bit_reader.advance(1);
      cell_data_length[i] = bit_reader.bits().get_uint(7);
      bit_reader.advance(7);

      if (!is_data_small[i]) {
        cell_data_length[i] *= 8;
        if (!cell_data_length[i]) {
          cell_data_length[i] += 1024;
        }
      }
    }
    boc_graph[i].resize(cell_refs_cnt[i]);
  }

  // Read direct edge connections
  for (int i = 0; i < node_count; ++i) {
    for (int j = 0; j < cell_refs_cnt[i]; ++j) {
      if (bit_reader.bits().get_uint(1)) {
        boc_graph[i][j] = i + 1;
      }
      bit_reader.advance(1);
    }
  }

  // Read initial cell data
  for (int i = 0; i < node_count; ++i) {
    if (prunned_branch_level[i]) {
      cell_builders[i].store_long((1 << 8) + prunned_branch_level[i], 16);
    }

    int remainder_bits = cell_data_length[i] % 8;
    cell_builders[i].store_bits(bit_reader.subslice(0, remainder_bits));
    bit_reader.advance(remainder_bits);
    cell_data_length[i] -= remainder_bits;
  }

  // Decode remaining edge connections
  for (int i = 0; i < node_count; ++i) {
    if (node_count - i - 3 <= 0) {
      for (int j = 0; j < cell_refs_cnt[i]; ++j) {
        if (!boc_graph[i][j]) {
          boc_graph[i][j] = i + 2;
        }
      }
      continue;
    }

    for (int j = 0; j < cell_refs_cnt[i]; ++j) {
      if (!boc_graph[i][j]) {
        int pref_size = (orig_size - bit_reader.size());
        int required_bits = 1 + (31 ^ __builtin_clz(node_count - i - 3));

        if (required_bits < 8 - (pref_size + 1) % 8 + 1) {
          boc_graph[i][j] = bit_reader.bits().get_uint(required_bits) + i + 2;
          bit_reader.advance(required_bits);
        } else if (bit_reader.bits().get_uint(1)) {
          bit_reader.advance(1);
          pref_size = (orig_size - bit_reader.size());
          int available_bits = 8 - pref_size % 8;
          boc_graph[i][j] = bit_reader.bits().get_uint(available_bits) + i + 2;
          bit_reader.advance(available_bits);
        } else {
          bit_reader.advance(1);
          boc_graph[i][j] = bit_reader.bits().get_uint(required_bits) + i + 2;
          bit_reader.advance(required_bits);
        }
      }
    }
  }

  // Align to byte boundary
  while ((orig_size - bit_reader.size()) % 8) {
    bit_reader.advance(1);
  }

  // Read remaining cell data
  for (int i = 0; i < node_count; ++i) {
    int padding_bits = 0;
    if (!prunned_branch_level[i] && !is_data_small[i]) {
      while (bit_reader.bits()[0] == 0) {
        ++padding_bits;
        bit_reader.advance(1);
      }
      bit_reader.advance(1);
      ++padding_bits;
    }
    cell_builders[i].store_bits(bit_reader.subslice(0, cell_data_length[i] - padding_bits));
    bit_reader.advance(cell_data_length[i] - padding_bits);
  }

  // Build cell tree
  std::vector<td::Ref<vm::Cell>> nodes(node_count);
  for (int i = node_count - 1; i >= 0; --i) {
    for (int child : boc_graph[i]) {
      cell_builders[i].store_ref(nodes[child]);
    }
    nodes[i] = cell_builders[i].finalize(is_special[i]);
  }

  std::vector<td::Ref<vm::Cell>> root_nodes;
  for (int index : root_indexes) {
    root_nodes.push_back(nodes[index]);
  }

  return root_nodes;
}

td::Result<td::BufferSlice> boc_compress(const std::vector<td::Ref<vm::Cell>>& boc_roots, CompressionAlgorithm algo) {
  td::BufferSlice compressed;
  if (algo == CompressionAlgorithm::BaselineLZ4) {
    TRY_RESULT_ASSIGN(compressed, boc_compress_baseline_lz4(boc_roots));
  } else if (algo == CompressionAlgorithm::ImprovedStructureLZ4) {
    TRY_RESULT_ASSIGN(compressed, boc_compress_improved_structure_lz4(boc_roots));
  } else {
      return td::Status::Error("Unknown compression algorithm");
  }
  td::BufferSlice compressed_with_algo(compressed.size() + 1);
  compressed_with_algo.data()[0] = int(algo);
  memcpy(compressed_with_algo.data() + 1, compressed.data(), compressed.size());
  return compressed_with_algo;
}

td::Result<std::vector<td::Ref<vm::Cell>>> boc_decompress(td::Slice compressed) {
  int algo = int(compressed[0]);
  compressed.remove_prefix(1);
  switch (algo) {
    case int(CompressionAlgorithm::BaselineLZ4):
      return boc_decompress_baseline_lz4(compressed);
    case int(CompressionAlgorithm::ImprovedStructureLZ4):
      return boc_decompress_improved_structure_lz4(compressed);
  }
  return td::Status::Error("Unknown compression algorithm");
}

}  // namespace vm
