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
  TRY_RESULT(roots, vm::std_boc_deserialize_multi(decompressed, vm::BagOfCells::default_max_roots, true));
  return roots;
}

namespace {

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

// Single-use operation state; each public compression call creates a fresh instance.
class ImprovedStructureCompressor {
 public:
  td::Result<td::BufferSlice> compress(const std::vector<td::Ref<vm::Cell>>& boc_roots) {
    // Input validation
    if (boc_roots.empty()) {
      return td::Status::Error("No root cells were provided for serialization");
    }
    for (const auto& root : boc_roots) {
      if (root.is_null()) {
        return td::Status::Error("Cannot serialize a null cell reference into a bag of cells");
      }
    }

    TRY_STATUS(import_roots(boc_roots));
    prepare_graph();
    TRY_STATUS(topological_sort());
    write_header_and_metadata();
    write_direct_edges();
    write_data_prefixes();
    write_remaining_edges();
    pad_to_byte_boundary();
    write_data_suffixes();
    pad_to_byte_boundary();
    return compress_serialized();
  }

 private:
  static constexpr bool kMURemoveSubtreeSums = true;

  td::HashMap<vm::Cell::Hash, unsigned> cell_hashes_;
  std::vector<std::array<unsigned, 4>> boc_graph_;
  std::vector<unsigned> refs_cnt_;
  std::vector<td::BitSlice> cell_data_;
  std::vector<unsigned> cell_type_;
  std::vector<unsigned> pb_level_mask_;
  std::vector<unsigned> root_indexes_;
  unsigned total_size_estimate_ = 0;
  unsigned node_count_ = 0;
  std::vector<std::vector<unsigned>> reverse_graph_;
  unsigned edge_count_ = 0;
  std::vector<unsigned> is_data_small_;
  std::vector<unsigned> topo_order_;
  std::vector<unsigned> rank_;
  td::BitString result_;

  td::Result<unsigned> build_graph(td::Ref<vm::Cell> cell, const td::Ref<vm::Cell>& main_mu_cell,
                                   bool under_mu_right = false, td::Ref<vm::Cell> left_cell = td::Ref<vm::Cell>(),
                                   td::RefInt256* sum_diff_out = nullptr) {
    if (cell.is_null()) {
      return td::Status::Error("Error while importing a cell during serialization: cell is null");
    }

    auto cell_hash = cell->get_hash();
    auto it = cell_hashes_.find(cell_hash);
    if (it != cell_hashes_.end()) {
      // The graph node is stored only once, but balance aggregation is per edge.
      // A repeated right node must still contribute its diff against the left
      // node paired with this particular occurrence.
      if (under_mu_right && left_cell.not_null() && sum_diff_out) {
        vm::CellSlice cs_left(NoVm(), left_cell);
        vm::CellSlice cs_right(NoVm(), cell);
        td::RefInt256 vertex_diff = process_shard_accounts_vertex(cs_left, cs_right);
        if (vertex_diff.not_null()) {
          *sum_diff_out += vertex_diff;
        }
      }
      return it->second;
    }

    unsigned current_cell_id = td::narrow_cast<unsigned>(boc_graph_.size());
    cell_hashes_.emplace(cell_hash, current_cell_id);

    bool is_special = false;
    vm::CellSlice cell_slice = vm::load_cell_slice_special(cell, is_special);
    if (!cell_slice.is_valid()) {
      return td::Status::Error("Invalid loaded cell data");
    }
    td::BitSlice cell_bitslice = cell_slice.as_bitslice();

    // Initialize new cell in graph
    boc_graph_.emplace_back();
    refs_cnt_.emplace_back(cell_slice.size_refs());
    cell_type_.emplace_back(static_cast<unsigned>(cell_slice.special_type()));
    pb_level_mask_.push_back(0);

    DCHECK(cell_slice.size_refs() <= 4);

    // Process special cell of type PrunnedBranch
    if (cell_slice.special_type() == vm::CellTraits::SpecialType::PrunnedBranch) {
      DCHECK(cell_slice.size() >= 16);
      cell_data_.emplace_back(cell_bitslice.subslice(16, cell_bitslice.size() - 16));
      pb_level_mask_.back() = cell_slice.data()[1];
    } else {
      cell_data_.emplace_back(cell_bitslice);
    }

    total_size_estimate_ += cell_bitslice.size();

    // Process cell references
    if (kMURemoveSubtreeSums && cell_slice.special_type() == vm::CellTraits::SpecialType::MerkleUpdate &&
        main_mu_cell.not_null() && cell_hash == main_mu_cell->get_hash()) {
      // Left branch: traverse normally
      TRY_RESULT(child_left_id, build_graph(cell_slice.prefetch_ref(0), main_mu_cell));
      boc_graph_[current_cell_id][0] = child_left_id;
      // Right branch: traverse paired with left and compute diffs inline
      TRY_RESULT(child_right_id,
                 build_graph(cell_slice.prefetch_ref(1), main_mu_cell, true, cell_slice.prefetch_ref(0)));
      boc_graph_[current_cell_id][1] = child_right_id;
    } else if (under_mu_right && left_cell.not_null()) {
      // Inline computation for RIGHT subtree nodes under MerkleUpdate
      vm::CellSlice cs_left(NoVm(), left_cell);
      td::RefInt256 sum_child_diff = td::make_refint(0);
      // Recurse children first
      for (unsigned i = 0; i < cell_slice.size_refs(); ++i) {
        TRY_RESULT(child_id, build_graph(cell_slice.prefetch_ref(i), main_mu_cell, true, cs_left.prefetch_ref(i),
                                         &sum_child_diff));
        boc_graph_[current_cell_id][i] = child_id;
      }

      // Compute this vertex diff and check skippable condition
      td::RefInt256 vertex_diff = process_shard_accounts_vertex(cs_left, cell_slice);
      if (!is_special && vertex_diff.not_null() && sum_child_diff.not_null() && cmp(sum_child_diff, vertex_diff) == 0) {
        cell_data_[current_cell_id] = td::BitSlice();
        pb_level_mask_[current_cell_id] = 9;
      }
      if (sum_diff_out && vertex_diff.not_null()) {
        *sum_diff_out += vertex_diff;
      }
    } else {
      for (unsigned i = 0; i < cell_slice.size_refs(); ++i) {
        TRY_RESULT(child_id, build_graph(cell_slice.prefetch_ref(i), main_mu_cell, under_mu_right));
        boc_graph_[current_cell_id][i] = child_id;
      }
    }

    return current_cell_id;
  }

  td::Status import_roots(const std::vector<td::Ref<vm::Cell>>& boc_roots) {
    // Build the graph starting from roots
    for (auto root : boc_roots) {
      td::Ref<vm::Cell> main_mu_cell;
      bool root_is_special;
      vm::CellSlice root_slice = vm::load_cell_slice_special(root, root_is_special);
      if (root_slice.is_valid() && root_slice.size_refs() > kMUCellOrderInRoot) {
        main_mu_cell = root_slice.prefetch_ref(kMUCellOrderInRoot);
      }

      TRY_RESULT(root_cell_id, build_graph(root, main_mu_cell));
      root_indexes_.push_back(root_cell_id);
    }

    return td::Status::OK();
  }

  void prepare_graph() {
    // Check graph properties
    node_count_ = td::narrow_cast<unsigned>(boc_graph_.size());
    reverse_graph_.resize(node_count_);
    edge_count_ = 0;

    // Build reverse graph
    for (unsigned i = 0; i < node_count_; ++i) {
      for (unsigned child_index = 0; child_index < refs_cnt_[i]; ++child_index) {
        unsigned child = boc_graph_[i][child_index];
        ++edge_count_;
        reverse_graph_[child].push_back(i);
      }
    }

    // Process cell data sizes
    is_data_small_.resize(node_count_, 0);
    for (unsigned i = 0; i < node_count_; ++i) {
      if (cell_type_[i] != 1) {
        is_data_small_[i] = cell_data_[i].size() < 128;
      }
    }

    rank_.resize(node_count_);
  }

  td::Status topological_sort() {
    std::vector<unsigned> queue;
    queue.reserve(node_count_);
    std::vector<unsigned> in_degree(node_count_);

    // Calculate in-degrees and initialize queue
    for (unsigned i = 0; i < node_count_; ++i) {
      in_degree[i] = refs_cnt_[i];
      if (in_degree[i] == 0) {
        queue.push_back(i);
      }
    }

    if (queue.empty()) {
      return td::Status::Error("Cycle detected in cell references");
    }

    std::sort(queue.begin(), queue.end(), [&](unsigned lhs, unsigned rhs) {
      bool lhs_is_ordinary = cell_type_[lhs] == 0;
      bool rhs_is_ordinary = cell_type_[rhs] == 0;
      if (lhs_is_ordinary != rhs_is_ordinary) {
        return lhs_is_ordinary;
      }
      if (cell_data_[lhs].size() != cell_data_[rhs].size()) {
        return cell_data_[lhs].size() < cell_data_[rhs].size();
      }
      return lhs < rhs;
    });
    std::reverse(queue.begin(), queue.end());

    // Process queue
    while (!queue.empty()) {
      unsigned node = queue.back();
      queue.pop_back();
      topo_order_.push_back(node);

      for (unsigned parent : reverse_graph_[node]) {
        if (--in_degree[parent] == 0) {
          queue.push_back(parent);
        }
      }
    }

    if (topo_order_.size() != node_count_) {
      return td::Status::Error("Invalid graph structure");
    }

    std::reverse(topo_order_.begin(), topo_order_.end());
    return td::Status::OK();
  }

  void write_header_and_metadata() {
    // Calculate index of vertices in topsort
    for (unsigned i = 0; i < node_count_; ++i) {
      rank_[topo_order_[i]] = i;
    }

    // Build compressed representation
    total_size_estimate_ += node_count_ * 10u * 8u;
    result_.reserve_bits(total_size_estimate_);

    // Store roots information
    const unsigned root_count = td::narrow_cast<unsigned>(root_indexes_.size());
    append_uint(result_, root_count, 32);
    for (unsigned root_ind : root_indexes_) {
      append_uint(result_, rank_[root_ind], 32);
    }

    // Store node count
    append_uint(result_, node_count_, 32);

    // Store cell types and sizes
    for (unsigned i = 0; i < node_count_; ++i) {
      unsigned node = topo_order_[i];
      unsigned current_cell_type = bool(cell_type_[node]) + pb_level_mask_[node];
      append_uint(result_, current_cell_type, 4);
      unsigned current_refs_cnt = refs_cnt_[node];
      if (cell_type_[node] == 1 && cell_data_[node].size() == 0) {
        DCHECK(current_refs_cnt == 0);
        current_refs_cnt = 1;
      }
      append_uint(result_, current_refs_cnt, 4);

      if (cell_type_[node] != 1 && current_cell_type != 9) {
        if (is_data_small_[node]) {
          append_uint(result_, 1, 1);
          append_uint(result_, cell_data_[node].size(), 7);
        } else {
          append_uint(result_, 0, 1);
          append_uint(result_, 1 + cell_data_[node].size() / 8, 7);
        }
      }
    }
  }

  void write_direct_edges() {
    // Store edge information
    auto edge_bits = result_.reserve_bitslice(edge_count_).bits();
    for (unsigned i = 0; i < node_count_; ++i) {
      unsigned node = topo_order_[i];
      for (unsigned child_index = 0; child_index < refs_cnt_[node]; ++child_index) {
        unsigned child = boc_graph_[node][child_index];
        edge_bits.store_uint(rank_[child] == i + 1, 1);
        ++edge_bits;
      }
    }
  }

  void write_data_prefixes() {
    // Store cell data
    for (unsigned node : topo_order_) {
      if (pb_level_mask_[node] == 9) {
        continue;
      }
      if (cell_type_[node] != 1 && !is_data_small_[node]) {
        continue;
      }
      result_.append(cell_data_[node].subslice(0, cell_data_[node].size() % 8));
    }
  }

  void write_remaining_edges() {
    // Store BOC graph with optimized encoding
    for (unsigned i = 0; i < node_count_; ++i) {
      unsigned node = topo_order_[i];
      if (node_count_ <= i + 3) {
        continue;
      }

      for (unsigned j = 0; j < refs_cnt_[node]; ++j) {
        if (rank_[boc_graph_[node][j]] <= i + 1) {
          continue;
        }

        unsigned delta = rank_[boc_graph_[node][j]] - i - 2;  // Always >= 0 because of above check
        unsigned remaining_nodes = node_count_ - i - 3;       // Always > 0 because of above node count check
        unsigned required_bits = 32u - td::count_leading_zeroes32(remaining_nodes);

        if (required_bits < 8 - (result_.size() + 1) % 8 + 1) {
          append_uint(result_, delta, required_bits);
        } else if (delta < (1u << (8 - (result_.size() + 1) % 8))) {
          unsigned available_bits = 8 - (result_.size() + 1) % 8;
          append_uint(result_, 1, 1);
          append_uint(result_, delta, available_bits);
        } else {
          append_uint(result_, 0, 1);
          append_uint(result_, delta, required_bits);
        }
      }
    }
  }

  void pad_to_byte_boundary() {
    while (result_.size() % 8) {
      append_uint(result_, 0, 1);
    }
  }

  void write_data_suffixes() {
    // Store remaining cell data
    for (unsigned node : topo_order_) {
      if (pb_level_mask_[node] == 9) {
        continue;
      }
      if (cell_type_[node] == 1 || is_data_small_[node]) {
        unsigned prefix_size = cell_data_[node].size() % 8;
        result_.append(cell_data_[node].subslice(prefix_size, cell_data_[node].size() - prefix_size));
      } else {
        unsigned data_size = cell_data_[node].size() + 1;
        unsigned padding = (8 - data_size % 8) % 8;

        if (padding) {
          append_uint(result_, 0, padding);
        }
        append_uint(result_, 1, 1);
        result_.append(cell_data_[node]);
      }
    }
  }

  td::Result<td::BufferSlice> compress_serialized() {
    // Create final compressed buffer
    td::BufferSlice serialized((const char*)result_.bits().get_byte_ptr(), result_.size() / 8);

    td::BufferSlice compressed = td::lz4_compress(serialized);

    // Add decompressed size at the beginning
    td::BufferSlice compressed_with_size(compressed.size() + kDecompressedSizeBytes);
    auto size_slice = td::BitSliceWrite(compressed_with_size.as_slice().ubegin(), kDecompressedSizeBytes * 8);
    size_slice.bits().store_uint(serialized.size(), kDecompressedSizeBytes * 8);
    memcpy(compressed_with_size.data() + kDecompressedSizeBytes, compressed.data(), compressed.size());

    return compressed_with_size;
  }
};

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

// Single-use operation state; the owning buffer outlives all decoded bit views.
class ImprovedStructureDecompressor {
 public:
  td::Result<std::vector<td::Ref<vm::Cell>>> decompress(td::Slice compressed, unsigned max_decompressed_size) {
    TRY_STATUS(decompress_frame(compressed, max_decompressed_size));
    TRY_STATUS(read_header());
    TRY_STATUS(read_metadata());
    TRY_STATUS(read_direct_edges());
    TRY_STATUS(read_prefixes());
    TRY_STATUS(read_remaining_edges());
    TRY_STATUS(read_padding());
    TRY_STATUS(read_suffixes());
    TRY_STATUS(validate_depth());
    return reconstruct_roots();
  }

 private:
  static constexpr unsigned kMaxCellDataLengthBits = 1024;
  static constexpr unsigned kMaxNodeCount = (1u << 20);
  static constexpr unsigned kNoNode = std::numeric_limits<unsigned>::max();

  // The bit reader and cell-data views borrow from this buffer for the whole operation.
  td::BufferSlice serialized_;
  td::BitSlice bit_reader_;
  unsigned decompressed_size_ = 0;
  unsigned orig_size_ = 0;
  unsigned root_count_ = 0;
  unsigned node_count_ = 0;
  std::vector<unsigned> root_indexes_;
  std::vector<td::uint16> cell_data_length_;
  std::vector<td::uint8> is_data_small_;
  std::vector<td::uint8> is_special_;
  std::vector<td::uint8> cell_refs_cnt_;
  std::vector<td::uint8> is_depth_balance_;
  std::vector<td::uint8> pb_level_mask_;
  std::vector<td::BitSlice> cell_data_prefix_;
  std::vector<td::BitSlice> cell_data_suffix_;
  std::vector<std::array<td::uint32, 4>> boc_graph_;
  std::vector<td::uint16> node_depth_;
  std::vector<td::Ref<vm::Cell>> nodes_;

  td::Status decompress_frame(td::Slice compressed, unsigned max_decompressed_size) {
    // Check minimum input size for decompressed size header
    if (compressed.size() < kDecompressedSizeBytes) {
      return td::Status::Error("BOC decompression failed: input too small for header");
    }

    // Read decompressed size
    constexpr unsigned kSizeBits = kDecompressedSizeBytes * 8;
    decompressed_size_ =
        td::narrow_cast<unsigned>(td::BitSlice(compressed.ubegin(), kSizeBits).bits().get_uint(kSizeBits));
    compressed.remove_prefix(kDecompressedSizeBytes);
    if (decompressed_size_ > max_decompressed_size) {
      return td::Status::Error("BOC decompression failed: invalid decompressed size");
    }

    // Decompress LZ4 data
    TRY_RESULT(decompressed_size_int, td::narrow_cast_safe<int>(decompressed_size_));
    TRY_RESULT_ASSIGN(serialized_, td::lz4_decompress(compressed, decompressed_size_int));

    if (serialized_.size() != decompressed_size_) {
      return td::Status::Error("BOC decompression failed: decompressed size mismatch");
    }

    // Initialize bit reader
    bit_reader_ =
        td::BitSlice(serialized_.as_slice().ubegin(), td::narrow_cast<unsigned>(serialized_.as_slice().size() * 8));
    orig_size_ = bit_reader_.size();
    return td::Status::OK();
  }

  td::Status read_header() {
    // Read root count
    TRY_RESULT_ASSIGN(root_count_, read_uint(bit_reader_, 32));
    if (root_count_ == 0 || root_count_ > BagOfCells::default_max_roots) {
      return td::Status::Error("BOC decompression failed: invalid root count");
    }
    // We assume that each root should take at least 1 byte in the decompressed payload.
    if (root_count_ > decompressed_size_) {
      return td::Status::Error("BOC decompression failed: invalid root count");
    }

    root_indexes_.resize(root_count_);
    for (unsigned i = 0; i < root_count_; ++i) {
      TRY_RESULT(root_index, read_uint(bit_reader_, 32));
      root_indexes_[i] = root_index;
    }

    // Read number of nodes from header
    TRY_RESULT_ASSIGN(node_count_, read_uint(bit_reader_, 32));
    if (node_count_ == 0) {
      return td::Status::Error("BOC decompression failed: invalid node count");
    }
    if (node_count_ > kMaxNodeCount) {
      return td::Status::Error("BOC decompression failed: invalid node count");
    }

    // We assume that each cell should take at least 1 byte, even effectively serialized
    // Otherwise it means that provided node_count is incorrect
    if (node_count_ > decompressed_size_) {
      return td::Status::Error("BOC decompression failed: incorrect node count provided");
    }

    // Validate root indexes
    for (unsigned i = 0; i < root_count_; ++i) {
      if (root_indexes_[i] >= node_count_) {
        return td::Status::Error("BOC decompression failed: invalid root index");
      }
    }

    // Initialize data structures
    cell_data_length_.resize(node_count_);
    is_data_small_.resize(node_count_, 0);
    is_special_.resize(node_count_, 0);
    cell_refs_cnt_.resize(node_count_, 0);
    is_depth_balance_.resize(node_count_, 0);
    pb_level_mask_.resize(node_count_, 0);

    // Cell data bits are stored as two views into the decompressed bitstream, and later fed into vm::CellBuilder to finalize the nodes.
    cell_data_prefix_.resize(node_count_);
    cell_data_suffix_.resize(node_count_);

    boc_graph_.resize(node_count_);
    for (auto& a : boc_graph_) {
      a.fill(0);
    }
    return td::Status::OK();
  }

  td::Status read_metadata() {
    // Read cell metadata
    for (unsigned i = 0; i < node_count_; ++i) {
      // Check enough bits for cell type and refs count
      if (bit_reader_.size() < 8) {
        return td::Status::Error("BOC decompression failed: not enough bits for cell metadata");
      }

      unsigned cell_type = td::narrow_cast<unsigned>(bit_reader_.bits().get_uint(4));
      is_special_[i] = (cell_type == 9 ? 0 : static_cast<td::uint8>(bool(cell_type)));
      is_depth_balance_[i] = (cell_type == 9);
      if (is_special_[i]) {
        pb_level_mask_[i] = static_cast<td::uint8>(cell_type - 1);
      }
      bit_reader_.advance(4);

      cell_refs_cnt_[i] = static_cast<td::uint8>(bit_reader_.bits().get_uint(4));
      bit_reader_.advance(4);
      if (cell_refs_cnt_[i] > 4) {
        return td::Status::Error("BOC decompression failed: invalid cell refs count");
      }
      if (is_depth_balance_[i]) {
        cell_data_length_[i] = 0;
      } else if (pb_level_mask_[i]) {
        unsigned coef = td::count_bits32(pb_level_mask_[i]);
        cell_data_length_[i] = static_cast<td::uint16>((256 + 16) * coef);
        if (cell_refs_cnt_[i] == 1) {
          cell_refs_cnt_[i] = 0;
          cell_data_length_[i] = 0;
        } else if (cell_refs_cnt_[i] > 1) {
          return td::Status::Error("BOC decompression failed: pruned branch cannot have references");
        }
      } else {
        // Check enough bits for data length metadata
        if (bit_reader_.size() < 8) {
          return td::Status::Error("BOC decompression failed: not enough bits for data length");
        }

        is_data_small_[i] = static_cast<td::uint8>(bit_reader_.bits().get_uint(1));
        bit_reader_.advance(1);
        cell_data_length_[i] = static_cast<td::uint16>(bit_reader_.bits().get_uint(7));
        bit_reader_.advance(7);

        if (!is_data_small_[i]) {
          cell_data_length_[i] = static_cast<td::uint16>(cell_data_length_[i] * 8);
          if (!cell_data_length_[i]) {
            cell_data_length_[i] = static_cast<td::uint16>(cell_data_length_[i] + 1024);
          }
        }
      }

      // Validate cell data length
      if (cell_data_length_[i] > kMaxCellDataLengthBits) {
        return td::Status::Error("BOC decompression failed: invalid cell data length");
      }
    }
    return td::Status::OK();
  }

  td::Status read_direct_edges() {
    // Read direct edge connections
    for (unsigned i = 0; i < node_count_; ++i) {
      for (unsigned j = 0; j < cell_refs_cnt_[i]; ++j) {
        TRY_RESULT(edge_connection, read_uint(bit_reader_, 1));
        if (edge_connection) {
          boc_graph_[i][j] = i + 1;
        }
      }
    }
    return td::Status::OK();
  }

  td::Status read_prefixes() {
    // Read initial cell data
    for (unsigned i = 0; i < node_count_; ++i) {
      if (is_depth_balance_[i]) {
        continue;
      }
      unsigned remainder_bits = cell_data_length_[i] % 8;
      if (bit_reader_.size() < remainder_bits) {
        return td::Status::Error("BOC decompression failed: not enough bits for initial cell data");
      }
      cell_data_prefix_[i] = bit_reader_.subslice(0, remainder_bits);
      bit_reader_.advance(remainder_bits);
      cell_data_length_[i] = static_cast<td::uint16>(cell_data_length_[i] - remainder_bits);
    }
    return td::Status::OK();
  }

  td::Status read_remaining_edges() {
    // Decode remaining edge connections
    for (unsigned i = 0; i < node_count_; ++i) {
      if (node_count_ <= i + 3) {
        for (unsigned j = 0; j < cell_refs_cnt_[i]; ++j) {
          if (!boc_graph_[i][j]) {
            boc_graph_[i][j] = i + 2;
          }
        }
        continue;
      }

      for (unsigned j = 0; j < cell_refs_cnt_[i]; ++j) {
        if (!boc_graph_[i][j]) {
          unsigned pref_size = orig_size_ - bit_reader_.size();
          unsigned remaining_nodes = node_count_ - i - 3;  // Always > 0 because of above node count check
          unsigned required_bits = 32u - td::count_leading_zeroes32(remaining_nodes);

          if (required_bits < 8 - (pref_size + 1) % 8 + 1) {
            TRY_RESULT_ASSIGN(boc_graph_[i][j], read_uint(bit_reader_, required_bits));
            boc_graph_[i][j] += i + 2;
          } else {
            TRY_RESULT(edge_connection, read_uint(bit_reader_, 1));
            if (edge_connection) {
              pref_size = orig_size_ - bit_reader_.size();
              unsigned available_bits = 8 - pref_size % 8;
              TRY_RESULT_ASSIGN(boc_graph_[i][j], read_uint(bit_reader_, available_bits));
              boc_graph_[i][j] += i + 2;
            } else {
              TRY_RESULT_ASSIGN(boc_graph_[i][j], read_uint(bit_reader_, required_bits));
              boc_graph_[i][j] += i + 2;
            }
          }
        }
      }
    }

    // Check if all graph connections are valid
    for (unsigned node = 0; node < node_count_; ++node) {
      for (unsigned j = 0; j < cell_refs_cnt_[node]; ++j) {
        unsigned child_node = boc_graph_[node][j];
        if (child_node >= node_count_) {
          return td::Status::Error("BOC decompression failed: invalid graph connection");
        }
        if (child_node <= node) {
          return td::Status::Error("BOC decompression failed: circular reference in graph");
        }
      }
    }
    return td::Status::OK();
  }

  td::Status read_padding() {
    // Align to byte boundary
    while ((orig_size_ - bit_reader_.size()) % 8) {
      TRY_RESULT(bit, read_uint(bit_reader_, 1));
      if (bit != 0) {
        return td::Status::Error("BOC decompression failed: non-zero padding bits");
      }
    }
    return td::Status::OK();
  }

  td::Status read_suffixes() {
    // Read remaining cell data
    for (unsigned i = 0; i < node_count_; ++i) {
      if (is_depth_balance_[i]) {
        continue;
      }
      unsigned padding_bits = 0;
      if (!pb_level_mask_[i] && !is_data_small_[i]) {
        while (bit_reader_.size() > 0 && bit_reader_.bits()[0] == 0) {
          ++padding_bits;
          bit_reader_.advance(1);
        }
        TRY_STATUS(read_uint(bit_reader_, 1));
        ++padding_bits;
      }
      if (cell_data_length_[i] < padding_bits) {
        return td::Status::Error("BOC decompression failed: invalid cell data length");
      }
      unsigned remaining_data_bits = cell_data_length_[i] - padding_bits;
      if (bit_reader_.size() < remaining_data_bits) {
        return td::Status::Error("BOC decompression failed: not enough bits for remaining cell data");
      }

      cell_data_suffix_[i] = bit_reader_.subslice(0, remaining_data_bits);
      bit_reader_.advance(remaining_data_bits);
    }

    // Strict end-of-stream: allow only final zero padding to byte boundary.
    if (bit_reader_.size() > 7) {
      return td::Status::Error("BOC decompression failed: trailing unused data");
    }
    while (bit_reader_.size() > 0) {
      TRY_RESULT(bit, read_uint(bit_reader_, 1));
      if (bit != 0) {
        return td::Status::Error("BOC decompression failed: trailing unused data");
      }
    }
    return td::Status::OK();
  }

  td::Status validate_depth() {
    // Early depth guard: reject graphs that can't fit CellTraits::max_depth before reconstruction.
    // The graph invariant child > parent guarantees reverse-order DP.
    node_depth_.resize(node_count_, 0);
    for (unsigned remaining_nodes = node_count_; remaining_nodes > 0; --remaining_nodes) {
      unsigned node = remaining_nodes - 1;
      td::uint16 max_child_depth = 0;
      for (unsigned j = 0; j < cell_refs_cnt_[node]; ++j) {
        max_child_depth = std::max(max_child_depth, node_depth_[boc_graph_[node][j]]);
      }
      if (cell_refs_cnt_[node] != 0) {
        if (max_child_depth >= CellTraits::max_depth) {
          return td::Status::Error("BOC decompression failed: cell depth too large");
        }
        node_depth_[node] = static_cast<td::uint16>(max_child_depth + 1);
      }
    }
    return td::Status::OK();
  }

  td::Status finalize_node_from_builder(unsigned idx, vm::CellBuilder& cb) {
    try {
      for (unsigned j = 0; j < cell_refs_cnt_[idx]; ++j) {
        cb.store_ref(nodes_[boc_graph_[idx][j]]);
      }
      try {
        nodes_[idx] = cb.finalize(is_special_[idx] != 0);
      } catch (vm::CellBuilder::CellWriteError&) {
        return td::Status::Error(PSTRING() << "BOC decompression failed: failed to finalize node (CellWriteError)");
      }
    } catch (vm::VmError&) {
      return td::Status::Error(PSTRING() << "BOC decompression failed: failed to finalize node (VmError)");
    }
    return td::Status::OK();
  }

  // Default finalize: reconstruct builder from stored bit-slices and finalize.
  td::Status finalize_node(unsigned idx) {
    if (is_depth_balance_[idx]) {
      return td::Status::Error("BOC decompression failed: depth-balance node must be reconstructed under MerkleUpdate");
    }
    vm::CellBuilder cb;
    if (pb_level_mask_[idx]) {
      cb.store_long((1 << 8) + pb_level_mask_[idx], 16);
    }
    cb.store_bits(cell_data_prefix_[idx]);
    cb.store_bits(cell_data_suffix_[idx]);
    return finalize_node_from_builder(idx, cb);
  }

  // Recursive build of right subtree under MerkleUpdate, pairing with the left subtree and computing sum diffs.
  td::Status build_right_under_mu(unsigned right_idx, unsigned left_idx, td::RefInt256* sum_diff_out) {
    if (left_idx != kNoNode && nodes_[left_idx].is_null()) {
      return td::Status::Error("BOC decompression failed: missing reconstructed left node under MerkleUpdate");
    }
    if (nodes_[right_idx].not_null()) {
      if (left_idx != kNoNode && sum_diff_out) {
        vm::CellSlice cs_left(NoVm(), nodes_[left_idx]);
        vm::CellSlice cs_right(NoVm(), nodes_[right_idx]);
        td::RefInt256 vertex_diff = process_shard_accounts_vertex(cs_left, cs_right);
        if (vertex_diff.not_null()) {
          *sum_diff_out += vertex_diff;
        }
      }
      return td::Status::OK();
    }
    td::RefInt256 cur_right_left_diff;
    td::RefInt256 sum_child_diff = td::make_refint(0);
    for (unsigned j = 0; j < cell_refs_cnt_[right_idx]; ++j) {
      unsigned right_child = boc_graph_[right_idx][j];
      unsigned left_child = (left_idx != kNoNode && j < cell_refs_cnt_[left_idx]) ? boc_graph_[left_idx][j] : kNoNode;
      TRY_STATUS(build_right_under_mu(right_child, left_child, &sum_child_diff));
    }
    // If this vertex was depth-balance-compressed, reconstruct its data from left + children sum
    if (is_depth_balance_[right_idx]) {
      if (left_idx == kNoNode) {
        return td::Status::Error("BOC decompression failed: depth-balance left vertex has no grams");
      }
      vm::CellSlice cs_left(NoVm(), nodes_[left_idx]);
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
      vm::CellSlice cs_left(NoVm(), nodes_[left_idx]);
      vm::CellSlice cs_right(NoVm(), nodes_[right_idx]);
      cur_right_left_diff = process_shard_accounts_vertex(cs_left, cs_right);
    }
    if (sum_diff_out && cur_right_left_diff.not_null()) {
      *sum_diff_out += cur_right_left_diff;
    }
    return td::Status::OK();
  }

  // General recursive build that handles the single MerkleUpdate node (if present) by pairing left/right subtrees.
  td::Status build_node(unsigned idx, const unsigned main_mu_cell_idx) {
    if (nodes_[idx].not_null()) {
      return td::Status::OK();
    }

    const bool is_mu_node =
        (idx == main_mu_cell_idx) &&
        is_merkle_update_node(is_special_[idx], pb_level_mask_[idx], cell_data_prefix_[idx], cell_data_suffix_[idx]);
    if (is_mu_node) {
      if (cell_refs_cnt_[idx] != 2) {
        return td::Status::Error("BOC decompression failed: MerkleUpdate node expected to have 2 references");
      }
      const unsigned left_idx = boc_graph_[idx][0];
      const unsigned right_idx = boc_graph_[idx][1];

      TRY_STATUS(build_node(left_idx, kNoNode));
      TRY_STATUS(build_right_under_mu(right_idx, left_idx, nullptr));
      TRY_STATUS(finalize_node(idx));
      return td::Status::OK();
    }

    for (unsigned j = 0; j < cell_refs_cnt_[idx]; ++j) {
      TRY_STATUS(build_node(boc_graph_[idx][j], main_mu_cell_idx));
    }
    TRY_STATUS(finalize_node(idx));
    return td::Status::OK();
  }

  td::Result<std::vector<td::Ref<vm::Cell>>> reconstruct_roots() {
    // Build cell tree
    nodes_.resize(node_count_);

    // Build from roots using recursive DFS (depth is pre-validated by the DP guard above).
    for (unsigned root_index : root_indexes_) {
      unsigned main_mu_cell_idx = kNoNode;
      if (cell_refs_cnt_[root_index] > kMUCellOrderInRoot) {
        main_mu_cell_idx = boc_graph_[root_index][kMUCellOrderInRoot];
      }
      TRY_STATUS(build_node(root_index, main_mu_cell_idx));
    }

    std::vector<td::Ref<vm::Cell>> root_nodes;
    root_nodes.reserve(root_count_);
    for (unsigned index : root_indexes_) {
      root_nodes.push_back(nodes_[index]);
    }

    return root_nodes;
  }
};

}  // namespace

td::Result<td::BufferSlice> boc_compress(const std::vector<td::Ref<vm::Cell>>& boc_roots, CompressionAlgorithm algo) {
  // Check for empty input
  if (boc_roots.empty()) {
    return td::Status::Error("Cannot compress empty BOC roots");
  }

  td::BufferSlice compressed;
  if (algo == CompressionAlgorithm::BaselineLZ4) {
    TRY_RESULT_ASSIGN(compressed, boc_compress_baseline_lz4(boc_roots));
  } else if (algo == CompressionAlgorithm::ImprovedStructureLZ4) {
    TRY_RESULT_ASSIGN(compressed, ImprovedStructureCompressor().compress(boc_roots));
  } else {
    return td::Status::Error("Unknown compression algorithm");
  }

  td::BufferSlice compressed_with_algo(compressed.size() + 1);
  compressed_with_algo.data()[0] = static_cast<char>(algo);
  memcpy(compressed_with_algo.data() + 1, compressed.data(), compressed.size());
  return compressed_with_algo;
}

td::Result<std::vector<td::Ref<vm::Cell>>> boc_decompress(td::Slice compressed, int max_decompressed_size) {
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
      return ImprovedStructureDecompressor().decompress(compressed, max_decompressed_size_uns);
  }
  return td::Status::Error("Unknown compression algorithm");
}

std::string compression_algorithm_to_str(CompressionAlgorithm algo) {
  switch (algo) {
    case CompressionAlgorithm::BaselineLZ4:
      return "BaselineLZ4";
    case CompressionAlgorithm::ImprovedStructureLZ4:
      return "ImprovedStructureLZ4";
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
      return compression_algorithm_to_str(algo);
    default:
      return td::Status::Error("Unknown compression algorithm");
  }
}

}  // namespace vm
