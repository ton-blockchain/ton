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
*/

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "block/block-parse.h"
#include "common/refint.h"
#include "td/utils/Slice.h"
#include "td/utils/crypto.h"
#include "td/utils/lz4.h"
#include "td/utils/misc.h"
#include "td/utils/tests.h"
#include "vm/boc-compression.h"
#include "vm/boc.h"
#include "vm/cells.h"
#include "vm/cells/CellBuilder.h"
#include "vm/cells/CellSlice.h"
#include "vm/cells/CellUsageTree.h"
#include "vm/cells/MerkleUpdate.h"
#include "vm/cells/UsageCell.h"

// MerkleUpdate balance optimization.

namespace vm {
namespace {

constexpr int kMaxDecompressedSize = 20 << 20;

Ref<Cell> make_plain_cell(std::uint32_t tag, std::vector<Ref<Cell>> refs = {}) {
  CellBuilder cb;
  CHECK(cb.store_long_bool(tag, 32));
  for (auto& ref : refs) {
    CHECK(cb.store_ref_bool(std::move(ref)));
  }
  return cb.finalize();
}

// This intentionally constructs only the bit pattern used by the compression
// optimization. It is contract-controlled data, not a claim that the cell is a
// semantically valid ShardAccounts vertex.
Ref<Cell> make_balance_shaped_cell(std::uint64_t grams, std::vector<Ref<Cell>> refs = {}) {
  CellBuilder cb;
  CHECK(cb.store_zeroes_bool(7));  // empty HmLabel followed by split_depth = 0
  CHECK(block::tlb::t_CurrencyCollection.pack_special(cb, td::make_refint(grams), Ref<Cell>()));
  for (auto& ref : refs) {
    CHECK(cb.store_ref_bool(std::move(ref)));
  }
  return cb.finalize();
}

Ref<Cell> make_large_balance_shaped_cell(td::RefInt256 grams, std::vector<Ref<Cell>> refs = {}) {
  CellBuilder cb;
  CHECK(cb.store_zeroes_bool(7));
  CHECK(block::tlb::t_CurrencyCollection.pack_special(cb, std::move(grams), Ref<Cell>()));
  for (auto& ref : refs) {
    CHECK(cb.store_ref_bool(std::move(ref)));
  }
  return cb.finalize();
}

// Encode a one-byte Grams value in two bytes with a leading zero.
// VarUInteger::as_integer_skip() rejects this noncanonical representation, so
// the optimizer must leave the duck-typed lookalike untouched.
Ref<Cell> make_noncanonical_balance_shaped_cell(std::uint64_t grams, std::vector<Ref<Cell>> refs = {}) {
  CHECK(grams <= 0xff);
  CellBuilder cb;
  CHECK(cb.store_zeroes_bool(7));
  CHECK(cb.store_long_bool(2, 4));
  CHECK(cb.store_long_bool(grams, 16));
  CHECK(cb.store_zeroes_bool(1));
  for (auto& ref : refs) {
    CHECK(cb.store_ref_bool(std::move(ref)));
  }
  return cb.finalize();
}

Ref<Cell> make_nonempty_label_balance_shaped_cell(std::uint64_t grams, std::vector<Ref<Cell>> refs = {}) {
  CellBuilder cb;
  CHECK(cb.store_long_bool(1, 2));  // not the empty HmLabel prefix '00'
  CHECK(cb.store_zeroes_bool(5));
  CHECK(block::tlb::t_CurrencyCollection.pack_special(cb, td::make_refint(grams), Ref<Cell>()));
  for (auto& ref : refs) {
    CHECK(cb.store_ref_bool(std::move(ref)));
  }
  return cb.finalize();
}

Ref<Cell> make_nonzero_split_depth_balance_shaped_cell(std::uint64_t grams, std::vector<Ref<Cell>> refs = {}) {
  CellBuilder cb;
  CHECK(cb.store_zeroes_bool(2));
  CHECK(cb.store_long_bool(1, 5));
  CHECK(block::tlb::t_CurrencyCollection.pack_special(cb, td::make_refint(grams), Ref<Cell>()));
  for (auto& ref : refs) {
    CHECK(cb.store_ref_bool(std::move(ref)));
  }
  return cb.finalize();
}

Ref<Cell> make_balance_shaped_cell_with_trailing_bit(std::uint64_t grams, std::vector<Ref<Cell>> refs = {}) {
  CellBuilder cb;
  CHECK(cb.store_zeroes_bool(7));
  CHECK(block::tlb::t_CurrencyCollection.pack_special(cb, td::make_refint(grams), Ref<Cell>()));
  CHECK(cb.store_long_bool(1, 1));
  for (auto& ref : refs) {
    CHECK(cb.store_ref_bool(std::move(ref)));
  }
  return cb.finalize();
}

Ref<Cell> make_balance_shaped_cell_with_extra_currency(std::uint64_t grams, Ref<Cell> extra,
                                                       std::vector<Ref<Cell>> refs = {}) {
  CellBuilder cb;
  CHECK(cb.store_zeroes_bool(7));
  CHECK(block::tlb::t_CurrencyCollection.pack_special(cb, td::make_refint(grams), std::move(extra)));
  for (auto& ref : refs) {
    CHECK(cb.store_ref_bool(std::move(ref)));
  }
  return cb.finalize();
}

Ref<Cell> make_block_root(Ref<Cell> merkle_update, Ref<Cell> first_block_ref = {}) {
  if (first_block_ref.is_null()) {
    first_block_ref = make_plain_cell(1);
  }
  CellBuilder cb;
  CHECK(cb.store_long_bool(0x11ef55aau, 32));
  CHECK(cb.store_ref_bool(std::move(first_block_ref)));
  CHECK(cb.store_ref_bool(make_plain_cell(2)));
  CHECK(cb.store_ref_bool(std::move(merkle_update)));  // kMUCellOrderInRoot == 2
  CHECK(cb.store_ref_bool(make_plain_cell(3)));
  return cb.finalize();
}

Ref<Cell> make_block_with_state_update(Ref<Cell> old_payload, Ref<Cell> new_payload, Ref<Cell> first_block_ref = {}) {
  Ref<Cell> old_state = std::move(old_payload);
  Ref<Cell> new_state = std::move(new_payload);
  auto stable = make_plain_cell(0xc0ffee00u, {make_plain_cell(0x70000000u)});
  old_state = make_plain_cell(0x51000000u, {old_state, stable});
  new_state = make_plain_cell(0x51000000u, {new_state, stable});

  // A MerkleUpdate is allowed to carry the complete old and new trees. Keeping
  // them unpruned makes the cell pairing under test explicit and independent
  // from CellUsageTree pruning decisions; a separate test below covers the
  // production-style generated/pruned form.
  Ref<Cell> merkle_update = CellBuilder::create_merkle_update(old_state, new_state);
  MerkleUpdate::may_apply(old_state, merkle_update).ensure();
  MerkleUpdate::validate(merkle_update).ensure();
  auto applied = MerkleUpdate::apply(old_state, merkle_update).move_as_ok();
  CHECK(applied->get_hash() == new_state->get_hash());

  return make_block_root(std::move(merkle_update), std::move(first_block_ref));
}

std::uint32_t read_u32_be(td::Slice data, std::size_t offset) {
  CHECK(offset + 4 <= data.size());
  const auto* ptr = data.ubegin() + offset;
  return (static_cast<std::uint32_t>(ptr[0]) << 24) | (static_cast<std::uint32_t>(ptr[1]) << 16) |
         (static_cast<std::uint32_t>(ptr[2]) << 8) | static_cast<std::uint32_t>(ptr[3]);
}

// Parse only the metadata prefix of ImprovedStructureLZ4. Cell type 9 is the
// internal marker saying that the cell data was omitted and must be rebuilt
// from the paired left cell and child deltas.
unsigned count_balance_elision_markers(td::Slice payload) {
  CHECK(payload.size() >= 5);
  auto algorithm = static_cast<CompressionAlgorithm>(static_cast<unsigned char>(payload[0]));
  CHECK(algorithm == CompressionAlgorithm::ImprovedStructureLZ4);

  payload.remove_prefix(1);
  auto decompressed_size = read_u32_be(payload, 0);
  CHECK(decompressed_size > 0);
  payload.remove_prefix(kDecompressedSizeBytes);
  auto decompressed = td::lz4_decompress(payload, static_cast<int>(decompressed_size)).move_as_ok();
  td::Slice metadata = decompressed.as_slice();

  std::size_t offset = 0;
  auto root_count = read_u32_be(metadata, offset);
  offset += 4;
  CHECK(root_count <= (metadata.size() - offset) / 4);
  offset += static_cast<std::size_t>(root_count) * 4;
  auto node_count = read_u32_be(metadata, offset);
  offset += 4;

  unsigned result = 0;
  for (std::uint32_t i = 0; i < node_count; ++i) {
    CHECK(offset < metadata.size());
    unsigned cell_type = metadata.ubegin()[offset] >> 4;
    ++offset;  // cell type and reference count occupy one byte together
    if (cell_type == 9) {
      ++result;
    } else if (cell_type <= 1) {
      // Ordinary cells and non-pruned exotic cells have one more byte of data-length metadata.
      CHECK(offset < metadata.size());
      ++offset;
    }
  }
  return result;
}

std::string structure_hash(const std::vector<Ref<Cell>>& roots) {
  auto compressed = boc_compress(roots, CompressionAlgorithm::ImprovedStructureLZ4).move_as_ok();
  auto size = read_u32_be(compressed.as_slice(), 1);
  auto raw =
      td::lz4_decompress(compressed.as_slice().substr(1 + kDecompressedSizeBytes), static_cast<int>(size)).move_as_ok();
  return td::hex_encode(td::sha256(raw.as_slice()));
}

struct RoundTripResult {
  bool ok{false};
  unsigned balance_elision_markers{0};
  std::string error;
};

RoundTripResult round_trip(const std::vector<Ref<Cell>>& roots) {
  RoundTripResult result;
  auto compressed_result = boc_compress(roots, CompressionAlgorithm::ImprovedStructureLZ4);
  if (compressed_result.is_error()) {
    result.error = compressed_result.move_as_error().to_string();
    return result;
  }
  auto compressed = compressed_result.move_as_ok();
  result.balance_elision_markers = count_balance_elision_markers(compressed.as_slice());

  auto decompressed_result = boc_decompress(compressed.as_slice(), kMaxDecompressedSize);
  if (decompressed_result.is_error()) {
    result.error = decompressed_result.move_as_error().to_string();
    return result;
  }
  auto decompressed = decompressed_result.move_as_ok();
  if (decompressed.size() != roots.size()) {
    result.error = "root count changed";
    return result;
  }
  for (std::size_t i = 0; i < roots.size(); ++i) {
    if (decompressed[i].is_null() || decompressed[i]->get_hash() != roots[i]->get_hash()) {
      result.error = "root hash or order changed";
      return result;
    }
  }
  // Hashes alone are not enough for a refactoring regression oracle: compare
  // the complete canonical BOC, including exotic data and every reference.
  auto before = std_boc_serialize_multi(roots, 31).move_as_ok();
  auto after = std_boc_serialize_multi(decompressed, 31).move_as_ok();
  if (before.as_slice() != after.as_slice()) {
    result.error = "canonical BOC changed";
    return result;
  }
  result.ok = true;
  return result;
}

bool expect_round_trip(const char* scenario, const Ref<Cell>& block, unsigned expected_elisions) {
  auto result = round_trip(std::vector<Ref<Cell>>{block});
  bool success = result.ok && result.balance_elision_markers == expected_elisions;
  if (!success) {
    LOG(ERROR) << scenario << " round_trip=" << result.ok << " balance_elisions=" << result.balance_elision_markers
               << " expected_elisions=" << expected_elisions
               << (result.error.empty() ? "" : PSTRING() << " error=" << result.error);
  }
  return success;
}

}  // namespace

TEST(BocCompression, DuplicateEdgesUseFullMultiplicity) {
  struct Case {
    const char* name;
    std::uint64_t child_old;
    std::uint64_t child_new;
    std::uint64_t parent_old;
    std::uint64_t parent_new;
    unsigned multiplicity;
    unsigned expected_elisions;
  };
  const std::array<Case, 5> cases{{
      {"two edges, false positive elision", 0, 5, 100, 105, 2, 0},
      {"two edges, valid positive elision", 0, 5, 100, 110, 2, 1},
      {"two edges, false negative elision", 10, 5, 100, 95, 2, 0},
      {"two edges, valid negative elision across Grams byte boundary", 10, 5, 260, 250, 2, 1},
      {"maximum four-edge multiplicity", 0, 5, 100, 120, 4, 1},
  }};

  bool success = true;
  for (const auto& test : cases) {
    auto tail = make_plain_cell(0x77000000u);
    auto old_child = make_balance_shaped_cell(test.child_old, {tail});
    auto new_child = make_balance_shaped_cell(test.child_new, {tail});

    std::vector<Ref<Cell>> old_refs(test.multiplicity, old_child);
    std::vector<Ref<Cell>> new_refs(test.multiplicity, new_child);
    auto old_parent = make_balance_shaped_cell(test.parent_old, std::move(old_refs));
    auto new_parent = make_balance_shaped_cell(test.parent_new, std::move(new_refs));
    auto block = make_block_with_state_update(std::move(old_parent), std::move(new_parent));
    success &= expect_round_trip(test.name, block, test.expected_elisions);
  }
  ASSERT_TRUE(success);
}

TEST(BocCompression, SharedRightCellUsesEachLeftOccurrence) {
  auto tail = make_plain_cell(0x77000003u);
  auto old_child1 = make_balance_shaped_cell(0, {tail});
  auto old_child2 = make_balance_shaped_cell(2, {tail});
  auto new_shared_child = make_balance_shaped_cell(5, {tail});

  // Deltas are 5 and 3, so the correct edge sum is 8. A parent delta of 5
  // must remain explicit; counting only the first occurrence would elide it.
  auto false_old_parent = make_balance_shaped_cell(100, {old_child1, old_child2});
  auto false_new_parent = make_balance_shaped_cell(105, {new_shared_child, new_shared_child});
  auto false_block = make_block_with_state_update(std::move(false_old_parent), std::move(false_new_parent));

  // With parent delta 8, elision is valid and exercises recomputation of the
  // same right cell against a different left cell on the second edge.
  auto valid_old_parent = make_balance_shaped_cell(100, {old_child1, old_child2});
  auto valid_new_parent = make_balance_shaped_cell(108, {new_shared_child, new_shared_child});
  auto valid_block = make_block_with_state_update(std::move(valid_old_parent), std::move(valid_new_parent));

  bool success = expect_round_trip("different-left false-elision guard", false_block, 0);
  success &= expect_round_trip("different-left genuine elision", valid_block, 1);
  ASSERT_TRUE(success);
}

TEST(BocCompression, SharedRightCellUsesSameFirstPairingOnDecompression) {
  auto run_order = [](bool elidable_pair_first) {
    auto tail = make_plain_cell(0x7700000fu);
    auto old_child_a = make_balance_shaped_cell(1, {tail});
    auto old_child_b = make_balance_shaped_cell(4, {tail});
    auto new_shared_child = make_balance_shaped_cell(6, {tail});

    auto left_a = make_balance_shaped_cell(10, {old_child_a});
    auto left_b = make_balance_shaped_cell(20, {old_child_b});
    auto shared_right = make_balance_shaped_cell(15, {new_shared_child});

    // shared_right is reconstructible when paired with left_a: both its own
    // balance delta and its child's balance delta are 5. It is not
    // reconstructible when paired with left_b: those deltas are -5 and 2.
    // The compressor therefore emits type 9 only when the A pairing is the
    // first occurrence of shared_right. The decompressor must encounter that
    // same pairing first or it would reconstruct shared_right incorrectly.
    Ref<Cell> old_parent;
    if (elidable_pair_first) {
      old_parent = make_balance_shaped_cell(100, {left_a, left_b});
    } else {
      old_parent = make_balance_shaped_cell(100, {left_b, left_a});
    }
    auto new_parent = make_balance_shaped_cell(101, {shared_right, shared_right});
    return make_block_with_state_update(std::move(old_parent), std::move(new_parent));
  };

  bool success = expect_round_trip("shared right cell, elidable pairing first", run_order(true), 1);
  success &= expect_round_trip("shared right cell, non-elidable pairing first", run_order(false), 0);
  ASSERT_TRUE(success);
}

TEST(BocCompression, EqualSubtreesAcrossParentsAreOrderIndependent) {
  auto run_order = [](bool valid_parent_first) {
    auto tail = make_plain_cell(0x77000004u);
    auto old_child1 = make_balance_shaped_cell(0, {tail});
    auto old_child2 = make_balance_shaped_cell(0, {tail});
    auto new_child1 = make_balance_shaped_cell(5, {tail});
    auto new_child2 = make_balance_shaped_cell(5, {tail});
    CHECK(old_child1.get() != old_child2.get());
    CHECK(new_child1.get() != new_child2.get());
    CHECK(old_child1->get_hash() == old_child2->get_hash());
    CHECK(new_child1->get_hash() == new_child2->get_hash());

    auto valid_old_parent = make_balance_shaped_cell(100, {old_child1});
    auto valid_new_parent = make_balance_shaped_cell(105, {new_child1});
    auto invalid_old_parent = make_balance_shaped_cell(200, {old_child2});
    auto invalid_new_parent = make_balance_shaped_cell(200, {new_child2});

    Ref<Cell> old_payload;
    Ref<Cell> new_payload;
    if (valid_parent_first) {
      old_payload = make_plain_cell(0x44440000u, {valid_old_parent, invalid_old_parent});
      new_payload = make_plain_cell(0x44440000u, {valid_new_parent, invalid_new_parent});
    } else {
      old_payload = make_plain_cell(0x44440000u, {invalid_old_parent, valid_old_parent});
      new_payload = make_plain_cell(0x44440000u, {invalid_new_parent, valid_new_parent});
    }
    return make_block_with_state_update(std::move(old_payload), std::move(new_payload));
  };

  bool success = expect_round_trip("shared equal-hash subtree, valid parent first", run_order(true), 1);
  success &= expect_round_trip("shared equal-hash subtree, valid parent second", run_order(false), 1);
  ASSERT_TRUE(success);
}

TEST(BocCompression, SharedLeftCellCanSplitIntoDistinctRightCells) {
  auto tail = make_plain_cell(0x7700000eu);
  auto old_shared_child = make_balance_shaped_cell(10, {tail});
  auto new_child1 = make_balance_shaped_cell(13, {tail});
  auto new_child2 = make_balance_shaped_cell(17, {tail});

  auto old_parent = make_balance_shaped_cell(100, {old_shared_child, old_shared_child});
  auto new_parent = make_balance_shaped_cell(110, {new_child1, new_child2});
  auto block = make_block_with_state_update(std::move(old_parent), std::move(new_parent));
  ASSERT_TRUE(expect_round_trip("one old subtree split into two distinct new subtrees", block, 1));
}

TEST(BocCompression, PrevisitedCellStillContributesInsideMerkleUpdate) {
  auto tail = make_plain_cell(0x77000005u);
  auto old_child = make_balance_shaped_cell(0, {tail});
  auto new_child = make_balance_shaped_cell(5, {tail});
  auto old_parent = make_balance_shaped_cell(100, {old_child});
  auto new_parent = make_balance_shaped_cell(100, {new_child});

  // refs[0] is traversed before refs[2] (the MerkleUpdate), so new_child is
  // already present in the compressor's global hash table when the state
  // traversal reaches it.
  auto block = make_block_with_state_update(std::move(old_parent), std::move(new_parent), new_child);
  ASSERT_TRUE(expect_round_trip("cell previsited by an earlier block reference", block, 0));
}

TEST(BocCompression, CellSeenInMerkleUpdateLeftStillContributesOnRight) {
  auto tail = make_plain_cell(0x7700000du);
  auto old_paired_child = make_balance_shaped_cell(0, {tail});
  auto cell_seen_on_left = make_balance_shaped_cell(5, {tail});

  auto old_parent = make_balance_shaped_cell(100, {old_paired_child});
  auto new_parent = make_balance_shaped_cell(100, {cell_seen_on_left});

  // The first branch is unchanged, so cell_seen_on_left is inserted into the
  // global cell table during the complete left-MerkleUpdate traversal. The
  // second branch then uses the same right cell paired with a different old
  // child. Its edge delta is 5 and must not disappear because of the earlier
  // visit.
  auto old_payload = make_plain_cell(0x55550000u, {cell_seen_on_left, old_parent});
  auto new_payload = make_plain_cell(0x55550000u, {cell_seen_on_left, new_parent});
  auto block = make_block_with_state_update(std::move(old_payload), std::move(new_payload));
  ASSERT_TRUE(expect_round_trip("right cell already present in MerkleUpdate left tree", block, 0));
}

TEST(BocCompression, UnpairedDuplicateRightEdgeDoesNotContribute) {
  auto tail = make_plain_cell(0x77000006u);
  auto old_child = make_balance_shaped_cell(0, {tail});
  auto new_child = make_balance_shaped_cell(5, {tail});
  auto old_parent = make_balance_shaped_cell(100, {old_child});
  auto new_parent = make_balance_shaped_cell(105, {new_child, new_child});
  auto block = make_block_with_state_update(std::move(old_parent), std::move(new_parent));

  // Only the first right edge has a corresponding left edge. The second use of
  // new_child must not reuse or invent a balance delta without a left partner.
  ASSERT_TRUE(expect_round_trip("duplicate right edge without a left partner", block, 1));
}

TEST(BocCompression, NestedElidedSharedChildCountsEveryEdge) {
  auto tail = make_plain_cell(0x77000007u);
  auto old_grandchild = make_balance_shaped_cell(0, {tail});
  auto new_grandchild = make_balance_shaped_cell(5, {tail});
  auto old_child = make_balance_shaped_cell(10, {old_grandchild});
  auto new_child = make_balance_shaped_cell(15, {new_grandchild});
  auto old_parent = make_balance_shaped_cell(100, {old_child, old_child});
  auto new_parent = make_balance_shaped_cell(110, {new_child, new_child});
  auto block = make_block_with_state_update(std::move(old_parent), std::move(new_parent));

  // new_child is itself elided, and both references to it are needed to elide
  // new_parent. This prevents a zero delta from masking a missed revisit.
  ASSERT_TRUE(expect_round_trip("nested elision with a shared nonzero-delta child", block, 2));
}

TEST(BocCompression, ZeroBalanceDeltaIsNotMissing) {
  auto old_tail = make_plain_cell(0x77000008u);
  auto new_tail = make_plain_cell(0x77000009u);
  auto old_root = make_balance_shaped_cell(5, {old_tail});
  auto new_root = make_balance_shaped_cell(5, {new_tail});
  auto block = make_block_with_state_update(std::move(old_root), std::move(new_root));

  // A parsed zero is a real balance delta, not the null RefInt256 used for an
  // unrecognized cell. The root is therefore reconstructible and is elided.
  ASSERT_TRUE(expect_round_trip("zero balance delta", block, 1));
}

TEST(BocCompression, DistinctChildrenAllowGenuineElision) {
  auto tail1 = make_plain_cell(0x7700000au);
  auto tail2 = make_plain_cell(0x7700000bu);
  auto old_child1 = make_balance_shaped_cell(10, {tail1});
  auto new_child1 = make_balance_shaped_cell(13, {tail1});
  auto old_child2 = make_balance_shaped_cell(20, {tail2});
  auto new_child2 = make_balance_shaped_cell(27, {tail2});

  auto old_parent = make_balance_shaped_cell(255, {old_child1, old_child2});
  auto new_parent = make_balance_shaped_cell(265, {new_child1, new_child2});
  auto block = make_block_with_state_update(std::move(old_parent), std::move(new_parent));
  ASSERT_TRUE(expect_round_trip("two distinct child deltas across Grams byte boundary", block, 1));
}

TEST(BocCompression, OnlyExactlyReconstructibleBalanceEncodingIsElided) {
  using CellFactory = Ref<Cell> (*)(std::uint64_t, std::vector<Ref<Cell>>);
  struct Case {
    const char* name;
    CellFactory factory;
  };
  const std::array<Case, 4> invalid_encodings{{
      {"noncanonical Grams", make_noncanonical_balance_shaped_cell},
      {"nonempty HmLabel", make_nonempty_label_balance_shaped_cell},
      {"nonzero split depth", make_nonzero_split_depth_balance_shaped_cell},
      {"trailing data bit", make_balance_shaped_cell_with_trailing_bit},
  }};

  bool success = true;
  for (const auto& test : invalid_encodings) {
    auto tail = make_plain_cell(0x7700000cu);
    auto old_child = make_balance_shaped_cell(0, {tail});
    auto new_child = make_balance_shaped_cell(5, {tail});
    auto old_parent = test.factory(100, {old_child});
    auto new_parent = test.factory(105, {new_child});
    auto block = make_block_with_state_update(std::move(old_parent), std::move(new_parent));
    success &= expect_round_trip(test.name, block, 0);
  }

  auto extra = make_plain_cell(0xec000001u);
  auto extra_case_tail = make_plain_cell(0x7700000du);
  auto old_child = make_balance_shaped_cell(0, {extra_case_tail});
  auto new_child = make_balance_shaped_cell(5, {extra_case_tail});
  auto old_parent = make_balance_shaped_cell_with_extra_currency(100, extra, {old_child});
  auto new_parent = make_balance_shaped_cell_with_extra_currency(105, extra, {new_child});
  auto block = make_block_with_state_update(std::move(old_parent), std::move(new_parent));
  success &= expect_round_trip("nonempty extra-currency dictionary", block, 0);
  ASSERT_TRUE(success);
}

TEST(BocCompression, GeneratedMerkleUpdateWithReusedStatePath) {
  // This is the production-style form of the regression. The new tree keeps a
  // usage-wrapped cell from the old tree, allowing MerkleUpdate::generate() to
  // prune unchanged state while retaining the corresponding old path needed by
  // balance-delta reconstruction.
  auto tail = make_plain_cell(0x7e7e0000u, {make_plain_cell(0xdeadbeefu)});
  auto old_child = make_balance_shaped_cell(0, {tail});
  auto old_parent = make_balance_shaped_cell(100, {old_child, old_child});
  auto old_state = make_plain_cell(0x51a1e000u, {old_parent});

  auto usage_tree = std::make_shared<CellUsageTree>(old_state);
  Ref<Cell> usage_root = UsageCell::create(old_state, usage_tree->root_ptr());
  CellSlice state_slice(NoVm(), usage_root);
  CellSlice parent_slice(NoVm(), state_slice.prefetch_ref(0));
  CellSlice child_slice(NoVm(), parent_slice.prefetch_ref(0));
  Ref<Cell> reused_tail = child_slice.prefetch_ref(0);

  auto new_child = make_balance_shaped_cell(5, {reused_tail});
  auto new_parent = make_balance_shaped_cell(105, {new_child, new_child});
  auto new_state = make_plain_cell(0x51a1e000u, {new_parent});
  auto merkle_update = MerkleUpdate::generate(old_state, new_state, usage_tree.get()).move_as_ok();
  MerkleUpdate::may_apply(old_state, merkle_update).ensure();
  MerkleUpdate::validate(merkle_update).ensure();
  ASSERT_EQ(MerkleUpdate::apply(old_state, merkle_update).move_as_ok()->get_hash(), new_state->get_hash());

  auto block = make_block_root(merkle_update);
  ASSERT_TRUE(expect_round_trip("generated MerkleUpdate with reused contract-state path", block, 0));
}

TEST(BocCompression, BalanceElisionAcrossAllGramsByteBoundaries) {
  // Grams is VarUInteger 16: at most 15 bytes, not a uint64_t. Exercise
  // positive and negative deltas at every byte boundary through 2^120 - 1.
  for (int bytes = 1; bytes <= 15; ++bytes) {
    for (const auto& value : {(td::make_refint(1) << (bytes * 8)) - 1, td::make_refint(1) << ((bytes - 1) * 8)}) {
      for (bool decreasing : {false, true}) {
        auto old_value = decreasing ? value : td::make_refint(0);
        auto new_value = decreasing ? td::make_refint(0) : value;
        auto tail = make_plain_cell(0x120b17ffu);
        auto old_child = make_large_balance_shaped_cell(old_value, {tail});
        auto new_child = make_large_balance_shaped_cell(new_value, {tail});
        auto old_parent = make_large_balance_shaped_cell(old_value, {old_child});
        auto new_parent = make_large_balance_shaped_cell(new_value, {new_child});
        auto block = make_block_with_state_update(old_parent, new_parent);
        ASSERT_TRUE(expect_round_trip("Grams byte boundary", block, 1));
      }
    }
  }
}

TEST(BocCompression, BalanceElisionWithUnitDeltaAcrossGramsByteBoundaries) {
  // Adjacent nonzero values require a carry or borrow across every encoded
  // Grams byte boundary, while the child and parent deltas remain just +/-1.
  for (int bytes = 1; bytes < 15; ++bytes) {
    auto high = td::make_refint(1) << (bytes * 8);
    auto low = high - 1;
    for (bool decreasing : {false, true}) {
      auto old_value = decreasing ? high : low;
      auto new_value = decreasing ? low : high;
      auto tail = make_plain_cell(0x120b1800u);
      auto old_child = make_large_balance_shaped_cell(old_value, {tail});
      auto new_child = make_large_balance_shaped_cell(new_value, {tail});
      auto old_parent = make_large_balance_shaped_cell(old_value, {old_child});
      auto new_parent = make_large_balance_shaped_cell(new_value, {new_child});
      auto block = make_block_with_state_update(old_parent, new_parent);
      ASSERT_TRUE(expect_round_trip("unit delta crossing Grams byte boundary", block, 1));
    }
  }
}

TEST(BocCompression, NewUnpairedRightSubtreesStayExplicit) {
  auto tail = make_plain_cell(0xabc00300u);
  auto old_child = make_balance_shaped_cell(0, {tail});
  auto new_child = make_balance_shaped_cell(5, {tail});
  auto unpaired_tail = make_plain_cell(0xabc00301u);
  auto unpaired_child = make_balance_shaped_cell(7, {unpaired_tail});
  auto unpaired_subtree = make_balance_shaped_cell(23, {unpaired_child});
  auto old_parent = make_balance_shaped_cell(100, {old_child});
  auto new_parent = make_balance_shaped_cell(105, {new_child, unpaired_subtree});

  // Both unpaired vertices are first encountered on the right, unlike the
  // duplicate-right-edge case. Neither can contribute a delta or be elided.
  // The paired first child still makes the parent itself reconstructible.
  auto block = make_block_with_state_update(old_parent, new_parent);
  ASSERT_TRUE(expect_round_trip("new unpaired right subtree", block, 1));

  for (unsigned refs = 1; refs <= 2; ++refs) {
    std::vector<Ref<Cell>> right_refs{unpaired_subtree};
    if (refs == 2) {
      right_refs.push_back(new_child);
    }
    auto old_leaf = make_balance_shaped_cell(100);
    auto new_branch = make_balance_shaped_cell(100, std::move(right_refs));
    auto leaf_to_branch = make_block_with_state_update(old_leaf, new_branch);
    // No child has a left partner. The unchanged parent grams still permit
    // one elision, with all newly introduced children stored explicitly.
    ASSERT_TRUE(expect_round_trip("left leaf gains right references", leaf_to_branch, 1));
  }
}

TEST(BocCompression, RemovedLeftReferencesDoNotContributeBalanceDeltas) {
  auto tail = make_plain_cell(0xabc00400u);
  auto old_child = make_balance_shaped_cell(0, {tail});
  auto new_child = make_balance_shaped_cell(5, {tail});
  auto removed_child = make_balance_shaped_cell(77, {make_plain_cell(0xabc00401u)});
  auto old_parent = make_balance_shaped_cell(100, {old_child, removed_child});
  auto new_parent = make_balance_shaped_cell(105, {new_child});
  auto block = make_block_with_state_update(old_parent, new_parent);
  ASSERT_TRUE(expect_round_trip("right branch has fewer references", block, 1));

  // The optimization pairs only references retained on the right. With none,
  // the child-delta sum is zero even though the old vertex had two children.
  auto new_leaf = make_balance_shaped_cell(100);
  auto branch_to_leaf = make_block_with_state_update(old_parent, new_leaf);
  ASSERT_TRUE(expect_round_trip("right branch drops all references", branch_to_leaf, 1));
}

TEST(BocCompression, ChildDeltaSumCanExceedGramsRangeBeforeCancellation) {
  auto maximum = (td::make_refint(1) << 120) - 1;
  for (bool negative_first : {false, true}) {
    std::vector<Ref<Cell>> old_children, new_children;
    for (unsigned i = 0; i < 4; ++i) {
      auto tail = make_plain_cell(0x12000000u + i);
      bool decreasing = (i < 2) == negative_first;
      old_children.push_back(make_large_balance_shaped_cell(decreasing ? maximum : td::make_refint(0), {tail}));
      new_children.push_back(make_large_balance_shaped_cell(decreasing ? td::make_refint(0) : maximum, {tail}));
    }
    // The intermediate sum is +/-2*(2^120-1), but the final sum is zero.
    // Grams' unsigned 120-bit bound applies to the reconstructed balance,
    // not to the signed intermediate child-delta accumulator.
    auto block = make_block_with_state_update(make_balance_shaped_cell(1, std::move(old_children)),
                                              make_balance_shaped_cell(1, std::move(new_children)));
    ASSERT_TRUE(expect_round_trip("wide signed intermediate sum with cancellation", block, 1));
  }
}

TEST(BocCompression, MultipleRootsPreserveFirstMerkleUpdatePairing) {
  auto tail = make_plain_cell(0xabc00100u);
  auto old_child_a = make_balance_shaped_cell(1, {tail});
  auto old_child_b = make_balance_shaped_cell(4, {tail});
  auto new_child = make_balance_shaped_cell(6, {tail});
  auto old_a = make_balance_shaped_cell(10, {old_child_a});
  auto old_b = make_balance_shaped_cell(20, {old_child_b});
  auto shared_right = make_balance_shaped_cell(15, {new_child});
  auto block_a = make_block_with_state_update(old_a, shared_right);
  auto block_b = make_block_with_state_update(old_b, shared_right);

  for (bool a_first : {false, true}) {
    // A elides shared_right, B cannot. Hash deduplication spans roots, so
    // reconstruction must use the first pairing in the original root order.
    std::vector<Ref<Cell>> roots =
        a_first ? std::vector<Ref<Cell>>{block_a, block_b, block_a} : std::vector<Ref<Cell>>{block_b, block_a, block_b};
    auto result = round_trip(roots);
    ASSERT_TRUE(result.ok);
    ASSERT_EQ(result.balance_elision_markers, a_first ? 1u : 0u);
    // Captured from the pre-refactor implementation, in addition to the
    // independently constructed DAG and marker-count oracle above. Hash the
    // structure before LZ4 so library encoder choices do not affect the test.
    ASSERT_EQ(structure_hash(roots), a_first ? "13887419d5b2bd59ca495be596304121e958cd3f561fe64b1688f3453ee44cc1"
                                             : "d56bebb084ded3af91b55abcc0764898ecb6af09a6c00510b80feac9e9af277c");
  }

  // Visiting the right subtree as an earlier root keeps it explicit, just as
  // visiting it through an earlier block reference does.
  auto result = round_trip(std::vector<Ref<Cell>>{shared_right, block_a, block_b});
  ASSERT_TRUE(result.ok);
  ASSERT_EQ(result.balance_elision_markers, 0u);
}

TEST(BocCompression, OnlyRootThirdReferenceSelectsMerkleUpdate) {
  auto make_update = [](std::uint32_t tag) {
    auto tail = make_plain_cell(tag);
    auto old_child = make_balance_shaped_cell(0, {tail});
    auto new_child = make_balance_shaped_cell(5, {tail});
    auto old_parent = make_balance_shaped_cell(100, {old_child});
    auto new_parent = make_balance_shaped_cell(105, {new_child});
    return Ref<Cell>(CellBuilder::create_merkle_update(old_parent, new_parent));
  };
  auto update = make_update(0xabc00200u);
  for (unsigned slot = 0; slot < 4; ++slot) {
    std::vector<Ref<Cell>> refs{make_plain_cell(1), make_plain_cell(2), make_plain_cell(3), make_plain_cell(4)};
    refs[slot] = update;
    auto block = make_plain_cell(0x11ef55aau, std::move(refs));
    ASSERT_TRUE(expect_round_trip("MerkleUpdate root reference position", block, slot == 2 ? 1 : 0));
  }
  ASSERT_TRUE(expect_round_trip("MerkleUpdate itself as root", update, 0));
  ASSERT_TRUE(expect_round_trip("MerkleUpdate nested below ordinary third reference",
                                make_block_root(make_plain_cell(0xabcd, {update})), 0));
  ASSERT_TRUE(expect_round_trip("only the selected one of two MerkleUpdates is optimized",
                                make_block_root(make_update(0xabc00201u), update), 1));
}

}  // namespace vm

// Valid inputs, roundtrips, and wire compatibility.

namespace {

using Cell = td::Ref<vm::Cell>;
using Roots = std::vector<Cell>;
using Algorithm = vm::CompressionAlgorithm;
constexpr int kMaxSize = 16 << 20;
constexpr std::array<Algorithm, 2> kAlgorithms{Algorithm::BaselineLZ4, Algorithm::ImprovedStructureLZ4};

Cell bits_cell(unsigned bits, unsigned salt = 0, Roots refs = {}) {
  std::array<unsigned char, 128> bytes{};
  // Both nonzero data and unused final-byte bits vary, including the all-zero
  // salt used by the independent wire fixtures below.
  for (unsigned i = 0; i < bytes.size(); ++i) {
    bytes[i] = static_cast<unsigned char>(salt ? (salt * 37 + i * 71 + (salt >> (i % 13))) : 0);
  }
  vm::CellBuilder cb;
  CHECK(cb.store_bits_bool(bytes.data(), bits));
  for (const auto& ref : refs) {
    CHECK(cb.store_ref_bool(ref));
  }
  return cb.finalize_novm();
}

Cell tagged_cell(std::uint32_t tag, Roots refs = {}) {
  vm::CellBuilder cb;
  cb.store_long(tag, 32);
  for (const auto& ref : refs) {
    cb.store_ref(ref);
  }
  return cb.finalize_novm();
}

Cell pruned_cell(unsigned mask, unsigned depth = 0, unsigned salt = 0) {
  vm::CellBuilder cb;
  cb.store_long(1, 8).store_long(mask, 8);
  unsigned count = 0;
  for (unsigned bit = 0; bit < vm::Cell::max_level; ++bit) {
    count += (mask >> bit) & 1;
  }
  for (unsigned i = 0; i < count; ++i) {
    for (unsigned j = 0; j < vm::Cell::hash_bytes; ++j) {
      cb.store_long(salt ? (salt + 37 * i + j) & 255 : 0, 8);
    }
  }
  for (unsigned i = 0; i < count; ++i) {
    cb.store_long(depth + (salt ? i : 0), 16);
  }
  return cb.finalize_novm(true);
}

Cell library_cell() {
  return vm::CellBuilder{}.store_long(2, 8).store_zeroes(256).finalize_novm(true);
}

void assert_same_roots(const Roots& expected, const Roots& actual) {
  ASSERT_EQ(expected.size(), actual.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    ASSERT_EQ(expected[i]->get_level_mask().get_mask(), actual[i]->get_level_mask().get_mask());
    for (int level = 0; level <= vm::Cell::max_level; ++level) {
      ASSERT_EQ(expected[i]->get_hash(level), actual[i]->get_hash(level));
      ASSERT_EQ(expected[i]->get_depth(level), actual[i]->get_depth(level));
    }
  }
  // Canonical complete serialization covers all cell data, exotic flags,
  // reference ordering, shared descendants and duplicate/ordered roots. Mode 31
  // additionally includes stored hashes, depths, index/cache bits and CRC.
  for (int mode : {2, 31}) {
    auto expected_boc = vm::std_boc_serialize_multi(expected, mode).move_as_ok();
    auto actual_boc = vm::std_boc_serialize_multi(actual, mode).move_as_ok();
    ASSERT_EQ(expected_boc.as_slice(), actual_boc.as_slice());
  }
}

std::uint32_t read_be32(td::Slice data, std::size_t offset) {
  CHECK(offset + 4 <= data.size());
  const auto* bytes = data.ubegin() + offset;
  return (std::uint32_t(bytes[0]) << 24) | (std::uint32_t(bytes[1]) << 16) | (std::uint32_t(bytes[2]) << 8) |
         std::uint32_t(bytes[3]);
}

td::BufferSlice pre_lz4(td::Slice wrapped) {
  CHECK(wrapped.size() >= 5);
  unsigned size = read_be32(wrapped, 1);
  CHECK(size <= kMaxSize);
  return td::lz4_decompress(wrapped.substr(5), static_cast<int>(size)).move_as_ok();
}

std::string hex_bytes(td::Slice hex) {
  CHECK(hex.size() % 2 == 0);
  const auto digit = [](char ch) -> unsigned {
    if (ch >= '0' && ch <= '9') {
      return ch - '0';
    }
    CHECK(ch >= 'a' && ch <= 'f');
    return ch - 'a' + 10;
  };
  std::string bytes(hex.size() / 2, '\0');
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] = static_cast<char>((digit(hex[2 * i]) << 4) | digit(hex[2 * i + 1]));
  }
  return bytes;
}

void roundtrip(const Roots& roots) {
  for (auto algorithm : kAlgorithms) {
    auto compressed = vm::boc_compress(roots, algorithm).move_as_ok();
    ASSERT_EQ(static_cast<unsigned char>(compressed[0]), static_cast<unsigned>(algorithm));
    auto decoded = vm::boc_decompress(compressed, kMaxSize).move_as_ok();
    assert_same_roots(roots, decoded);

    // An inclusive exact advertised limit must also accept the payload.
    const auto exact_size = read_be32(compressed, 1);
    decoded = vm::boc_decompress(compressed, static_cast<int>(exact_size)).move_as_ok();
    assert_same_roots(roots, decoded);

    if (algorithm == Algorithm::BaselineLZ4) {
      auto direct = vm::boc_compress_baseline_lz4(roots).move_as_ok();
      ASSERT_EQ(compressed.as_slice().substr(1), direct.as_slice());
      auto direct_decoded = vm::boc_decompress_baseline_lz4(direct, exact_size).move_as_ok();
      assert_same_roots(roots, direct_decoded);
    }
    // A decode/encode cycle must preserve the full current wire representation.
    ASSERT_EQ(compressed.as_slice(), vm::boc_compress(decoded, algorithm).move_as_ok().as_slice());
  }
}

// These are hand-derived structure bytes, not output captured from the encoder.
// Each fixture contains the complete root table, metadata, edges and data; the
// tests compare before LZ4 because valid LZ4 encoder choices can change by version.
void structure_golden(const Roots& roots, const std::string& expected) {
  auto compressed = vm::boc_compress(roots, Algorithm::ImprovedStructureLZ4).move_as_ok();
  ASSERT_EQ(pre_lz4(compressed).as_slice(), td::Slice(expected));
  roundtrip(roots);
}

struct EdgeForms {
  std::size_t direct = 0;
  std::size_t implicit_tail = 0;
  std::size_t fixed = 0;
  std::size_t compact = 0;
  std::size_t extended = 0;
};

// Only scans the ordinary-cell structure header to prove that the graph
// fixtures reach all edge encodings. Actual correctness is checked against the
// independently created cell DAG and complete canonical BOC above.
void count_edge_forms(td::Slice raw, EdgeForms& forms) {
  std::size_t position = 0;
  const auto read = [&](unsigned count) {
    CHECK(position + count <= raw.size() * 8);
    unsigned value = 0;
    for (unsigned i = 0; i < count; ++i, ++position) {
      value = (value << 1) | ((raw.ubegin()[position / 8] >> (7 - position % 8)) & 1);
    }
    return value;
  };
  unsigned roots = read(32);
  for (unsigned i = 0; i < roots; ++i) {
    read(32);
  }
  unsigned nodes = read(32);
  std::vector<unsigned> refs(nodes), prefix_bits(nodes);
  for (unsigned i = 0; i < nodes; ++i) {
    ASSERT_EQ(read(4), 0u);
    refs[i] = read(4);
    const auto small = read(1);
    const auto length = read(7);
    prefix_bits[i] = small ? length % 8 : 0;
  }
  std::vector<std::vector<bool>> adjacent(nodes);
  for (unsigned i = 0; i < nodes; ++i) {
    for (unsigned j = 0; j < refs[i]; ++j) {
      adjacent[i].push_back(read(1) != 0);
      forms.direct += adjacent[i].back();
    }
  }
  for (auto length : prefix_bits) {
    read(length);
  }
  for (unsigned i = 0; i < nodes; ++i) {
    for (bool is_adjacent : adjacent[i]) {
      if (is_adjacent) {
        continue;
      }
      if (nodes <= i + 3) {
        ++forms.implicit_tail;
        continue;
      }
      unsigned required_bits = 0;
      for (unsigned remaining = nodes - i - 3; remaining; remaining >>= 1) {
        ++required_bits;
      }
      if (required_bits < 9 - (position + 1) % 8) {
        ++forms.fixed;
        read(required_bits);
      } else if (read(1)) {
        ++forms.compact;
        read(8 - position % 8);
      } else {
        ++forms.extended;
        read(required_bits);
      }
    }
  }
}

}  // namespace

TEST(BocCompressionRoundtrip, IndependentWireGoldens) {
  auto empty = bits_cell(0);
  const auto header = hex_bytes("000000010000000000000001");
  structure_golden({empty}, header + hex_bytes("0080"));
  structure_golden({bits_cell(1)}, header + hex_bytes("008100"));
  structure_golden({vm::CellBuilder{}.store_long(1, 1).finalize_novm()}, header + hex_bytes("008180"));
  structure_golden({vm::CellBuilder{}.store_long(0xa5, 8).finalize_novm()}, header + hex_bytes("0088a5"));
  structure_golden({bits_cell(127)}, header + hex_bytes("00ff") + std::string(16, '\0'));
  structure_golden({bits_cell(128)}, header + hex_bytes("001101") + std::string(16, '\0'));
  structure_golden({bits_cell(1023)}, header + hex_bytes("000080") + std::string(127, '\0'));
  structure_golden({bits_cell(0, 0, {empty})}, hex_bytes("0000000100000000000000020180008080"));
  structure_golden({bits_cell(0, 0, {empty, empty})}, hex_bytes("00000001000000000000000202800080c0"));
  structure_golden({empty, empty}, hex_bytes("000000020000000000000000000000010080"));
  structure_golden({pruned_cell(1)}, header + hex_bytes("20") + std::string(34, '\0'));
  structure_golden({library_cell()}, header + hex_bytes("10220102") + std::string(32, '\0'));
}

TEST(BocCompressionRoundtrip, FixedLz4DecodeCompatibility) {
  // Fixed literal-only LZ4 blocks deliberately do not depend on the linked
  // encoder's decisions. The baseline fixture contains the standard CRC32C BOC
  // of one empty cell; the improved one contains its independent wire golden.
  const std::array<std::string, 2> fixtures{hex_bytes("0000000011f002b5ee9c724101010100020000004cacb9cd"),
                                            hex_bytes("010000000ee00000000100000000000000010080")};
  const Roots expected{bits_cell(0)};
  for (const auto& fixture : fixtures) {
    assert_same_roots(expected, vm::boc_decompress(fixture, kMaxSize).move_as_ok());
  }
  auto baseline = vm::boc_compress(expected, Algorithm::BaselineLZ4).move_as_ok();
  ASSERT_EQ(pre_lz4(baseline).as_slice(), td::Slice(hex_bytes("b5ee9c724101010100020000004cacb9cd")));
}

TEST(BocCompressionRoundtrip, CompressorInputValidation) {
  for (auto algorithm : kAlgorithms) {
    ASSERT_TRUE(vm::boc_compress({}, algorithm).is_error());
  }
  const Cell null;
  const auto valid = bits_cell(0);
  for (const Roots& roots : {Roots{}, Roots{null}, Roots{valid, null}, Roots{null, valid}, Roots{null, null}}) {
    ASSERT_TRUE(vm::boc_compress(roots, Algorithm::ImprovedStructureLZ4).is_error());
  }
  // The legacy baseline BOC serializer drops mixed null roots and CHECK-fails
  // on all-null roots. These are known API limitations, not valid roundtrip
  // inputs or behavior to freeze as a passing expectation in this test runner.
  for (int algorithm : {-1, 2, 127, 128, 255, 256, 0x7fffffff}) {
    ASSERT_TRUE(vm::boc_compress({valid}, static_cast<Algorithm>(algorithm)).is_error());
  }
}

TEST(BocCompressionRoundtrip, AllOrdinaryBitLengthsAndReferenceCounts) {
  const Roots children{tagged_cell(0x11223344), tagged_cell(0x22334455), tagged_cell(0x33445566),
                       tagged_cell(0x44556677)};
  // Includes every small-data length, 127/128 transition, every large-data
  // sentinel position, 1016-bit encoded length wrap, and maximum 1023 bits.
  for (unsigned bits = 0; bits <= vm::Cell::max_bits; ++bits) {
    for (unsigned count = 0; count <= vm::Cell::max_refs; ++count) {
      roundtrip({bits_cell(bits, bits + 1, Roots(children.begin(), children.begin() + count))});
    }
  }
}

TEST(BocCompressionRoundtrip, ZeroAndOneDataAtEncodingBoundaries) {
  for (unsigned bits : {0u, 1u, 7u, 8u, 9u, 120u, 126u, 127u, 128u, 129u, 135u, 136u, 1015u, 1016u, 1022u, 1023u}) {
    for (bool value : {false, true}) {
      auto cell = vm::CellBuilder{}.store_bits_same(bits, value).finalize_novm();
      roundtrip({cell});
    }
  }
}

TEST(BocCompressionRoundtrip, RootOrderDuplicatesAndSharedEqualHashCells) {
  auto leaf = tagged_cell(1);
  auto equal_leaf = tagged_cell(1);
  ASSERT_TRUE(leaf.get() != equal_leaf.get());
  ASSERT_EQ(leaf->get_hash(), equal_leaf->get_hash());
  auto left = tagged_cell(2, {leaf, equal_leaf, leaf, equal_leaf});
  auto right = tagged_cell(3, {equal_leaf, left});
  auto root = tagged_cell(4, {left, right, leaf, right});
  Roots roots{leaf, root, left, right, equal_leaf, root};
  roundtrip(roots);
  std::reverse(roots.begin(), roots.end());
  roundtrip(roots);
  // Root count is bounded independently from the number of unique graph nodes.
  roundtrip(Roots(vm::BagOfCells::default_max_roots, leaf));
}

TEST(BocCompressionRoundtrip, EveryValidExoticTypeAndPrunedLevelMask) {
  auto library = library_cell();
  roundtrip({library});
  for (unsigned mask = 1; mask <= 7; ++mask) {
    auto pruned = pruned_cell(mask, 7, mask);
    ASSERT_EQ(pruned->get_level_mask().get_mask(), mask);
    auto ordinary = tagged_cell(mask, {pruned, library, pruned});
    auto proof = vm::CellBuilder::create_merkle_proof(ordinary);
    auto nested_proof = vm::CellBuilder::create_merkle_proof(proof);
    auto updated = tagged_cell(mask + 100, {pruned, library});
    auto update = vm::CellBuilder::create_merkle_update(ordinary, updated);
    roundtrip({pruned, ordinary, proof, nested_proof, update, library});
  }
  // Merge disjoint masks through ordinary references, then shift levels through
  // Merkle proofs/updates; this catches loss of non-top hashes or depths.
  auto merged =
      tagged_cell(999, {pruned_cell(1, 11, 1), pruned_cell(2, 17, 2), pruned_cell(4, 23, 4), pruned_cell(7, 31, 7)});
  ASSERT_EQ(merged->get_level_mask().get_mask(), 7u);
  roundtrip({merged, vm::CellBuilder::create_merkle_proof(merged),
             vm::CellBuilder::create_merkle_update(merged, pruned_cell(7, 32, 8))});
}

TEST(BocCompressionRoundtrip, MaximumRepresentableDepth) {
  Cell chain = bits_cell(0);
  Roots keep_alive{chain};
  for (unsigned depth = 1; depth <= vm::Cell::max_depth; ++depth) {
    chain = bits_cell(depth % 9, depth, {chain});
    keep_alive.push_back(chain);
    if (depth == vm::Cell::max_depth - 1 || depth == vm::Cell::max_depth) {
      ASSERT_EQ(chain->get_depth(), depth);
      roundtrip({chain});
    }
  }
  // Pruned depths come from the payload, not graph distance.
  for (unsigned mask = 1; mask <= 7; ++mask) {
    roundtrip({pruned_cell(mask, vm::Cell::max_depth)});
  }
}

TEST(BocCompressionRoundtrip, SeededRandomSharedDags) {
  // Fixed PRNG and explicit modulo operations make failures reproducible across
  // standard-library implementations (no implementation-defined distributions).
  std::mt19937 random(0xb0c2026u);
  constexpr std::array<unsigned, 14> boundary_bits{0, 1, 7, 8, 9, 126, 127, 128, 129, 1015, 1016, 1022, 1023, 264};
  for (unsigned trial = 0; trial < 80; ++trial) {
    Roots cells;
    for (unsigned i = 0; i < 160; ++i) {
      unsigned count = cells.empty() ? 0 : static_cast<unsigned>(random() % 5);
      Roots children;
      for (unsigned j = 0; j < count; ++j) {
        children.push_back(cells[random() % cells.size()]);
      }
      unsigned bits = i % 2 ? random() % 1024 : boundary_bits[random() % boundary_bits.size()];
      cells.push_back(bits_cell(bits, static_cast<unsigned>(random()), std::move(children)));
      if (i % 17 == 0) {
        cells.push_back(cells.back());
      }
    }
    Roots roots{cells.back()};
    for (unsigned i = 0; i < 8; ++i) {
      roots.push_back(cells[random() % cells.size()]);
    }
    roundtrip(roots);
    std::reverse(roots.begin(), roots.end());
    roundtrip(roots);
  }
}

TEST(BocCompressionRoundtrip, WideDagsExerciseEveryEdgeEncoding) {
  EdgeForms forms;
  // A three-node DAG forces a non-adjacent implicit-tail edge.
  auto leaf = tagged_cell(1);
  Roots small{tagged_cell(3, {leaf, tagged_cell(2, {leaf})})};
  roundtrip(small);
  count_edge_forms(pre_lz4(vm::boc_compress(small, Algorithm::ImprovedStructureLZ4).move_as_ok()).as_slice(), forms);

  std::mt19937 random(0xed9e2026u);
  for (unsigned width : {16u, 127u, 128u, 255u, 256u, 511u, 512u, 1024u}) {
    Roots leaves;
    for (unsigned i = 0; i < width; ++i) {
      vm::CellBuilder cb;
      cb.store_long(i, 32).store_zeroes(i % 8);
      leaves.push_back(cb.finalize_novm());
    }
    Roots roots;
    for (unsigned i = 0; i < width; ++i) {
      roots.push_back(tagged_cell(
          i + width, {leaves[i], leaves[(i + width / 2) % width], leaves[random() % width], leaves[(i + 1) % width]}));
    }
    roundtrip(roots);
    auto compressed = vm::boc_compress(roots, Algorithm::ImprovedStructureLZ4).move_as_ok();
    count_edge_forms(pre_lz4(compressed).as_slice(), forms);
  }
  ASSERT_TRUE(forms.direct > 0);
  ASSERT_TRUE(forms.implicit_tail > 0);
  ASSERT_TRUE(forms.fixed > 0);
  ASSERT_TRUE(forms.compact > 0);
  ASSERT_TRUE(forms.extended > 0);
}

// Malformed inputs and resource bounds.

namespace vm {
namespace {

// These fixtures describe the pre-LZ4 wire representation directly. They do not
// call the improved compressor: a matching bug in encoder and decoder must not
// make malformed-wire tests pass. Mutation tests cap allocations independently
// of every attacker-controlled size field.
// These bounds test rejection before large allocations, not arbitrary-memory
// OOM resilience. Some defensive paths cannot be encoded: all graph edges are
// strictly forward under the node-count cap, and metadata lengths are <=1024.
constexpr unsigned kMalformedLimit = 128 << 10;
using Roots = std::vector<td::Ref<Cell>>;

class WireBits {
 public:
  WireBits& put(std::uint32_t value, unsigned bits) {
    CHECK(bits <= 32);
    for (unsigned i = bits; i != 0; --i) {
      if (bit_count_ % 8 == 0) {
        data_.push_back(0);
      }
      data_.back() = static_cast<char>(static_cast<unsigned char>(data_.back()) |
                                       (((value >> (i - 1)) & 1) << (7 - bit_count_ % 8)));
      ++bit_count_;
    }
    return *this;
  }

  WireBits& byte(unsigned value) {
    return put(value, 8);
  }

  WireBits& u32(std::uint32_t value) {
    return put(value, 32);
  }

  WireBits& align() {
    while (bit_count_ % 8 != 0) {
      put(0, 1);
    }
    return *this;
  }

  const std::string& str() const {
    return data_;
  }

 private:
  std::string data_;
  unsigned bit_count_{0};
};

WireBits header(unsigned nodes = 1, unsigned root_index = 0) {
  WireBits result;
  result.u32(1).u32(root_index).u32(nodes);
  return result;
}

std::string frame(td::Slice raw, unsigned declared_size) {
  auto compressed = td::lz4_compress(raw);
  std::string result = WireBits().u32(declared_size).str();
  result.append(compressed.as_slice().str());
  return result;
}

std::string frame(td::Slice raw) {
  return frame(raw, static_cast<unsigned>(raw.size()));
}

std::string envelope(unsigned algorithm, td::Slice framed) {
  return std::string(1, static_cast<char>(algorithm)) + framed.str();
}

td::Result<Roots> decode_raw(td::Slice raw) {
  return boc_decompress(envelope(1, frame(raw)), kMalformedLimit);
}

template <class T>
void expect_error(td::Result<T> result, td::Slice message) {
  EXPECT(result.is_error());
  if (result.is_error()) {
    EXPECT_EQ(result.error().message().str(), message.str());
  }
}

void expect_decode_error(td::Slice raw, td::Slice message) {
  expect_error(decode_raw(raw), message);
}

void check_valid_roots(const Roots& roots) {
  ASSERT_TRUE(!roots.empty());
  for (const auto& root : roots) {
    ASSERT_TRUE(root.not_null());
    ASSERT_TRUE(root->get_depth() <= CellTraits::max_depth);
  }
  // A mutation need not be rejected: it can describe another valid DAG. Its
  // accepted roots must still survive the canonical BOC writer and reader.
  auto serialized = std_boc_serialize_multi(roots, 31).move_as_ok();
  auto restored = std_boc_deserialize_multi(serialized.as_slice(), BagOfCells::default_max_roots, true).move_as_ok();
  ASSERT_EQ(restored.size(), roots.size());
  for (std::size_t i = 0; i < roots.size(); ++i) {
    ASSERT_EQ(restored[i]->get_hash(), roots[i]->get_hash());
  }
}

void check_result(td::Result<Roots> result) {
  if (result.is_ok()) {
    check_valid_roots(result.move_as_ok());
  }
}

std::string empty_cell_wire() {
  return header().byte(0).byte(0x80).str();
}

std::string chain_wire(unsigned depth) {
  auto raw = header(depth + 1);
  for (unsigned i = 0; i <= depth; ++i) {
    raw.byte(i == depth ? 0 : 1).byte(0x80);  // ordinary empty cells
  }
  for (unsigned i = 0; i < depth; ++i) {
    raw.put(1, 1);  // each child is the following node
  }
  return raw.align().str();
}

// Node 0 refers to one of many leaves tagged by their node index. The caller supplies the complete
// edge bitstream, independently of the compressor's variable-length encoder.
std::string distant_edge_wire(unsigned nodes, const std::vector<unsigned>& edge_bytes) {
  auto raw = header(nodes).byte(1).byte(0x80);
  for (unsigned i = 1; i < nodes; ++i) {
    raw.byte(0).byte(0x90);  // ordinary leaf, 16-bit payload
  }
  for (auto value : edge_bytes) {
    raw.byte(value);
  }
  for (unsigned i = 1; i < nodes; ++i) {
    raw.put(i, 16);
  }
  return raw.str();
}

void check_distant_edge(unsigned nodes, const std::vector<unsigned>& edge_bytes, unsigned expected_index) {
  auto roots = decode_raw(distant_edge_wire(nodes, edge_bytes)).move_as_ok();
  ASSERT_EQ(roots.size(), 1u);
  CellSlice root(NoVm(), roots[0]);
  ASSERT_EQ(root.size_refs(), 1u);
  CellSlice child(NoVm(), root.fetch_ref());
  ASSERT_EQ(child.size(), 16u);
  ASSERT_EQ(child.fetch_ulong(16), expected_index);
  check_valid_roots(roots);
}

}  // namespace

TEST(BocCompressionMalformed, EnvelopeAndAlgorithmDispatch) {
  expect_error(boc_decompress({}, kMalformedLimit), "Can't decompress empty data");
  expect_error(boc_get_algorithm_name({}), "Can't get algorithm name from empty data");
  for (unsigned algorithm = 0; algorithm != 256; ++algorithm) {
    const std::string data(1, static_cast<char>(algorithm));
    if (algorithm <= 1) {
      expect_error(boc_decompress(data, kMalformedLimit), "BOC decompression failed: input too small for header");
      ASSERT_EQ(boc_get_algorithm_name(data).move_as_ok(), algorithm ? "ImprovedStructureLZ4" : "BaselineLZ4");
    } else {
      expect_error(boc_decompress(data, kMalformedLimit), "Unknown compression algorithm");
      expect_error(boc_get_algorithm_name(data), "Unknown compression algorithm");
    }
  }
  for (int limit : {0, -1, std::numeric_limits<int>::min()}) {
    expect_error(boc_decompress(envelope(1, frame(empty_cell_wire())), limit),
                 "BOC decompression failed: invalid decompressed size");
  }
}

TEST(BocCompressionMalformed, FrameHeaderAndAllocationBounds) {
  for (unsigned length = 0; length < kDecompressedSizeBytes; ++length) {
    std::string data(length, '\0');
    expect_error(boc_decompress(envelope(1, data), kMalformedLimit),
                 "BOC decompression failed: input too small for header");
    expect_error(boc_decompress_baseline_lz4(data, kMalformedLimit),
                 "BOC decompression failed: input too small for header");
  }
  for (std::uint32_t declared : {kMalformedLimit + 1, 0x7fffffffu, 0x80000000u, 0xffffffffu}) {
    auto data = WireBits().u32(declared).str();
    expect_error(boc_decompress(envelope(1, data), kMalformedLimit),
                 "BOC decompression failed: invalid decompressed size");
    expect_error(boc_decompress_baseline_lz4(data, kMalformedLimit),
                 "BOC decompression failed: invalid decompressed size");
    // The generic API's signed limit cannot permit INT_MAX+1. The direct
    // baseline API's unsigned limit permits it, but narrowing must still fail
    // before allocating a buffer.
    if (declared > 0x7fffffffu) {
      expect_error(boc_decompress(envelope(1, data), std::numeric_limits<int>::max()),
                   "BOC decompression failed: invalid decompressed size");
      expect_error(boc_decompress_baseline_lz4(data, 0xffffffffu), "Narrow cast failed");
    }
  }
  expect_error(boc_decompress_baseline_lz4(frame({}, 0), kMalformedLimit),
               "BOC decompression failed: invalid decompressed size");
  expect_error(boc_decompress(envelope(1, frame({}, 0)), kMalformedLimit),
               "BOC decompression failed: not enough bits to read");

  auto raw = empty_cell_wire();
  check_valid_roots(boc_decompress(envelope(1, frame(raw)), static_cast<int>(raw.size())).move_as_ok());
  expect_error(boc_decompress(envelope(1, frame(raw)), static_cast<int>(raw.size() - 1)),
               "BOC decompression failed: invalid decompressed size");
  expect_error(boc_decompress(envelope(1, frame(raw, static_cast<unsigned>(raw.size() + 1))), kMalformedLimit),
               "BOC decompression failed: decompressed size mismatch");
  EXPECT(boc_decompress(envelope(1, frame(raw, static_cast<unsigned>(raw.size() - 1))), kMalformedLimit).is_error());
  for (const auto& corrupt_lz4 : {std::string(), std::string(1, '\xff'), std::string(8, '\xff')}) {
    auto data = WireBits().u32(14).str() + corrupt_lz4;
    EXPECT(boc_decompress(envelope(1, data), kMalformedLimit).is_error());
    EXPECT(boc_decompress_baseline_lz4(data, kMalformedLimit).is_error());
  }
}

TEST(BocCompressionMalformed, BaselineBocValidationAndSizeCapacity) {
  // Standard BOC with one empty ordinary cell, no index, no CRC32C.
  const std::string boc =
      WireBits().u32(0xb5ee9c72).byte(1).byte(1).byte(1).byte(1).byte(0).byte(2).byte(0).byte(0).byte(0).str();
  for (unsigned capacity : {static_cast<unsigned>(boc.size()), static_cast<unsigned>(boc.size() + 1),
                            static_cast<unsigned>(boc.size() + 16)}) {
    auto data = frame(boc, capacity);
    check_valid_roots(boc_decompress_baseline_lz4(data, kMalformedLimit).move_as_ok());
    check_valid_roots(boc_decompress(envelope(0, data), kMalformedLimit).move_as_ok());
  }
  // Baseline uses the declared size as LZ4 output capacity; unlike improved,
  // it deliberately does not require exact equality with the actual output.
  EXPECT(boc_decompress_baseline_lz4(frame(boc, static_cast<unsigned>(boc.size() - 1)), kMalformedLimit).is_error());
  for (std::size_t length = 0; length < boc.size(); ++length) {
    EXPECT(boc_decompress_baseline_lz4(frame(td::Slice(boc).substr(0, length)), kMalformedLimit).is_error());
  }
  for (std::size_t offset : {std::size_t{0}, std::size_t{6}, std::size_t{10}, std::size_t{11}}) {
    auto invalid_boc = boc;
    invalid_boc[offset] = static_cast<char>(0xff);
    EXPECT(boc_decompress_baseline_lz4(frame(invalid_boc), kMalformedLimit).is_error());
  }
}

TEST(BocCompressionMalformed, RootAndNodeCounts) {
  for (unsigned length = 0; length < 12; ++length) {
    auto raw = empty_cell_wire().substr(0, length);
    expect_decode_error(raw, "BOC decompression failed: not enough bits to read");
  }
  for (std::uint32_t roots : {0u, 16385u, 0x7fffffffu, 0x80000000u, 0xffffffffu}) {
    expect_decode_error(WireBits().u32(roots).str(), "BOC decompression failed: invalid root count");
  }
  expect_decode_error(WireBits().u32(5).str(), "BOC decompression failed: invalid root count");
  for (std::uint32_t nodes : {0u, (1u << 20) + 1, 0x7fffffffu, 0x80000000u, 0xffffffffu}) {
    expect_decode_error(header(nodes).str(), "BOC decompression failed: invalid node count");
  }
  for (unsigned nodes : {13u, 1u << 20}) {
    expect_decode_error(header(nodes).str(), "BOC decompression failed: incorrect node count provided");
  }
  for (std::uint32_t root : {1u, 0x7fffffffu, 0x80000000u, 0xffffffffu}) {
    expect_decode_error(header(1, root).str(), "BOC decompression failed: invalid root index");
  }
  // The documented maximum is inclusive, including repeated root indexes.
  WireBits maximum_roots;
  maximum_roots.u32(BagOfCells::default_max_roots);
  for (unsigned i = 0; i < BagOfCells::default_max_roots; ++i) {
    maximum_roots.u32(0);
  }
  maximum_roots.u32(1).byte(0).byte(0x80);
  auto roots = decode_raw(maximum_roots.str()).move_as_ok();
  ASSERT_EQ(roots.size(), static_cast<std::size_t>(BagOfCells::default_max_roots));
  check_valid_roots(roots);
}

TEST(BocCompressionMalformed, MetadataAndPrunedBranches) {
  expect_decode_error(header().str(), "BOC decompression failed: not enough bits for cell metadata");
  expect_decode_error(header().byte(0).str(), "BOC decompression failed: not enough bits for data length");
  for (unsigned refs = 5; refs != 16; ++refs) {
    expect_decode_error(header().byte(refs).str(), "BOC decompression failed: invalid cell refs count");
  }
  for (unsigned type : {2u, 3u, 4u, 5u, 6u, 7u, 8u, 10u, 11u, 12u, 13u, 14u, 15u}) {
    for (unsigned refs = 2; refs <= 4; ++refs) {
      expect_decode_error(header().byte((type << 4) | refs).str(),
                          "BOC decompression failed: pruned branch cannot have references");
    }
  }
  expect_decode_error(header().byte(0x20).str(), "BOC decompression failed: not enough bits for remaining cell data");
  // The refs=1 sentinel encodes an absent pruned payload. A standalone root
  // cannot reconstruct a valid pruned branch from only its type and level.
  expect_decode_error(header().byte(0x21).str(), "BOC decompression failed: failed to finalize node (CellWriteError)");
  // Type 10 represents an out-of-range level mask (9), despite having a legal
  // payload length. It must reach CellBuilder validation and fail there.
  auto invalid_mask = header().byte(0xa0);
  for (unsigned i = 0; i < 68; ++i) {
    invalid_mask.byte(0);
  }
  expect_decode_error(invalid_mask.str(), "BOC decompression failed: failed to finalize node (CellWriteError)");
}

TEST(BocCompressionMalformed, DirectAndVariableLengthEdges) {
  expect_decode_error(header(2).byte(1).byte(0x80).byte(0).byte(0x80).str(),
                      "BOC decompression failed: not enough bits to read");
  for (unsigned edge_byte : {0u, 0x80u}) {
    expect_decode_error(header().byte(1).byte(0x80).byte(edge_byte).str(),
                        "BOC decompression failed: invalid graph connection");
  }
  // Five nodes: direct=0, 2-bit delta=2 selects the last node.
  check_distant_edge(5, {0x40}, 4);
  expect_decode_error(distant_edge_wire(5, {0x60}), "BOC decompression failed: invalid graph connection");
  // 260 nodes need 9 delta bits. Short form: direct=0, flag=1,
  // six-bit delta=63. Long form: direct=0, flag=0, nine-bit delta=257.
  check_distant_edge(260, {0x7f}, 65);
  check_distant_edge(260, {0x20, 0x20}, 259);
  expect_decode_error(distant_edge_wire(260, {0x3f, 0xe0}), "BOC decompression failed: invalid graph connection");
  expect_decode_error(distant_edge_wire(260, {0}).substr(0, 12 + 2 * 260 + 1),
                      "BOC decompression failed: not enough bits to read");
}

TEST(BocCompressionMalformed, PrefixPaddingSuffixAndEndOfStream) {
  expect_decode_error(header(2).byte(0).byte(0x87).byte(0).byte(0x87).byte(0).str(),
                      "BOC decompression failed: not enough bits for initial cell data");
  auto one_bit = header().byte(0).byte(0x81).byte(0x80).str();
  check_valid_roots(decode_raw(one_bit).move_as_ok());
  for (unsigned bit = 0; bit < 7; ++bit) {
    auto raw = one_bit;
    raw.back() = static_cast<char>(0x80 | (1u << bit));
    expect_decode_error(raw, "BOC decompression failed: non-zero padding bits");
  }
  expect_decode_error(header().byte(0).byte(0x88).str(),
                      "BOC decompression failed: not enough bits for remaining cell data");
  expect_decode_error(header().byte(0).byte(1).str(), "BOC decompression failed: not enough bits to read");
  expect_decode_error(header().byte(0).byte(1).byte(0).str(), "BOC decompression failed: not enough bits to read");
  expect_decode_error(header().byte(0).byte(1).byte(0).byte(0x80).str(),
                      "BOC decompression failed: invalid cell data length");
  expect_decode_error(header().byte(0).byte(2).byte(0x80).str(),
                      "BOC decompression failed: not enough bits for remaining cell data");
  for (unsigned byte : {0u, 1u, 0xffu}) {
    expect_decode_error(empty_cell_wire() + std::string(1, static_cast<char>(byte)),
                        "BOC decompression failed: trailing unused data");
  }
  // Large-data length=0 means 1024 encoded bits; marker + 1023 cell bits is
  // legal, while a missing final byte is an ordinary truncation error.
  auto maximum_cell = header().byte(0).byte(0).byte(0x80);
  for (unsigned i = 1; i < 128; ++i) {
    maximum_cell.byte(0);
  }
  auto roots = decode_raw(maximum_cell.str()).move_as_ok();
  ASSERT_EQ(CellSlice(NoVm(), roots[0]).size(), 1023u);
  check_valid_roots(roots);
  expect_decode_error(maximum_cell.str().substr(0, maximum_cell.str().size() - 1),
                      "BOC decompression failed: not enough bits for remaining cell data");
}

TEST(BocCompressionMalformed, DepthBoundaryBeforeRecursiveReconstruction) {
  for (unsigned depth :
       {0u, 1u, static_cast<unsigned>(CellTraits::max_depth - 1), static_cast<unsigned>(CellTraits::max_depth)}) {
    auto roots = decode_raw(chain_wire(depth)).move_as_ok();
    ASSERT_EQ(roots[0]->get_depth(), depth);
    check_valid_roots(roots);
  }
  for (unsigned depth :
       {static_cast<unsigned>(CellTraits::max_depth + 1), static_cast<unsigned>(2 * CellTraits::max_depth)}) {
    expect_decode_error(chain_wire(depth), "BOC decompression failed: cell depth too large");
  }
}

TEST(BocCompressionMalformed, PrunedBranchStoredDepthBounds) {
  for (unsigned depth :
       {0u, static_cast<unsigned>(CellTraits::max_depth), static_cast<unsigned>(CellTraits::max_depth + 1), 65535u}) {
    auto raw = header().byte(0x20);  // pruned level mask 1, one hash and one depth
    for (unsigned i = 0; i < 32; ++i) {
      raw.byte(0);
    }
    raw.put(depth, 16);
    if (depth <= CellTraits::max_depth) {
      auto roots = decode_raw(raw.str()).move_as_ok();
      ASSERT_EQ(roots[0]->get_depth(0), depth);
      check_valid_roots(roots);
    } else {
      expect_decode_error(raw.str(), "BOC decompression failed: failed to finalize node (CellWriteError)");
    }
  }
  // Graph DP sees only depth 1, but the stored depth of the pruned child
  // makes its ordinary parent exceed the cell depth limit at level 0.
  auto parent = header(2).byte(1).byte(0x80).byte(0x20).byte(0x80);
  for (unsigned i = 0; i < 32; ++i) {
    parent.byte(0);
  }
  parent.put(CellTraits::max_depth, 16);
  expect_decode_error(parent.str(), "BOC decompression failed: failed to finalize node (CellWriteError)");
}

TEST(BocCompressionMalformed, ExoticCellAndMerkleUpdateValidation) {
  expect_decode_error(header().byte(0x10).byte(0x80).str(),
                      "BOC decompression failed: failed to finalize node (CellWriteError)");
  for (unsigned tag : {0u, 1u, 2u, 3u, 4u, 5u, 255u}) {
    expect_decode_error(header().byte(0x10).byte(0x88).byte(tag).str(),
                        "BOC decompression failed: failed to finalize node (CellWriteError)");
  }
  expect_decode_error(header().byte(0x90).str(),
                      "BOC decompression failed: depth-balance node must be reconstructed under MerkleUpdate");
  // Root's third ref selects a tag-4 exotic cell with zero refs as main MU.
  expect_decode_error(header(2).byte(3).byte(0x80).byte(0x10).byte(0x88).byte(0xe0).byte(4).str(),
                      "BOC decompression failed: MerkleUpdate node expected to have 2 references");
  // Selected exotic node has too few bits even to contain a MerkleUpdate tag.
  expect_decode_error(header(2).byte(3).byte(0x80).byte(0x10).byte(0x80).byte(0xe0).str(),
                      "BOC decompression failed: failed to finalize node (CellWriteError)");
  // root -> [MU, MU, MU], MU -> [empty ordinary left, omitted right].
  auto no_grams = header(4).byte(3).byte(0x80).byte(0x12).byte(0x88).byte(0).byte(0x80).byte(0x90);
  no_grams.byte(0xf0).byte(4);
  expect_decode_error(no_grams.str(), "BOC decompression failed: depth-balance left vertex has no grams");
  // Left contains an empty label but truncated split-depth/CurrencyCollection
  // fields. It must fail the TLB parser, before any grams reconstruction.
  for (unsigned bits : {2u, 6u, 11u}) {
    auto truncated_balance = header(4).byte(3).byte(0x80).byte(0x12).byte(0x88);
    truncated_balance.byte(0).byte(0x80 | bits).byte(0x90);
    truncated_balance.put(0x1e, 5).put(0, bits % 8).align().byte(4);
    if (bits >= 8) {
      truncated_balance.byte(0);
    }
    expect_decode_error(truncated_balance.str(), "BOC decompression failed: depth-balance left vertex has no grams");
  }
  // The right subtree has an omitted child, but its paired left has no child.
  auto no_left = header(5).byte(3).byte(0x80).byte(0x12).byte(0x88).byte(0).byte(0x80);
  no_left.byte(1).byte(0x80).byte(0x90).byte(0xf4).byte(4);
  expect_decode_error(no_left.str(), "BOC decompression failed: depth-balance left vertex has no grams");
  // Parent's left balance=0, child's left balance=5 and right balance=0.
  // Reconstructing the omitted parent would produce -5 grams, which cannot
  // be represented by CurrencyCollection's unsigned VarUInteger16.
  auto negative = header(6).byte(3).byte(0x80).byte(0x12).byte(0x88).byte(1).byte(0x8c);
  negative.byte(0).byte(0x94).byte(0x91).byte(0).byte(0x8c);
  negative.put(0x7b, 7).put(0, 12).put(1, 2).align();  // edges, small-data prefixes, far edge
  negative.byte(4).byte(0).byte(2).byte(10).byte(0);
  expect_decode_error(negative.str(), "BOC decompression failed: failed to write depth-balance grams");

  // The opposite arithmetic boundary: parent left=(2^120-1), child left=0,
  // child right=1. Reconstructed parent=2^120 needs 16 bytes, but grams permits
  // at most 15. The large left cell has 132 bits (136 including its marker).
  auto overflow = header(6).byte(3).byte(0x80).byte(0x12).byte(0x88).byte(1).byte(17);
  overflow.byte(0).byte(0x8c).byte(0x91).byte(0).byte(0x94);
  overflow.put(0x7b, 7).put(0, 8).put(1, 2).align().byte(4);
  overflow.put(0, 3).put(1, 1).put(0, 7).put(15, 4);
  for (unsigned i = 0; i < 15; ++i) {
    overflow.byte(0xff);
  }
  overflow.put(0, 1).byte(0).byte(2).byte(2);
  expect_decode_error(overflow.str(), "BOC decompression failed: failed to write depth-balance grams");
}

TEST(BocCompressionMalformed, BoundedDeterministicMutationsAndTruncations) {
  const std::array<std::string, 5> corpus{{empty_cell_wire(), header().byte(0).byte(0x81).byte(0x80).str(),
                                           header().byte(0).byte(0x88).byte(0xab).str(), chain_wire(16),
                                           distant_edge_wire(260, {0x20, 0x20})}};
  for (const auto& raw : corpus) {
    check_valid_roots(decode_raw(raw).move_as_ok());
    for (std::size_t length = 0; length < raw.size(); ++length) {
      EXPECT(decode_raw(td::Slice(raw).substr(0, length)).is_error());
    }
    // Exhaust every one-bit mutation in the independently encoded raw data.
    // All declared frame sizes remain small, including mutated graph counts.
    for (std::size_t byte = 0; byte < raw.size(); ++byte) {
      for (unsigned bit = 0; bit < 8; ++bit) {
        auto changed = raw;
        changed[byte] = static_cast<char>(static_cast<unsigned char>(changed[byte]) ^ (1u << bit));
        check_result(decode_raw(changed));
      }
    }
    const auto compressed = envelope(1, frame(raw));
    for (std::size_t length = 0; length < compressed.size(); ++length) {
      EXPECT(boc_decompress(td::Slice(compressed).substr(0, length), kMalformedLimit).is_error());
    }
    for (std::size_t byte = 0; byte < compressed.size(); ++byte) {
      for (unsigned bit = 0; bit < 8; ++bit) {
        auto changed = compressed;
        changed[byte] = static_cast<char>(static_cast<unsigned char>(changed[byte]) ^ (1u << bit));
        check_result(boc_decompress(changed, kMalformedLimit));
      }
    }
  }
  // Fixed xorshift seed makes failures exactly reproducible. Random raw
  // payloads supplement targeted fixtures without allocating more than 255 B.
  std::uint32_t state = 0x51a7e123;
  auto random = [&]() {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
  };
  for (unsigned iteration = 0; iteration < 1024; ++iteration) {
    std::string raw(random() % 256, '\0');
    for (auto& byte : raw) {
      byte = static_cast<char>(random());
    }
    check_result(decode_raw(raw));
    check_result(boc_decompress(raw, kMalformedLimit));
  }
}

}  // namespace vm
