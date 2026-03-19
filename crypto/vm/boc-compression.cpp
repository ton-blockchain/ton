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

#include "common/bitstring.h"
#include "common/refint.h"
#include "crypto/block/block-auto.h"
#include "crypto/block/block-parse.h"
#include "td/utils/Slice-decl.h"
#include "td/utils/lz4.h"
#include "vm/boc-writers.h"
#include "vm/boc.h"
#include "vm/cells.h"
#include "vm/cellslice.h"

#include "boc-compression.h"

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

td::Result<std::vector<td::Ref<vm::Cell>>> boc_decompress_baseline_lz4(td::Slice compressed,
                                                                       unsigned max_decompressed_size) {
  // Check minimum input size for decompressed size header
  if (compressed.size() < kDecompressedSizeBytes) {
    return td::Status::Error("BOC decompression failed: input too small for header");
  }

  // Read decompressed size
  constexpr unsigned kSizeBits = kDecompressedSizeBytes * 8;
  unsigned decompressed_size =
      td::narrow_cast<unsigned>(td::BitSlice(compressed.ubegin(), kSizeBits).bits().get_uint(kSizeBits));
  compressed.remove_prefix(kDecompressedSizeBytes);
  if (decompressed_size == 0 || decompressed_size > max_decompressed_size) {
    return td::Status::Error("BOC decompression failed: invalid decompressed size");
  }

  TRY_RESULT(decompressed_size_int, td::narrow_cast_safe<int>(decompressed_size));
  TRY_RESULT(decompressed, td::lz4_decompress(compressed, decompressed_size_int));
  TRY_RESULT(roots, vm::std_boc_deserialize_multi(decompressed));
  return roots;
}

inline void append_uint(td::BitString& bs, unsigned long long val, unsigned bits) {
  bs.reserve_bitslice(bits).bits().store_uint(val, bits);
}

inline td::Result<unsigned> read_uint(td::BitSlice& bs, unsigned bits) {
  // Check if there enough bits available
  if (bs.size() < bits) {
    return td::Status::Error("BOC decompression failed: not enough bits to read");
  }
  DCHECK(bits <= 32);
  unsigned result = td::narrow_cast<unsigned>(bs.bits().get_uint(bits));
  bs.advance(bits);
  return result;
}

// Decode DepthBalanceInfo and extract grams using TLB methods
td::RefInt256 extract_balance_from_depth_balance_info(vm::CellSlice& cs) {
  // Check hashmap label is empty ('00')
  if (cs.size() < 2 || cs.fetch_ulong(2) != 0) {
    return td::RefInt256{};
  }

  unsigned split_depth;
  Ref<vm::CellSlice> balance_cs_ref;
  if (!block::gen::t_DepthBalanceInfo.unpack_depth_balance(cs, split_depth, balance_cs_ref)) {
    return td::RefInt256{};
  }
  if (split_depth != 0u) {
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

td::Result<td::BufferSlice> boc_compress_improved_structure_lz4(const std::vector<td::Ref<vm::Cell>>& boc_roots,
                                                                bool compress_merkle_update, td::Ref<vm::Cell> state) {
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
  td::HashMap<vm::Cell::Hash, unsigned> cell_hashes;
  std::vector<std::array<unsigned, 4>> boc_graph;
  std::vector<unsigned> refs_cnt;
  std::vector<td::BitSlice> cell_data;
  std::vector<unsigned> cell_type;
  std::vector<unsigned> pb_level_mask;
  std::vector<unsigned> root_indexes;
  unsigned total_size_estimate = 0;

  // Build graph representation using recursive lambda
  const auto build_graph = [&](auto&& self, td::Ref<vm::Cell> cell, const td::Ref<vm::Cell>& main_mu_cell,
                               bool under_mu_left = false, bool under_mu_right = false,
                               td::Ref<vm::Cell> left_cell = td::Ref<vm::Cell>(),
                               td::RefInt256* sum_diff_out = nullptr) -> td::Result<unsigned> {
    if (cell.is_null()) {
      return td::Status::Error("Error while importing a cell during serialization: cell is null");
    }

    auto cell_hash = cell->get_hash();
    auto it = cell_hashes.find(cell_hash);
    if (it != cell_hashes.end()) {
      return it->second;
    }

    unsigned current_cell_id = td::narrow_cast<unsigned>(boc_graph.size());
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
    cell_type.emplace_back(static_cast<unsigned>(cell_slice.special_type()));
    pb_level_mask.push_back(0);

    DCHECK(cell_slice.size_refs() <= 4);

    // Process special cell of type PrunnedBranch
    if (cell_slice.special_type() == vm::CellTraits::SpecialType::PrunnedBranch) {
      DCHECK(cell_slice.size() >= 16);
      cell_data.emplace_back(cell_bitslice.subslice(16, cell_bitslice.size() - 16));
      pb_level_mask.back() = cell_slice.data()[1];
    } else {
      cell_data.emplace_back(cell_bitslice);
    }

    if (compress_merkle_update && under_mu_left) {
      cell_data.back() = td::BitSlice();
    }
    total_size_estimate += cell_bitslice.size();

    // Process cell references
    if (kMURemoveSubtreeSums && cell_slice.special_type() == vm::CellTraits::SpecialType::MerkleUpdate &&
        main_mu_cell.not_null() && cell_hash == main_mu_cell->get_hash()) {
      // Left branch: traverse normally
      TRY_RESULT(child_left_id, self(self, cell_slice.prefetch_ref(0), main_mu_cell, true));
      boc_graph[current_cell_id][0] = child_left_id;
      // Right branch: traverse paired with left and compute diffs inline
      TRY_RESULT(child_right_id,
                 self(self, cell_slice.prefetch_ref(1), main_mu_cell, false, true, cell_slice.prefetch_ref(0)));
      boc_graph[current_cell_id][1] = child_right_id;
    } else if (under_mu_right && left_cell.not_null()) {
      // Inline computation for RIGHT subtree nodes under MerkleUpdate
      vm::CellSlice cs_left(NoVm(), left_cell);
      td::RefInt256 sum_child_diff = td::make_refint(0);
      // Recurse children first
      for (unsigned i = 0; i < cell_slice.size_refs(); ++i) {
        TRY_RESULT(child_id, self(self, cell_slice.prefetch_ref(i), main_mu_cell, false, true, cs_left.prefetch_ref(i),
                                  &sum_child_diff));
        boc_graph[current_cell_id][i] = child_id;
      }

      // Compute this vertex diff and check skippable condition
      td::RefInt256 vertex_diff = process_shard_accounts_vertex(cs_left, cell_slice);
      if (!is_special && vertex_diff.not_null() && sum_child_diff.not_null() && cmp(sum_child_diff, vertex_diff) == 0) {
        cell_data[current_cell_id] = td::BitSlice();
        pb_level_mask[current_cell_id] = 9;
      }
      if (sum_diff_out && vertex_diff.not_null()) {
        *sum_diff_out += vertex_diff;
      }
    } else {
      for (unsigned i = 0; i < cell_slice.size_refs(); ++i) {
        TRY_RESULT(child_id, self(self, cell_slice.prefetch_ref(i), main_mu_cell, under_mu_left, under_mu_right));
        boc_graph[current_cell_id][i] = child_id;
      }
    }

    return current_cell_id;
  };

  // Build the graph starting from roots
  for (auto root : boc_roots) {
    td::Ref<vm::Cell> main_mu_cell;
    bool root_is_special;
    vm::CellSlice root_slice = vm::load_cell_slice_special(root, root_is_special);
    if (root_slice.is_valid() && root_slice.size_refs() > kMUCellOrderInRoot) {
      main_mu_cell = root_slice.prefetch_ref(kMUCellOrderInRoot);
    }

    TRY_RESULT(root_cell_id, build_graph(build_graph, root, main_mu_cell));
    root_indexes.push_back(root_cell_id);
  }

  // Check graph properties
  const unsigned node_count = td::narrow_cast<unsigned>(boc_graph.size());
  std::vector<std::vector<unsigned>> reverse_graph(node_count);
  unsigned edge_count = 0;

  // Build reverse graph
  for (unsigned i = 0; i < node_count; ++i) {
    for (unsigned child_index = 0; child_index < refs_cnt[i]; ++child_index) {
      unsigned child = boc_graph[i][child_index];
      ++edge_count;
      reverse_graph[child].push_back(i);
    }
  }

  // Process cell data sizes
  std::vector<unsigned> is_data_small(node_count, 0);
  for (unsigned i = 0; i < node_count; ++i) {
    if (cell_type[i] != 1) {
      is_data_small[i] = cell_data[i].size() < 128;
    }
  }

  // Perform topological sort
  std::vector<unsigned> topo_order, rank(node_count);
  const auto topological_sort = [&]() -> td::Status {
    std::vector<unsigned> queue;
    queue.reserve(node_count);
    std::vector<unsigned> in_degree(node_count);

    // Calculate in-degrees and initialize queue
    for (unsigned i = 0; i < node_count; ++i) {
      in_degree[i] = refs_cnt[i];
      if (in_degree[i] == 0) {
        queue.push_back(i);
      }
    }

    if (queue.empty()) {
      return td::Status::Error("Cycle detected in cell references");
    }

    std::sort(queue.begin(), queue.end(), [&](unsigned lhs, unsigned rhs) {
      bool lhs_is_ordinary = cell_type[lhs] == 0;
      bool rhs_is_ordinary = cell_type[rhs] == 0;
      if (lhs_is_ordinary != rhs_is_ordinary) {
        return lhs_is_ordinary;
      }
      if (cell_data[lhs].size() != cell_data[rhs].size()) {
        return cell_data[lhs].size() < cell_data[rhs].size();
      }
      return lhs < rhs;
    });
    std::reverse(queue.begin(), queue.end());

    // Process queue
    while (!queue.empty()) {
      unsigned node = queue.back();
      queue.pop_back();
      topo_order.push_back(node);

      for (unsigned parent : reverse_graph[node]) {
        if (--in_degree[parent] == 0) {
          queue.push_back(parent);
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
  for (unsigned i = 0; i < node_count; ++i) {
    rank[topo_order[i]] = i;
  }

  // Build compressed representation
  td::BitString result;
  total_size_estimate += node_count * 10u * 8u;
  result.reserve_bits(total_size_estimate);

  // Store roots information
  const unsigned root_count = td::narrow_cast<unsigned>(root_indexes.size());
  append_uint(result, root_count, 32);
  for (unsigned root_ind : root_indexes) {
    append_uint(result, rank[root_ind], 32);
  }

  // Store node count
  append_uint(result, node_count, 32);

  // Store cell types and sizes
  for (unsigned i = 0; i < node_count; ++i) {
    unsigned node = topo_order[i];
    unsigned current_cell_type = bool(cell_type[node]) + pb_level_mask[node];
    append_uint(result, current_cell_type, 4);
    unsigned current_refs_cnt = refs_cnt[node];
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
  for (unsigned i = 0; i < node_count; ++i) {
    unsigned node = topo_order[i];
    for (unsigned child_index = 0; child_index < refs_cnt[node]; ++child_index) {
      unsigned child = boc_graph[node][child_index];
      edge_bits.store_uint(rank[child] == i + 1, 1);
      ++edge_bits;
    }
  }

  // Store cell data
  for (unsigned node : topo_order) {
    if (pb_level_mask[node] == 9) {
      continue;
    }
    if (cell_type[node] != 1 && !is_data_small[node]) {
      continue;
    }
    result.append(cell_data[node].subslice(0, cell_data[node].size() % 8));
  }

  // Store BOC graph with optimized encoding
  for (unsigned i = 0; i < node_count; ++i) {
    unsigned node = topo_order[i];
    if (node_count <= i + 3) {
      continue;
    }

    for (unsigned j = 0; j < refs_cnt[node]; ++j) {
      if (rank[boc_graph[node][j]] <= i + 1) {
        continue;
      }

      unsigned delta = rank[boc_graph[node][j]] - i - 2;  // Always >= 0 because of above check
      unsigned remaining_nodes = node_count - i - 3;  // Always > 0 because of above node count check
      unsigned required_bits = 32u - td::count_leading_zeroes32(remaining_nodes);

      if (required_bits < 8 - (result.size() + 1) % 8 + 1) {
        append_uint(result, delta, required_bits);
      } else if (delta < (1u << (8 - (result.size() + 1) % 8))) {
        unsigned available_bits = 8 - (result.size() + 1) % 8;
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
  for (unsigned node : topo_order) {
    if (pb_level_mask[node] == 9) {
      continue;
    }
    if (cell_type[node] == 1 || is_data_small[node]) {
      unsigned prefix_size = cell_data[node].size() % 8;
      result.append(cell_data[node].subslice(prefix_size, cell_data[node].size() - prefix_size));
    } else {
      unsigned data_size = cell_data[node].size() + 1;
      unsigned padding = (8 - data_size % 8) % 8;

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

// Helper: detect MerkleUpdate (special cell AND first data byte tag == 0x04).
bool is_merkle_update_node(td::uint8 is_special, td::uint8 pb_level_mask, const td::BitSlice& prefix,
                           const td::BitSlice& suffix) {
  if (!is_special) {
    return false;
  }
  if (pb_level_mask != 0) {
    return false;
  }
  if (prefix.size() + suffix.size() < 8) {
    return false;
  }

  td::uint8 first_byte = 0;
  for (unsigned i = 0; i < 8; ++i) {
    bool bit = i < prefix.size() ? prefix.bits()[i] : suffix.bits()[i - prefix.size()];
    first_byte = static_cast<td::uint8>((first_byte << 1) | static_cast<td::uint8>(bit));
  }
  return first_byte == static_cast<td::uint8>(vm::CellTraits::SpecialType::MerkleUpdate);
}

td::Result<std::vector<td::Ref<vm::Cell>>> boc_decompress_improved_structure_lz4(td::Slice compressed,
                                                                                 unsigned max_decompressed_size,
                                                                                 bool decompress_merkle_update,
                                                                                 td::Ref<vm::Cell> state) {
  constexpr unsigned kMaxCellDataLengthBits = 1024;
  constexpr unsigned kMaxNodeCount = (1u << 20);

  if (decompress_merkle_update && state.is_null()) {
    return td::Status::Error("BOC decompression failed: state is required for MU decompressing");
  }

  // Check minimum input size for decompressed size header
  if (compressed.size() < kDecompressedSizeBytes) {
    return td::Status::Error("BOC decompression failed: input too small for header");
  }

  // Read decompressed size
  constexpr unsigned kSizeBits = kDecompressedSizeBytes * 8;
  unsigned decompressed_size = td::narrow_cast<unsigned>(td::BitSlice(compressed.ubegin(), kSizeBits).bits().get_uint(kSizeBits));
  compressed.remove_prefix(kDecompressedSizeBytes);
  if (decompressed_size > max_decompressed_size) {
    return td::Status::Error("BOC decompression failed: invalid decompressed size");
  }

  // Decompress LZ4 data
  TRY_RESULT(decompressed_size_int, td::narrow_cast_safe<int>(decompressed_size));
  TRY_RESULT(serialized, td::lz4_decompress(compressed, decompressed_size_int));

  if (serialized.size() != decompressed_size) {
    return td::Status::Error("BOC decompression failed: decompressed size mismatch");
  }

  // Initialize bit reader
  td::BitSlice bit_reader(serialized.as_slice().ubegin(),
                          td::narrow_cast<unsigned>(serialized.as_slice().size() * 8));
  unsigned orig_size = bit_reader.size();

  // Read root count
  TRY_RESULT(root_count, read_uint(bit_reader, 32));
  if (root_count == 0 || root_count > BagOfCells::default_max_roots) {
    return td::Status::Error("BOC decompression failed: invalid root count");
  }
  // We assume that each root should take at least 1 byte in the decompressed payload.
  if (root_count > decompressed_size) {
    return td::Status::Error("BOC decompression failed: invalid root count");
  }

  std::vector<unsigned> root_indexes(root_count);
  for (unsigned i = 0; i < root_count; ++i) {
    TRY_RESULT(root_index, read_uint(bit_reader, 32));
    root_indexes[i] = root_index;
  }

  // Read number of nodes from header
  TRY_RESULT(node_count, read_uint(bit_reader, 32));
  if (node_count == 0) {
    return td::Status::Error("BOC decompression failed: invalid node count");
  }
  if (node_count > kMaxNodeCount) {
    return td::Status::Error("BOC decompression failed: invalid node count");
  }

  // We assume that each cell should take at least 1 byte, even effectively serialized
  // Otherwise it means that provided node_count is incorrect
  if (node_count > decompressed_size) {
    return td::Status::Error("BOC decompression failed: incorrect node count provided");
  }

  // Validate root indexes
  for (unsigned i = 0; i < root_count; ++i) {
    if (root_indexes[i] >= node_count) {
      return td::Status::Error("BOC decompression failed: invalid root index");
    }
  }

  // Initialize data structures
  std::vector<td::uint16> cell_data_length(node_count);
  std::vector<td::uint8> is_data_small(node_count, 0);
  std::vector<td::uint8> is_special(node_count, 0);
  std::vector<td::uint8> cell_refs_cnt(node_count, 0);
  std::vector<td::uint8> is_depth_balance(node_count, 0);
  std::vector<td::uint8> pb_level_mask(node_count, 0);

  // Cell data bits are stored as two views into the decompressed bitstream, and later fed into vm::CellBuilder to finalize the nodes.
  std::vector<td::BitSlice> cell_data_prefix(node_count);
  std::vector<td::BitSlice> cell_data_suffix(node_count);

  std::vector<std::array<td::uint32, 4>> boc_graph(node_count);
  for (auto& a : boc_graph) {
    a.fill(0);
  }

  // Read cell metadata
  for (unsigned i = 0; i < node_count; ++i) {
    // Check enough bits for cell type and refs count
    if (bit_reader.size() < 8) {
      return td::Status::Error("BOC decompression failed: not enough bits for cell metadata");
    }

    unsigned cell_type = td::narrow_cast<unsigned>(bit_reader.bits().get_uint(4));
    is_special[i] = (cell_type == 9 ? 0 : static_cast<td::uint8>(bool(cell_type)));
    is_depth_balance[i] = (cell_type == 9);
    if (is_special[i]) {
      pb_level_mask[i] = static_cast<td::uint8>(cell_type - 1);
    }
    bit_reader.advance(4);

    cell_refs_cnt[i] = static_cast<td::uint8>(bit_reader.bits().get_uint(4));
    bit_reader.advance(4);
    if (cell_refs_cnt[i] > 4) {
      return td::Status::Error("BOC decompression failed: invalid cell refs count");
    }
    if (is_depth_balance[i]) {
      cell_data_length[i] = 0;
    } else if (pb_level_mask[i]) {
      unsigned coef = td::count_bits32(pb_level_mask[i]);
      cell_data_length[i] = static_cast<td::uint16>((256 + 16) * coef);
      if (cell_refs_cnt[i] == 1) {
        cell_refs_cnt[i] = 0;
        cell_data_length[i] = 0;
      } else if (cell_refs_cnt[i] > 1) {
        return td::Status::Error("BOC decompression failed: pruned branch cannot have references");
      }
    } else {
      // Check enough bits for data length metadata
      if (bit_reader.size() < 8) {
        return td::Status::Error("BOC decompression failed: not enough bits for data length");
      }

      is_data_small[i] = static_cast<td::uint8>(bit_reader.bits().get_uint(1));
      bit_reader.advance(1);
      cell_data_length[i] = static_cast<td::uint16>(bit_reader.bits().get_uint(7));
      bit_reader.advance(7);

      if (!is_data_small[i]) {
        cell_data_length[i] = static_cast<td::uint16>(cell_data_length[i] * 8);
        if (!cell_data_length[i]) {
          cell_data_length[i] = static_cast<td::uint16>(cell_data_length[i] + 1024);
        }
      }
    }

    // Validate cell data length
    if (cell_data_length[i] > kMaxCellDataLengthBits) {
      return td::Status::Error("BOC decompression failed: invalid cell data length");
    }
  }

  // Read direct edge connections
  for (unsigned i = 0; i < node_count; ++i) {
    for (unsigned j = 0; j < cell_refs_cnt[i]; ++j) {
      TRY_RESULT(edge_connection, read_uint(bit_reader, 1));
      if (edge_connection) {
        boc_graph[i][j] = i + 1;
      }
    }
  }

  // Read initial cell data
  for (unsigned i = 0; i < node_count; ++i) {
    if (is_depth_balance[i]) {
      continue;
    }
    unsigned remainder_bits = cell_data_length[i] % 8;
    if (bit_reader.size() < remainder_bits) {
      return td::Status::Error("BOC decompression failed: not enough bits for initial cell data");
    }
    cell_data_prefix[i] = bit_reader.subslice(0, remainder_bits);
    bit_reader.advance(remainder_bits);
    cell_data_length[i] = static_cast<td::uint16>(cell_data_length[i] - remainder_bits);
  }

  // Decode remaining edge connections
  for (unsigned i = 0; i < node_count; ++i) {
    if (node_count <= i + 3) {
      for (unsigned j = 0; j < cell_refs_cnt[i]; ++j) {
        if (!boc_graph[i][j]) {
          boc_graph[i][j] = i + 2;
        }
      }
      continue;
    }

    for (unsigned j = 0; j < cell_refs_cnt[i]; ++j) {
      if (!boc_graph[i][j]) {
        unsigned pref_size = orig_size - bit_reader.size();
        unsigned remaining_nodes = node_count - i - 3;  // Always > 0 because of above node count check
        unsigned required_bits = 32u - td::count_leading_zeroes32(remaining_nodes);

        if (required_bits < 8 - (pref_size + 1) % 8 + 1) {
          TRY_RESULT_ASSIGN(boc_graph[i][j], read_uint(bit_reader, required_bits));
          boc_graph[i][j] += i + 2;
        } else {
          TRY_RESULT(edge_connection, read_uint(bit_reader, 1));
          if (edge_connection) {
            pref_size = orig_size - bit_reader.size();
            unsigned available_bits = 8 - pref_size % 8;
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
  for (unsigned node = 0; node < node_count; ++node) {
    for (unsigned j = 0; j < cell_refs_cnt[node]; ++j) {
      unsigned child_node = boc_graph[node][j];
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
    if (bit != 0) {
      return td::Status::Error("BOC decompression failed: non-zero padding bits");
    }
  }

  // Read remaining cell data
  for (unsigned i = 0; i < node_count; ++i) {
    if (is_depth_balance[i]) {
      continue;
    }
    unsigned padding_bits = 0;
    if (!pb_level_mask[i] && !is_data_small[i]) {
      while (bit_reader.size() > 0 && bit_reader.bits()[0] == 0) {
        ++padding_bits;
        bit_reader.advance(1);
      }
      TRY_STATUS(read_uint(bit_reader, 1));
      ++padding_bits;
    }
    if (cell_data_length[i] < padding_bits) {
      return td::Status::Error("BOC decompression failed: invalid cell data length");
    }
    unsigned remaining_data_bits = cell_data_length[i] - padding_bits;
    if (bit_reader.size() < remaining_data_bits) {
      return td::Status::Error("BOC decompression failed: not enough bits for remaining cell data");
    }

    cell_data_suffix[i] = bit_reader.subslice(0, remaining_data_bits);
    bit_reader.advance(remaining_data_bits);
  }

  // Strict end-of-stream: allow only final zero padding to byte boundary.
  if (bit_reader.size() > 7) {
    return td::Status::Error("BOC decompression failed: trailing unused data");
  }
  while (bit_reader.size() > 0) {
    TRY_RESULT(bit, read_uint(bit_reader, 1));
    if (bit != 0) {
      return td::Status::Error("BOC decompression failed: trailing unused data");
    }
  }

  // Early depth guard: reject graphs that can't fit CellTraits::max_depth before reconstruction.
  // The graph invariant child > parent guarantees reverse-order DP.
  std::vector<td::uint16> node_depth(node_count, 0);
  for (unsigned remaining_nodes = node_count; remaining_nodes > 0; --remaining_nodes) {
    unsigned node = remaining_nodes - 1;
    td::uint16 max_child_depth = 0;
    for (unsigned j = 0; j < cell_refs_cnt[node]; ++j) {
      max_child_depth = std::max(max_child_depth, node_depth[boc_graph[node][j]]);
    }
    if (cell_refs_cnt[node] != 0) {
      if (max_child_depth >= CellTraits::max_depth) {
        return td::Status::Error("BOC decompression failed: cell depth too large");
      }
      node_depth[node] = static_cast<td::uint16>(max_child_depth + 1);
    }
  }

  // Build cell tree
  std::vector<td::Ref<vm::Cell>> nodes(node_count);

  auto finalize_node_from_builder = [&](unsigned idx, vm::CellBuilder& cb) -> td::Status {
    try {
      for (unsigned j = 0; j < cell_refs_cnt[idx]; ++j) {
        cb.store_ref(nodes[boc_graph[idx][j]]);
      }
      try {
        nodes[idx] = cb.finalize(is_special[idx] != 0);
      } catch (vm::CellBuilder::CellWriteError&) {
        return td::Status::Error(PSTRING() << "BOC decompression failed: failed to finalize node (CellWriteError)");
      }
    } catch (vm::VmError&) {
      return td::Status::Error(PSTRING() << "BOC decompression failed: failed to finalize node (VmError)");
    }
    return td::Status::OK();
  };

  // Default finalize: reconstruct builder from stored bit-slices and finalize.
  auto finalize_node = [&](unsigned idx) -> td::Status {
    if (is_depth_balance[idx]) {
      return td::Status::Error("BOC decompression failed: depth-balance node must be reconstructed under MerkleUpdate");
    }
    vm::CellBuilder cb;
    if (pb_level_mask[idx]) {
      cb.store_long((1 << 8) + pb_level_mask[idx], 16);
    }
    cb.store_bits(cell_data_prefix[idx]);
    cb.store_bits(cell_data_suffix[idx]);
    return finalize_node_from_builder(idx, cb);
  };

  auto build_prunned_branch_from_state = [&](unsigned idx, td::Ref<vm::Cell> source_cell) -> td::Status {
    // Mask uniquely defines the PB structure
    vm::Cell::LevelMask level_mask(pb_level_mask[idx]);
    unsigned pb_level = level_mask.get_level();
    if (pb_level == 0 || pb_level > vm::Cell::max_level) {
      return td::Status::Error("BOC decompression failed: invalid level for prunned branch under MerkleUpdate");
    }

    // If source is already at the right level, just copy it
    if (source_cell->get_level() == pb_level) {
      nodes[idx] = source_cell;
      return td::Status::OK();
    }

    try {
      td::Ref<vm::Cell> pb_cell = vm::CellBuilder::do_create_pruned_branch(source_cell, pb_level);
      if (pb_cell.is_null()) {
        return td::Status::Error("BOC decompression failed: failed to create pruned branch from state");
      }
      nodes[idx] = std::move(pb_cell);
      return td::Status::OK();
    } catch (const vm::CellBuilder::CellWriteError&) {
      return td::Status::Error("BOC decompression failed: failed to create pruned branch from state (CellWriteError)");
    }
  };

  const unsigned kNoNode = std::numeric_limits<unsigned>::max();

  // Recursive rebuild of the left subtree of a MerkleUpdate by mirroring the provided state tree.
  const auto build_left_under_mu = [&](auto&& self, unsigned left_idx, td::Ref<vm::Cell> state_cell) -> td::Status {
    if (state_cell.is_null()) {
      return td::Status::Error("BOC decompression failed: missing state subtree for MerkleUpdate left branch");
    }
    if (nodes[left_idx].not_null()) {
      return td::Status::OK();
    }
    bool is_prunned_branch = pb_level_mask[left_idx] != 0;

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

    for (unsigned j = 0; j < cell_refs_cnt[left_idx]; ++j) {
      td::Ref<vm::Cell> child_state = state_slice.prefetch_ref(j);
      TRY_STATUS(self(self, boc_graph[left_idx][j], child_state));
    }

    vm::CellBuilder cb;
    cb.store_bits(state_slice.as_bitslice());
    TRY_STATUS(finalize_node_from_builder(left_idx, cb));
    return td::Status::OK();
  };

  // Recursive build of right subtree under MerkleUpdate, pairing with the left subtree and computing sum diffs.
  const auto build_right_under_mu = [&](auto&& self, unsigned right_idx, unsigned left_idx,
                                        td::RefInt256* sum_diff_out) -> td::Status {
    if (left_idx != kNoNode && nodes[left_idx].is_null()) {
      return td::Status::Error("BOC decompression failed: missing reconstructed left node under MerkleUpdate");
    }
    if (nodes[right_idx].not_null()) {
      if (left_idx != kNoNode && sum_diff_out) {
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
    td::RefInt256 sum_child_diff = td::make_refint(0);
    for (unsigned j = 0; j < cell_refs_cnt[right_idx]; ++j) {
      unsigned right_child = boc_graph[right_idx][j];
      unsigned left_child = (left_idx != kNoNode && j < cell_refs_cnt[left_idx]) ? boc_graph[left_idx][j] : kNoNode;
      TRY_STATUS(self(self, right_child, left_child, &sum_child_diff));
    }
    // If this vertex was depth-balance-compressed, reconstruct its data from left + children sum
    if (is_depth_balance[right_idx]) {
      if (left_idx == kNoNode) {
        return td::Status::Error("BOC decompression failed: depth-balance left vertex has no grams");
      }
      vm::CellSlice cs_left(NoVm(), nodes[left_idx]);
      td::RefInt256 left_grams = extract_balance_from_depth_balance_info(cs_left);
      if (left_grams.is_null()) {
        return td::Status::Error("BOC decompression failed: depth-balance left vertex has no grams");
      }
      td::RefInt256 expected_right_grams = left_grams;
      expected_right_grams += sum_child_diff;

      vm::CellBuilder cb;
      if (!write_depth_balance_grams(cb, expected_right_grams)) {
        return td::Status::Error("BOC decompression failed: failed to write depth-balance grams");
      }
      cur_right_left_diff = sum_child_diff;
      TRY_STATUS(finalize_node_from_builder(right_idx, cb));
    } else {
      TRY_STATUS(finalize_node(right_idx));
    }

    if (cur_right_left_diff.is_null() && left_idx != kNoNode) {
      vm::CellSlice cs_left(NoVm(), nodes[left_idx]);
      vm::CellSlice cs_right(NoVm(), nodes[right_idx]);
      cur_right_left_diff = process_shard_accounts_vertex(cs_left, cs_right);
    }
    if (sum_diff_out && cur_right_left_diff.not_null()) {
      *sum_diff_out += cur_right_left_diff;
    }
    return td::Status::OK();
  };

  // General recursive build that handles the single MerkleUpdate node (if present) by pairing left/right subtrees.
  const auto build_node = [&](auto&& self, unsigned idx, const unsigned main_mu_cell_idx) -> td::Status {
    if (nodes[idx].not_null()) {
      return td::Status::OK();
    }

    const bool is_mu_node =
        (idx == main_mu_cell_idx) &&
        is_merkle_update_node(is_special[idx], pb_level_mask[idx], cell_data_prefix[idx], cell_data_suffix[idx]);
    if (is_mu_node) {
      if (cell_refs_cnt[idx] != 2) {
        return td::Status::Error("BOC decompression failed: MerkleUpdate node expected to have 2 references");
      }
      const unsigned left_idx = boc_graph[idx][0];
      const unsigned right_idx = boc_graph[idx][1];

      if (decompress_merkle_update) {
        TRY_STATUS(build_left_under_mu(build_left_under_mu, left_idx, state));
      } else {
        TRY_STATUS(self(self, left_idx, kNoNode));
      }
      TRY_STATUS(build_right_under_mu(build_right_under_mu, right_idx, left_idx, nullptr));
      TRY_STATUS(finalize_node(idx));
      return td::Status::OK();
    }

    for (unsigned j = 0; j < cell_refs_cnt[idx]; ++j) {
      TRY_STATUS(self(self, boc_graph[idx][j], main_mu_cell_idx));
    }
    TRY_STATUS(finalize_node(idx));
    return td::Status::OK();
  };

  // Build from roots using recursive DFS (depth is pre-validated by the DP guard above).
  for (unsigned root_index : root_indexes) {
    unsigned main_mu_cell_idx = kNoNode;
    if (cell_refs_cnt[root_index] > kMUCellOrderInRoot) {
      main_mu_cell_idx = boc_graph[root_index][kMUCellOrderInRoot];
    }
    TRY_STATUS(build_node(build_node, root_index, main_mu_cell_idx));
  }

  std::vector<td::Ref<vm::Cell>> root_nodes;
  root_nodes.reserve(root_count);
  for (unsigned index : root_indexes) {
    root_nodes.push_back(nodes[index]);
  }

  return root_nodes;
}

td::Result<td::BufferSlice> boc_compress(const std::vector<td::Ref<vm::Cell>>& boc_roots, CompressionAlgorithm algo,
                                         td::Ref<vm::Cell> state) {
  // Check for empty input
  if (boc_roots.empty()) {
    return td::Status::Error("Cannot compress empty BOC roots");
  }

  td::BufferSlice compressed;
  if (algo == CompressionAlgorithm::BaselineLZ4) {
    TRY_RESULT_ASSIGN(compressed, boc_compress_baseline_lz4(boc_roots));
  } else if (algo == CompressionAlgorithm::ImprovedStructureLZ4) {
    TRY_RESULT_ASSIGN(compressed, boc_compress_improved_structure_lz4(boc_roots, false));
  } else if (algo == CompressionAlgorithm::ImprovedStructureLZ4WithState) {
    TRY_RESULT_ASSIGN(compressed, boc_compress_improved_structure_lz4(boc_roots, true, state));
  } else {
    return td::Status::Error("Unknown compression algorithm");
  }

  td::BufferSlice compressed_with_algo(compressed.size() + 1);
  compressed_with_algo.data()[0] = static_cast<char>(algo);
  memcpy(compressed_with_algo.data() + 1, compressed.data(), compressed.size());
  return compressed_with_algo;
}

td::Result<std::vector<td::Ref<vm::Cell>>> boc_decompress(td::Slice compressed, int max_decompressed_size,
                                                          td::Ref<vm::Cell> state) {
  if (compressed.size() == 0) {
    return td::Status::Error("Can't decompress empty data");
  }
  if (max_decompressed_size <= 0) {
    return td::Status::Error("BOC decompression failed: invalid decompressed size");
  }

  unsigned max_decompressed_size_uns = static_cast<unsigned>(max_decompressed_size);

  CompressionAlgorithm algo = static_cast<CompressionAlgorithm>(compressed[0]);
  compressed.remove_prefix(1);

  switch (algo) {
    case CompressionAlgorithm::BaselineLZ4:
      return boc_decompress_baseline_lz4(compressed, max_decompressed_size_uns);
    case CompressionAlgorithm::ImprovedStructureLZ4:
      return boc_decompress_improved_structure_lz4(compressed, max_decompressed_size_uns, false);
    case CompressionAlgorithm::ImprovedStructureLZ4WithState:
      return boc_decompress_improved_structure_lz4(compressed, max_decompressed_size_uns, true, state);
  }
  return td::Status::Error("Unknown compression algorithm");
}

td::Result<bool> boc_need_state_for_decompression(const td::Slice& compressed) {
  if (compressed.size() == 0) {
    return td::Status::Error("Can't check algorithm on empty data");
  }

  CompressionAlgorithm algo = static_cast<CompressionAlgorithm>(compressed[0]);

  switch (algo) {
    case CompressionAlgorithm::BaselineLZ4:
    case CompressionAlgorithm::ImprovedStructureLZ4:
      return false;
    case CompressionAlgorithm::ImprovedStructureLZ4WithState:
      return true;
    default:
      return td::Status::Error("Unknown compression algorithm");
  }
}

std::string compression_algorithm_to_str(CompressionAlgorithm algo) {
  switch (algo) {
    case CompressionAlgorithm::BaselineLZ4:
      return "BaselineLZ4";
    case CompressionAlgorithm::ImprovedStructureLZ4:
      return "ImprovedStructureLZ4";
    case CompressionAlgorithm::ImprovedStructureLZ4WithState:
      return "ImprovedStructureLZ4WithState";
    default:
      return "Unknown";
  }
}

td::Result<std::string> boc_get_algorithm_name(const td::Slice& compressed) {
  if (compressed.size() == 0) {
    return td::Status::Error("Can't get algorithm name from empty data");
  }

  CompressionAlgorithm algo = static_cast<CompressionAlgorithm>(compressed[0]);

  switch (algo) {
    case CompressionAlgorithm::BaselineLZ4:
    case CompressionAlgorithm::ImprovedStructureLZ4:
    case CompressionAlgorithm::ImprovedStructureLZ4WithState:
      return compression_algorithm_to_str(algo);
    default:
      return td::Status::Error("Unknown compression algorithm");
  }
}

}  // namespace vm
