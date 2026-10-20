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

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "block/block-parse.h"
#include "common/refint.h"
#include "td/utils/Slice.h"
#include "td/utils/lz4.h"
#include "td/utils/tests.h"
#include "vm/boc-compression.h"
#include "vm/cells/CellBuilder.h"
#include "vm/cells/CellSlice.h"
#include "vm/cells/CellUsageTree.h"
#include "vm/cells/MerkleUpdate.h"
#include "vm/cells/UsageCell.h"

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

struct RoundTripResult {
  bool ok{false};
  unsigned balance_elision_markers{0};
  std::string error;
};

RoundTripResult round_trip(const Ref<Cell>& root) {
  RoundTripResult result;
  auto compressed_result = boc_compress({root}, CompressionAlgorithm::ImprovedStructureLZ4);
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
  if (decompressed.size() != 1) {
    result.error = "root count changed";
    return result;
  }
  if (decompressed[0].is_null() || decompressed[0]->get_hash() != root->get_hash()) {
    result.error = "root hash changed";
    return result;
  }
  result.ok = true;
  return result;
}

bool expect_round_trip(const char* scenario, const Ref<Cell>& block, unsigned expected_elisions) {
  auto result = round_trip(block);
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

}  // namespace vm
