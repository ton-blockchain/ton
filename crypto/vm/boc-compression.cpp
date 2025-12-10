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
#include <algorithm>
#include <bitset>
#include <set>

#include "common/bitstring.h"
#include "td/utils/Slice-decl.h"
#include "td/utils/lz4.h"
#include "vm/boc-writers.h"
#include "vm/boc.h"
#include "vm/cells.h"
#include "vm/cellslice.h"

#include "boc-compression.h"
#include "common/refint.h"
#include "crypto/block/block-auto.h"
#include "crypto/block/block-parse.h"

namespace vm {

td::Result<td::BufferSlice> boc_compress_baseline_lz4(const std::vector<td::Ref<vm::Cell>>& boc_roots) {
  TRY_RESULT(data, vm::std_boc_serialize_multi(std::move(boc_roots), 2));
  td::BufferSlice compressed = td::lz4_compress(data);

  // Add decompressed size at the beginning
  td::BufferSlice compressed_with_size(compressed.size() + kDecompressedSizeBytes);
  auto size_slice = td::BitSliceWrite(compressed_with_size.as_slice().ubegin(), kDecompressedSizeBytes * 8);
  size_slice.bits().store_uint(data.size(), kDecompressedSizeBytes * 8);
  memcpy(compressed_with_size.data() + kDecompressedSizeBytes, compressed.data(), compressed.size());

  return compressed_with_size;
}

td::Result<std::vector<td::Ref<vm::Cell>>> boc_decompress_baseline_lz4(td::Slice compressed, int max_decompressed_size) {
  // Check minimum input size for decompressed size header
  if (compressed.size() < kDecompressedSizeBytes) {
    return td::Status::Error("BOC decompression failed: input too small for header");
  }

  // Read decompressed size
  constexpr size_t kSizeBits = kDecompressedSizeBytes * 8;
  int decompressed_size = td::BitSlice(compressed.ubegin(), kSizeBits).bits().get_uint(kSizeBits);
  compressed.remove_prefix(kDecompressedSizeBytes);
  if (decompressed_size <= 0 || decompressed_size > max_decompressed_size) {
    return td::Status::Error("BOC decompression failed: invalid decompressed size");
  }

  TRY_RESULT(decompressed, td::lz4_decompress(compressed, decompressed_size));
  TRY_RESULT(roots, vm::std_boc_deserialize_multi(decompressed));
  return roots;
}

inline void append_uint(td::BitString& bs, unsigned val, unsigned n) {
  bs.reserve_bitslice(n).bits().store_uint(val, n);
}

inline td::Result<unsigned> read_uint(td::BitSlice& bs, int bits) {
  // Check if there enough bits available
  if (bs.size() < bits) {
    return td::Status::Error("BOC decompression failed: not enough bits to read");
  }
  unsigned result = bs.bits().get_uint(bits);
  bs.advance(bits);
  return result;
}

// Decode DepthBalanceInfo and extract grams using TLB methods
td::RefInt256 extract_balance_from_depth_balance_info(vm::CellSlice& cs) {
  // Check hashmap label is empty ('00')
  if (cs.size() < 2 || cs.fetch_ulong(2) != 0) {
    return td::RefInt256{};
  }

  int split_depth;
  Ref<vm::CellSlice> balance_cs_ref;
  if (!block::gen::t_DepthBalanceInfo.unpack_depth_balance(cs, split_depth, balance_cs_ref)) {
    return td::RefInt256{};
  }
  if (split_depth != 0) {
    return td::RefInt256{};
  }
  if (!cs.empty()) {
    return td::RefInt256{};
  }
  auto balance_cs = balance_cs_ref.write();
  auto res = block::tlb::t_Grams.as_integer_skip(balance_cs);
  if (balance_cs.size() != 1 || balance_cs.fetch_ulong(1) != 0) {
    return td::RefInt256{};
  }
  return res;
}

// Process ShardAccounts vertex and compute balance difference (right - left)
td::RefInt256 process_shard_accounts_vertex(vm::CellSlice& cs_left, vm::CellSlice& cs_right) {
  auto balance_left = extract_balance_from_depth_balance_info(cs_left);
  auto balance_right = extract_balance_from_depth_balance_info(cs_right);
  if (balance_left.not_null() && balance_right.not_null()) {
    td::RefInt256 diff = balance_right;
    diff -= balance_left;
    return diff;
  }
  return td::RefInt256{};
}

td::Result<td::BufferSlice> boc_compress_improved_structure_lz4(
  const std::vector<td::Ref<vm::Cell>>& boc_roots, 
  bool compress_merkle_update,
  td::Ref<vm::Cell> state
) {
  const bool kMURemoveSubtreeSums = true;
  // Input validation
  if (boc_roots.empty()) {
    return td::Status::Error("No root cells were provided for serialization");
  }
  for (const auto& root : boc_roots) {
    if (root.is_null()) {
      return td::Status::Error("Cannot serialize a null cell reference into a bag of cells");
    }
  }

  // Initialize data structures for graph representation
  td::HashMap<vm::Cell::Hash, size_t> cell_hashes;
  std::vector<std::array<size_t, 4>> boc_graph;
  std::vector<size_t> refs_cnt;
  std::vector<td::BitSlice> cell_data;
  std::vector<size_t> cell_type;
  std::vector<size_t> prunned_branch_level;
  std::vector<size_t> root_indexes;
  size_t total_size_estimate = 0;

  // Build graph representation using recursive lambda
  const auto build_graph = [&](auto&& self,
                               td::Ref<vm::Cell> cell,
                               bool under_mu_left = false,
                               bool under_mu_right = false,
                               td::Ref<vm::Cell> left_cell = td::Ref<vm::Cell>(),
                               td::RefInt256* sum_diff_out = nullptr,
                               td::Ref<vm::Cell> state_cell = td::Ref<vm::Cell>()) -> td::Result<size_t> {
    if (cell.is_null()) {
      return td::Status::Error("Error while importing a cell during serialization: cell is null");
    }

    auto cell_hash = cell->get_hash();
    auto it = cell_hashes.find(cell_hash);
    if (it != cell_hashes.end()) {
      return it->second;
    }

    size_t current_cell_id = boc_graph.size();
    cell_hashes.emplace(cell_hash, current_cell_id);

    bool is_special = false;
    vm::CellSlice cell_slice = vm::load_cell_slice_special(cell, is_special);
    if (!cell_slice.is_valid()) {
      return td::Status::Error("Invalid loaded cell data");
    }
    td::BitSlice cell_bitslice = cell_slice.as_bitslice();

    // Initialize new cell in graph
    boc_graph.emplace_back();
    refs_cnt.emplace_back(cell_slice.size_refs());
    cell_type.emplace_back(size_t(cell_slice.special_type()));
    prunned_branch_level.push_back(0);

    DCHECK(cell_slice.size_refs() <= 4);

    // Process special cell of type PrunnedBranch
    if (cell_slice.special_type() == vm::CellTraits::SpecialType::PrunnedBranch) {
      DCHECK(cell_slice.size() >= 16);
      cell_data.emplace_back(cell_bitslice.subslice(16, cell_bitslice.size() - 16));
      prunned_branch_level.back() = cell_slice.data()[1];
    } else {
      cell_data.emplace_back(cell_bitslice);
    }

    if (compress_merkle_update && under_mu_left) {
      cell_data.back() = td::BitSlice();
    }
    total_size_estimate += cell_bitslice.size();

    // Process cell references
    if (kMURemoveSubtreeSums && cell_slice.special_type() == vm::CellTraits::SpecialType::MerkleUpdate) {
      // Left branch: traverse normally
      TRY_RESULT(child_left_id, self(self, cell_slice.prefetch_ref(0), true));
      boc_graph[current_cell_id][0] = child_left_id;
      // Right branch: traverse paired with left and compute diffs inline
      TRY_RESULT(child_right_id, self(self, cell_slice.prefetch_ref(1), false, true, cell_slice.prefetch_ref(0)));
      boc_graph[current_cell_id][1] = child_right_id;
    } else if (under_mu_right && left_cell.not_null()) {
      // Inline computation for RIGHT subtree nodes under MerkleUpdate
      vm::CellSlice cs_left(NoVm(), left_cell);
      td::RefInt256 sum_child_diff = td::make_refint(0);
      // Recurse children first
      for (int i = 0; i < cell_slice.size_refs(); ++i) {
        TRY_RESULT(child_id, self(self, cell_slice.prefetch_ref(i), false, true, cs_left.prefetch_ref(i), &sum_child_diff));
        boc_graph[current_cell_id][i] = child_id;
      }
    
      // Compute this vertex diff and check skippable condition
      td::RefInt256 vertex_diff = process_shard_accounts_vertex(cs_left, cell_slice);
      if (!is_special && vertex_diff.not_null() && sum_child_diff.not_null() && cmp(sum_child_diff, vertex_diff) == 0) {
        cell_data[current_cell_id] = td::BitSlice();
        prunned_branch_level[current_cell_id] = 9;
      }
      if (sum_diff_out && vertex_diff.not_null()) {
        *sum_diff_out += vertex_diff;
      }
    } else {
      for (int i = 0; i < cell_slice.size_refs(); ++i) {
        TRY_RESULT(child_id, self(self, cell_slice.prefetch_ref(i), under_mu_left, under_mu_right));
        boc_graph[current_cell_id][i] = child_id;
      }
    }

    return current_cell_id;
  };

  // Build the graph starting from roots
  for (auto root : boc_roots) {
    TRY_RESULT(root_cell_id, build_graph(build_graph, root));
    root_indexes.push_back(root_cell_id);
  }

  // Check graph properties
  const size_t node_count = boc_graph.size();
  std::vector<std::vector<size_t>> reverse_graph(node_count);
  size_t edge_count = 0;

  // Build reverse graph
  for (int i = 0; i < node_count; ++i) {
    for (size_t child_index = 0; child_index < refs_cnt[i]; ++child_index) {
      size_t child = boc_graph[i][child_index];
      ++edge_count;
      reverse_graph[child].push_back(i);
    }
  }

  // Process cell data sizes
  std::vector<size_t> is_data_small(node_count, 0);
  for (int i = 0; i < node_count; ++i) {
    if (cell_type[i] != 1) {
      is_data_small[i] = cell_data[i].size() < 128;
    }
  }

  // Perform topological sort
  std::vector<size_t> topo_order, rank(node_count);
  const auto topological_sort = [&]() -> td::Status {
    std::vector<std::tuple<int, int, int>> queue;
    queue.reserve(node_count);
    std::vector<size_t> in_degree(node_count);

    // Calculate in-degrees and initialize queue
    for (int i = 0; i < node_count; ++i) {
      in_degree[i] = refs_cnt[i];
      if (in_degree[i] == 0) {
        queue.emplace_back(cell_type[i] == 0, -int(cell_data[i].size()), -i);
      }
    }

    if (queue.empty()) {
      return td::Status::Error("Cycle detected in cell references");
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

    if (topo_order.size() != node_count) {
      return td::Status::Error("Invalid graph structure");
    }

    std::reverse(topo_order.begin(), topo_order.end());
    return td::Status::OK();
  };

  TRY_STATUS(topological_sort());

  // Calculate index of vertices in topsort
  for (int i = 0; i < node_count; ++i) {
    rank[topo_order[i]] = i;
  }

  // Build compressed representation
  td::BitString result;
  total_size_estimate += (node_count * 10 * 8);
  result.reserve_bits(total_size_estimate);

  // Store roots information
  append_uint(result, root_indexes.size(), 32);
  for (int root_ind : root_indexes) {
    append_uint(result, rank[root_ind], 32);
  }

  // Store node count
  append_uint(result, node_count, 32);

  // Store cell types and sizes
  for (int i = 0; i < node_count; ++i) {
    size_t node = topo_order[i];
    size_t current_cell_type = bool(cell_type[node]) + prunned_branch_level[node];
    append_uint(result, current_cell_type, 4);
    int current_refs_cnt = refs_cnt[node];
    if (cell_type[node] == 1 && cell_data[node].size() == 0) {
      DCHECK(current_refs_cnt == 0);
      current_refs_cnt = 1;
    }
    append_uint(result, current_refs_cnt, 4);

    if (cell_type[node] != 1 && current_cell_type != 9) {
      if (is_data_small[node]) {
        append_uint(result, 1, 1);
        append_uint(result, cell_data[node].size(), 7);
      } else {
        append_uint(result, 0, 1);
        append_uint(result, 1 + cell_data[node].size() / 8, 7);
      }
    }
  }

  // Store edge information
  auto edge_bits = result.reserve_bitslice(edge_count).bits();
  for (int i = 0; i < node_count; ++i) {
    size_t node = topo_order[i];
    for (size_t child_index = 0; child_index < refs_cnt[node]; ++child_index) {
      size_t child = boc_graph[node][child_index];
      edge_bits.store_uint(rank[child] == i + 1, 1);
      ++edge_bits;
    }
  }

  // Store cell data
  for (size_t node : topo_order) {
    if (prunned_branch_level[node] == 9) {
      continue;
    }
    if (cell_type[node] != 1 && !is_data_small[node]) {
      continue;
    }
    result.append(cell_data[node].subslice(0, cell_data[node].size() % 8));
  }

  // Store BOC graph with optimized encoding
  for (size_t i = 0; i < node_count; ++i) {
    size_t node = topo_order[i];
    if (node_count <= i + 3)
      continue;

    for (int j = 0; j < refs_cnt[node]; ++j) {
      if (rank[boc_graph[node][j]] <= i + 1)
        continue;

      int delta = rank[boc_graph[node][j]] - i - 2;  // Always >= 0 because of above check
      size_t required_bits = 1 + (31 ^ td::count_leading_zeroes32(node_count - i - 3));

      if (required_bits < 8 - (result.size() + 1) % 8 + 1) {
        append_uint(result, delta, required_bits);
      } else if (delta < (1 << (8 - (result.size() + 1) % 8))) {
        size_t available_bits = 8 - (result.size() + 1) % 8;
        append_uint(result, 1, 1);
        append_uint(result, delta, available_bits);
      } else {
        append_uint(result, 0, 1);
        append_uint(result, delta, required_bits);
      }
    }
  }

  // Pad result to byte boundary
  while (result.size() % 8) {
    append_uint(result, 0, 1);
  }

  // Store remaining cell data
  for (size_t node : topo_order) {
    if (prunned_branch_level[node] == 9) {
      continue;
    }
    if (cell_type[node] == 1 || is_data_small[node]) {
      size_t prefix_size = cell_data[node].size() % 8;
      result.append(cell_data[node].subslice(prefix_size, cell_data[node].size() - prefix_size));
    } else {
      size_t data_size = cell_data[node].size() + 1;
      size_t padding = (8 - data_size % 8) % 8;

      if (padding) {
        append_uint(result, 0, padding);
      }
      append_uint(result, 1, 1);
      result.append(cell_data[node]);
    }
  }

  // Final padding
  while (result.size() % 8) {
    append_uint(result, 0, 1);
  }

  // Create final compressed buffer
  td::BufferSlice serialized((const char*)result.bits().get_byte_ptr(), result.size() / 8);

  td::BufferSlice compressed = td::lz4_compress(serialized);

  // Add decompressed size at the beginning
  td::BufferSlice compressed_with_size(compressed.size() + kDecompressedSizeBytes);
  auto size_slice = td::BitSliceWrite(compressed_with_size.as_slice().ubegin(), kDecompressedSizeBytes * 8);
  size_slice.bits().store_uint(serialized.size(), kDecompressedSizeBytes * 8);
  memcpy(compressed_with_size.data() + kDecompressedSizeBytes, compressed.data(), compressed.size());

  return compressed_with_size;
}

// Helper: write ShardAccounts augmentation (DepthBalanceInfo with grams) into builder
bool write_depth_balance_grams(vm::CellBuilder& cb, const td::RefInt256& grams) {
  if (!cb.store_zeroes_bool(7)) {  // empty HmLabel and split_depth
    return false;
  }
  if (!block::tlb::t_CurrencyCollection.pack_special(cb, grams, td::Ref<vm::Cell>())) {
    return false;
  }
  return true;
}

// Helper: detect MerkleUpdate (is_special AND first byte == 0x04) without finalizing
bool is_merkle_update_node(bool is_special, const vm::CellBuilder& cb) {
  if (!is_special) {
    return false;
  }
  // Need at least one full byte in data to read the tag
  if (cb.get_bits() < 8) {
    return false;
  }
  unsigned first_byte = cb.get_data()[0];
  return first_byte == 0x04;
}

td::Result<std::vector<td::Ref<vm::Cell>>> boc_decompress_improved_structure_lz4(td::Slice compressed, int max_decompressed_size,
    bool decompress_merkle_update, td::Ref<vm::Cell> state
  ) {
  constexpr size_t kMaxCellDataLengthBits = 1024;

  if (decompress_merkle_update && state.is_null()) {
    return td::Status::Error("BOC decompression failed: state is required for MU decompressing");
  }

  // Check minimum input size for decompressed size header
  if (compressed.size() < kDecompressedSizeBytes) {
    return td::Status::Error("BOC decompression failed: input too small for header");
  }

  // Read decompressed size
  constexpr size_t kSizeBits = kDecompressedSizeBytes * 8;
  size_t decompressed_size = td::BitSlice(compressed.ubegin(), kSizeBits).bits().get_uint(kSizeBits);
  compressed.remove_prefix(kDecompressedSizeBytes);
  if (decompressed_size > max_decompressed_size) {
    return td::Status::Error("BOC decompression failed: invalid decompressed size");
  }

  // Decompress LZ4 data
  TRY_RESULT(serialized, td::lz4_decompress(compressed, decompressed_size));

  if (serialized.size() != decompressed_size) {
    return td::Status::Error("BOC decompression failed: decompressed size mismatch");
  }

  // Initialize bit reader
  td::BitSlice bit_reader(serialized.as_slice().ubegin(), serialized.as_slice().size() * 8);
  size_t orig_size = bit_reader.size();

  // Read root count
  TRY_RESULT(root_count, read_uint(bit_reader, 32));
  // We assume that each cell should take at least 1 byte, even effectively serialized
  // Otherwise it means that provided root_count is incorrect
  if (root_count < 1 || root_count > decompressed_size) {
    return td::Status::Error("BOC decompression failed: invalid root count");
  }

  std::vector<size_t> root_indexes(root_count);
  for (int i = 0; i < root_count; ++i) {
    TRY_RESULT_ASSIGN(root_indexes[i], read_uint(bit_reader, 32));
  }

  // Read number of nodes from header
  TRY_RESULT(node_count, read_uint(bit_reader, 32));
  if (node_count < 1) {
    return td::Status::Error("BOC decompression failed: invalid node count");
  }

  // We assume that each cell should take at least 1 byte, even effectively serialized
  // Otherwise it means that provided node_count is incorrect
  if (node_count > decompressed_size) {
    return td::Status::Error("BOC decompression failed: incorrect node count provided");
  }

  // Validate root indexes
  for (int i = 0; i < root_count; ++i) {
    if (root_indexes[i] >= node_count) {
      return td::Status::Error("BOC decompression failed: invalid root index");
    }
  }

  // Initialize data structures
  std::vector<size_t> cell_data_length(node_count), is_data_small(node_count);
  std::vector<size_t> is_special(node_count), cell_refs_cnt(node_count), is_depth_balance(node_count);
  std::vector<size_t> prunned_branch_level(node_count, 0);

  std::vector<vm::CellBuilder> cell_builders(node_count);
  std::vector<std::array<size_t, 4>> boc_graph(node_count);

  // Read cell metadata
  for (int i = 0; i < node_count; ++i) {
    // Check enough bits for cell type and refs count
    if (bit_reader.size() < 8) {
      return td::Status::Error("BOC decompression failed: not enough bits for cell metadata");
    }

    size_t cell_type = bit_reader.bits().get_uint(4);
    is_special[i] = (cell_type == 9 ? false : bool(cell_type));
    is_depth_balance[i] = cell_type == 9;
    if (is_special[i]) {
      prunned_branch_level[i] = cell_type - 1;
    }
    bit_reader.advance(4);

    cell_refs_cnt[i] = bit_reader.bits().get_uint(4);
    bit_reader.advance(4);
    if (cell_refs_cnt[i] > 4) {
      return td::Status::Error("BOC decompression failed: invalid cell refs count");
    }
    if (is_depth_balance[i]) {
      cell_data_length[i] = 0;
    } else if (prunned_branch_level[i]) {
      size_t coef = std::bitset<4>(prunned_branch_level[i]).count();
      cell_data_length[i] = (256 + 16) * coef;
      if (cell_refs_cnt[i] == 1) {
        cell_refs_cnt[i] = 0;
        cell_data_length[i] = 0;
      }
    } else {
      // Check enough bits for data length metadata
      if (bit_reader.size() < 8) {
        return td::Status::Error("BOC decompression failed: not enough bits for data length");
      }

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

    // Validate cell data length
    if (cell_data_length[i] > kMaxCellDataLengthBits) {
      return td::Status::Error("BOC decompression failed: invalid cell data length");
    }
  }

  // Read direct edge connections
  for (int i = 0; i < node_count; ++i) {
    for (int j = 0; j < cell_refs_cnt[i]; ++j) {
      TRY_RESULT(edge_connection, read_uint(bit_reader, 1));
      if (edge_connection) {
        boc_graph[i][j] = i + 1;
      }
    }
  }

  // Read initial cell data
  for (int i = 0; i < node_count; ++i) {
    if (is_depth_balance[i]) {
      continue;
    }
    if (prunned_branch_level[i]) {
      cell_builders[i].store_long((1 << 8) + prunned_branch_level[i], 16);
    }

    size_t remainder_bits = cell_data_length[i] % 8;
    if (bit_reader.size() < remainder_bits) {
      return td::Status::Error("BOC decompression failed: not enough bits for initial cell data");
    }
    cell_builders[i].store_bits(bit_reader.subslice(0, remainder_bits));
    bit_reader.advance(remainder_bits);
    cell_data_length[i] -= remainder_bits;
  }

  // Decode remaining edge connections
  for (size_t i = 0; i < node_count; ++i) {
    if (node_count <= i + 3) {
      for (int j = 0; j < cell_refs_cnt[i]; ++j) {
        if (!boc_graph[i][j]) {
          boc_graph[i][j] = i + 2;
        }
      }
      continue;
    }

    for (int j = 0; j < cell_refs_cnt[i]; ++j) {
      if (!boc_graph[i][j]) {
        size_t pref_size = (orig_size - bit_reader.size());
        size_t required_bits = 1 + (31 ^ td::count_leading_zeroes32(node_count - i - 3));

        if (required_bits < 8 - (pref_size + 1) % 8 + 1) {
          TRY_RESULT_ASSIGN(boc_graph[i][j], read_uint(bit_reader, required_bits));
          boc_graph[i][j] += i + 2;
        } else {
          TRY_RESULT(edge_connection, read_uint(bit_reader, 1));
          if (edge_connection) {
            pref_size = (orig_size - bit_reader.size());
            size_t available_bits = 8 - pref_size % 8;
            TRY_RESULT_ASSIGN(boc_graph[i][j], read_uint(bit_reader, available_bits));
            boc_graph[i][j] += i + 2;
          } else {
            TRY_RESULT_ASSIGN(boc_graph[i][j], read_uint(bit_reader, required_bits));
            boc_graph[i][j] += i + 2;
          }
        }
      }
    }
  }

  // Check if all graph connections are valid
  for (int node = 0; node < node_count; ++node) {
    for (int j = 0; j < cell_refs_cnt[node]; ++j) {
      size_t child_node = boc_graph[node][j];
      if (child_node >= node_count) {
        return td::Status::Error("BOC decompression failed: invalid graph connection");
      }
      if (child_node <= node) {
        return td::Status::Error("BOC decompression failed: circular reference in graph");
      }
    }
  }

  // Align to byte boundary
  while ((orig_size - bit_reader.size()) % 8) {
    TRY_RESULT(bit, read_uint(bit_reader, 1));
  }

  // Read remaining cell data
  for (int i = 0; i < node_count; ++i) {
    if (is_depth_balance[i]) {
      continue;
    }
    size_t padding_bits = 0;
    if (!prunned_branch_level[i] && !is_data_small[i]) {
      while (bit_reader.size() > 0 && bit_reader.bits()[0] == 0) {
        ++padding_bits;
        bit_reader.advance(1);
      }
      TRY_RESULT(bit, read_uint(bit_reader, 1));
      ++padding_bits;
    }
    if (cell_data_length[i] < padding_bits) {
      return td::Status::Error("BOC decompression failed: invalid cell data length");
    }
    size_t remaining_data_bits = cell_data_length[i] - padding_bits;
    if (bit_reader.size() < remaining_data_bits) {
      return td::Status::Error("BOC decompression failed: not enough bits for remaining cell data");
    }

    cell_builders[i].store_bits(bit_reader.subslice(0, remaining_data_bits));
    bit_reader.advance(remaining_data_bits);
  }

  // Build cell tree
  std::vector<td::Ref<vm::Cell>> nodes(node_count);

  // Helper: finalize a node by storing refs and finalizing the builder
  auto finalize_node = [&](size_t idx) -> td::Status {
    try {
      for (int j = 0; j < cell_refs_cnt[idx]; ++j) {
        cell_builders[idx].store_ref(nodes[boc_graph[idx][j]]);
      }
      try {
        nodes[idx] = cell_builders[idx].finalize(is_special[idx]);
      } catch (vm::CellBuilder::CellWriteError& e) {
        return td::Status::Error(PSTRING() << "BOC decompression failed: failed to finalize node (CellWriteError)");
      }
    } catch (vm::VmError& e) {
      return td::Status::Error(PSTRING() << "BOC decompression failed: failed to finalize node (VmError)");
    }
    return td::Status::OK();
  };

  auto build_prunned_branch_from_state = [&](size_t idx, td::Ref<vm::Cell> source_cell) -> td::Status {
    size_t mask_value = prunned_branch_level[idx];
    if (!mask_value) {
      return td::Status::Error(
          "BOC decompression failed: invalid prunned branch metadata inside MerkleUpdate left subtree");
    }
    if (cell_refs_cnt[idx] != 0) {
      return td::Status::Error(
          "BOC decompression failed: prunned branch node unexpectedly has references inside MerkleUpdate left subtree");
    }

    td::uint32 mask = static_cast<td::uint32>(mask_value);
    int leading_zeroes = td::count_leading_zeroes32(mask);
    if (leading_zeroes >= 32) {
      return td::Status::Error(
          "BOC decompression failed: unable to determine level mask for prunned branch under MerkleUpdate");
    }
    td::uint32 highest_bit = 31 - leading_zeroes;
    td::uint32 highest_bit_mask = 1u << highest_bit;
    td::uint32 base_mask = mask & (highest_bit_mask ? (highest_bit_mask - 1) : 0);
    vm::Cell::LevelMask level_mask(base_mask);
    td::uint32 max_level = level_mask.get_level();
    if (source_cell->get_level() < max_level) {
      return td::Status::Error(
          "BOC decompression failed: state subtree level is too small for requested prunned branch");
    }

    cell_builders[idx].reset();
    cell_builders[idx].store_long(static_cast<td::uint8>(vm::CellTraits::SpecialType::PrunnedBranch), 8);
    cell_builders[idx].store_long(mask, 8);

    for (td::uint32 lvl = 0; lvl <= max_level; ++lvl) {
      if (!level_mask.is_significant(lvl)) {
        continue;
      }
      cell_builders[idx].store_bytes(source_cell->get_hash(lvl).as_slice());
    }
    for (td::uint32 lvl = 0; lvl <= max_level; ++lvl) {
      if (!level_mask.is_significant(lvl)) {
        continue;
      }
      cell_builders[idx].store_long(source_cell->get_depth(lvl), 16);
    }

    return finalize_node(idx);
  };

  // Recursively rebuild the left subtree of a MerkleUpdate by mirroring the provided state tree.
  std::function<td::Status(size_t, td::Ref<vm::Cell>)> build_left_under_mu =
      [&](size_t left_idx, td::Ref<vm::Cell> state_cell) -> td::Status {
    if (state_cell.is_null()) {
      return td::Status::Error("BOC decompression failed: missing state subtree for MerkleUpdate left branch");
    }
    bool is_prunned_branch = prunned_branch_level[left_idx] != 0;
    if (nodes[left_idx].not_null()) {
      if (!is_prunned_branch && nodes[left_idx]->get_hash() != state_cell->get_hash()) {
        return td::Status::Error(
            "BOC decompression failed: inconsistent state subtree reused within MerkleUpdate left branch");
      }
      return td::Status::OK();
    }

    bool state_is_special = false;
    vm::CellSlice state_slice = vm::load_cell_slice_special(state_cell, state_is_special);
    if (!state_slice.is_valid()) {
      return td::Status::Error(
          "BOC decompression failed: invalid state cell while restoring MerkleUpdate left subtree");
    }
    if (is_prunned_branch) {
      TRY_STATUS(build_prunned_branch_from_state(left_idx, state_cell));
      return td::Status::OK();
    } 
    
    if (state_slice.size_refs() != cell_refs_cnt[left_idx]) {
      return td::Status::Error(
          "BOC decompression failed: state subtree refs mismatch while restoring MerkleUpdate left subtree");
    }
    if (static_cast<bool>(is_special[left_idx]) != state_is_special) {
      return td::Status::Error(
          "BOC decompression failed: state subtree special flag mismatch while restoring MerkleUpdate left subtree");
    }

    for (size_t j = 0; j < cell_refs_cnt[left_idx]; ++j) {
      td::Ref<vm::Cell> child_state = state_slice.prefetch_ref(j);
      TRY_STATUS(build_left_under_mu(boc_graph[left_idx][j], child_state));
    }

    cell_builders[left_idx].reset();
    cell_builders[left_idx].store_bits(state_slice.as_bitslice());
    TRY_STATUS(finalize_node(left_idx));

    return td::Status::OK();
  };

  // Recursively build right subtree under MerkleUpdate, pairing with left subtree, computing sum diffs.
  // Sum is accumulated into sum_diff_out (if non-null), similar to compression flow.
  std::function<td::Status(size_t, size_t, td::RefInt256*)> build_right_under_mu =
      [&](size_t right_idx, size_t left_idx, td::RefInt256* sum_diff_out) -> td::Status {
    if (nodes[right_idx].not_null()) {
      if (left_idx != std::numeric_limits<size_t>::max() && sum_diff_out) {
        vm::CellSlice cs_left(NoVm(), nodes[left_idx]);
        vm::CellSlice cs_right(NoVm(), nodes[right_idx]);
        td::RefInt256 vertex_diff = process_shard_accounts_vertex(cs_left, cs_right);
        if (vertex_diff.not_null()) {
          *sum_diff_out += vertex_diff;
        }
      }
      return td::Status::OK();
    }
    td::RefInt256 cur_right_left_diff;
    // Build children first
    td::RefInt256 sum_child_diff = td::make_refint(0);
    for (int j = 0; j < cell_refs_cnt[right_idx]; ++j) {
      size_t right_child = boc_graph[right_idx][j];
      size_t left_child = (left_idx != std::numeric_limits<size_t>::max() && j < cell_refs_cnt[left_idx])
                            ? boc_graph[left_idx][j]
                            : std::numeric_limits<size_t>::max();
      TRY_STATUS(build_right_under_mu(right_child, left_child, &sum_child_diff));
    }
    // If this vertex was depth-balance-compressed, reconstruct its data from left + children sum
    if (is_depth_balance[right_idx]) {
      vm::CellSlice cs_left(NoVm(), nodes[left_idx]);
      td::RefInt256 left_grams = extract_balance_from_depth_balance_info(cs_left);
      if (left_grams.is_null()) {
        return td::Status::Error("BOC decompression failed: depth-balance left vertex has no grams");
      }
      td::RefInt256 expected_right_grams = left_grams;
      expected_right_grams += sum_child_diff;
      if (!write_depth_balance_grams(cell_builders[right_idx], expected_right_grams)) {
        return td::Status::Error("BOC decompression failed: failed to write depth-balance grams");
      }
      cur_right_left_diff = sum_child_diff;
    }

    // Store children refs and finalize this right node
    TRY_STATUS(finalize_node(right_idx));

    // Compute this vertex diff (right - left) to propagate upward
    if (cur_right_left_diff.is_null() && left_idx != std::numeric_limits<size_t>::max()) {
      vm::CellSlice cs_left(NoVm(), nodes[left_idx]);
      vm::CellSlice cs_right(NoVm(), nodes[right_idx]);
      cur_right_left_diff = process_shard_accounts_vertex(cs_left, cs_right);
    }
    if (sum_diff_out && cur_right_left_diff.not_null()) {
      *sum_diff_out += cur_right_left_diff;
    }
    return td::Status::OK();
  };

  // General recursive build that handles MerkleUpdate by pairing left/right subtrees
  std::function<td::Status(size_t)> build_node = [&](size_t idx) -> td::Status {
    if (nodes[idx].not_null()) {
      return td::Status::OK();
    }
    // If this node is a MerkleUpdate, build left subtree normally first, then right subtree paired with left
    if (is_merkle_update_node(is_special[idx], cell_builders[idx])) {
      size_t left_idx = boc_graph[idx][0];
      size_t right_idx = boc_graph[idx][1];
      if (decompress_merkle_update) {
        TRY_STATUS(build_left_under_mu(left_idx, state));
      } else {
        TRY_STATUS(build_node(left_idx));
      }
      TRY_STATUS(build_right_under_mu(right_idx, left_idx, nullptr));
      TRY_STATUS(finalize_node(idx));
      return td::Status::OK();
    } else {
      // Default: build children normally then finalize
      for (int j = 0; j < cell_refs_cnt[idx]; ++j) {
        TRY_STATUS(build_node(boc_graph[idx][j]));
      } 
    }
    
    TRY_STATUS(finalize_node(idx));
    return td::Status::OK();
  };

  // Build from roots using DFS
  for (size_t index : root_indexes) {
    TRY_STATUS(build_node(index));
  }

  std::vector<td::Ref<vm::Cell>> root_nodes;
  root_nodes.reserve(root_count);
  for (size_t index : root_indexes) {
    root_nodes.push_back(nodes[index]);
  }

  return root_nodes;
}

td::Result<td::BufferSlice> boc_compress(const std::vector<td::Ref<vm::Cell>>& boc_roots, CompressionAlgorithm algo, 
  td::Ref<vm::Cell> state
) {
  // Check for empty input
  if (boc_roots.empty()) {
    return td::Status::Error("Cannot compress empty BOC roots");
  }

  td::BufferSlice compressed;
  if (algo == CompressionAlgorithm::BaselineLZ4) {
    TRY_RESULT_ASSIGN(compressed, boc_compress_baseline_lz4(boc_roots));
  } else if (algo == CompressionAlgorithm::ImprovedStructureLZ4) {
    TRY_RESULT_ASSIGN(compressed, boc_compress_improved_structure_lz4(boc_roots, false));
  } else if (algo == CompressionAlgorithm::ImprovedStructureLZ4WithMU) {
    TRY_RESULT_ASSIGN(compressed, boc_compress_improved_structure_lz4(boc_roots, true, state));
  } else {
      return td::Status::Error("Unknown compression algorithm");
  }

  td::BufferSlice compressed_with_algo(compressed.size() + 1);
  compressed_with_algo.data()[0] = int(algo);
  memcpy(compressed_with_algo.data() + 1, compressed.data(), compressed.size());
  return compressed_with_algo;
}

td::Result<std::vector<td::Ref<vm::Cell>>> boc_decompress(td::Slice compressed, int max_decompressed_size,
  td::Ref<vm::Cell> state
) {
  if (compressed.size() == 0) {
    return td::Status::Error("Can't decompress empty data");
  }

  CompressionAlgorithm algo = static_cast<CompressionAlgorithm>(compressed[0]);
  compressed.remove_prefix(1);

  switch (algo) {
    case CompressionAlgorithm::BaselineLZ4:
      return boc_decompress_baseline_lz4(compressed, max_decompressed_size);
    case CompressionAlgorithm::ImprovedStructureLZ4:
      return boc_decompress_improved_structure_lz4(compressed, max_decompressed_size, false);
    case CompressionAlgorithm::ImprovedStructureLZ4WithMU:
      return boc_decompress_improved_structure_lz4(compressed, max_decompressed_size, true, state);
  }
  return td::Status::Error("Unknown compression algorithm");
}

td::Result<bool> boc_need_state_for_decompression(td::Slice compressed) {
  if (compressed.size() == 0) {
    return td::Status::Error("Can't check algorithm on empty data");
  }

  CompressionAlgorithm algo = static_cast<CompressionAlgorithm>(compressed[0]);
  
  switch (algo) {
    case CompressionAlgorithm::BaselineLZ4:
    case CompressionAlgorithm::ImprovedStructureLZ4:
      return false;
    case CompressionAlgorithm::ImprovedStructureLZ4WithMU:
      return true;
    default:
      return td::Status::Error("Unknown compression algorithm");
  }
}

}  // namespace vm
