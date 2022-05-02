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

#include "MerkleTree.h"

#include "common/bitstring.h"
#include "td/utils/UInt.h"

#include "vm/cells/CellSlice.h"
#include "vm/cells/MerkleProof.h"
#include "vm/cellslice.h"
#include "vm/excno.hpp"

namespace ton {
static td::Ref<vm::Cell> unpack_proof(td::Ref<vm::Cell> root) {
  vm::CellSlice cs(vm::NoVm(), root);
  CHECK(cs.special_type() == vm::Cell::SpecialType::MerkleProof);
  return cs.fetch_ref();
}

td::uint32 MerkleTree::get_depth() const {
  return log_n_;
}
td::Ref<vm::Cell> MerkleTree::get_root(size_t depth_limit) const {
  if (depth_limit > log_n_ || root_proof_.is_null()) {
    return root_proof_;
  }

  auto usage_tree = std::make_shared<vm::CellUsageTree>();
  auto root_raw = vm::MerkleProof::virtualize(root_proof_, 1);
  auto usage_cell = vm::UsageCell::create(root_raw, usage_tree->root_ptr());
  do_gen_proof(std::move(usage_cell), unpack_proof(root_proof_), depth_limit);
  auto res = vm::MerkleProof::generate(root_raw, usage_tree.get());
  CHECK(res.not_null());
  return res;
}

void MerkleTree::do_gen_proof(td::Ref<vm::Cell> node, td::Ref<vm::Cell> node_raw, size_t depth_limit) const {
  if (depth_limit == 0) {
    return;
  }
  // check if it is possible to load node without breaking virtualization
  vm::CellSlice cs_raw(vm::NoVm(), std::move(node_raw));
  if (cs_raw.is_special()) {
    return;
  }
  vm::CellSlice cs(vm::NoVm(), std::move(node));
  while (cs.have_refs()) {
    do_gen_proof(cs.fetch_ref(), cs_raw.fetch_ref(), depth_limit - 1);
  }
}

td::Bits256 MerkleTree::get_root_hash() const {
  CHECK(root_hash_);
  return root_hash_.value();
}

MerkleTree::MerkleTree(size_t chunks_count, td::Bits256 root_hash) {
  init_begin(chunks_count);
  root_hash_ = root_hash;
  init_finish();
}

MerkleTree::MerkleTree(size_t chunks_count, td::Ref<vm::Cell> root_proof) {
  init_begin(chunks_count);
  root_hash_ = unpack_proof(root_proof)->get_hash(0).as_array();
  root_proof_ = std::move(root_proof);
  init_finish();
}

MerkleTree::MerkleTree(td::Span<Chunk> chunks) {
  init_begin(chunks.size());

  for (size_t i = 0; i < chunks.size(); i++) {
    CHECK(chunks[i].index == i);
    init_add_chunk(i, chunks[i].hash.as_slice());
  }

  init_finish();
}

void MerkleTree::init_begin(size_t chunks_count) {
  log_n_ = 0;
  while ((size_t(1) << log_n_) < chunks_count) {
    log_n_++;
  }
  n_ = size_t(1) << log_n_;
  total_blocks_ = chunks_count;
  mark_.resize(n_ * 2);
  proof_.resize(n_ * 2);

  td::UInt256 null{};
  auto cell = vm::CellBuilder().store_bytes(null.as_slice()).finalize();
  for (auto i = chunks_count; i < n_; i++) {
    proof_[i + n_] = cell;
  }
}

void MerkleTree::init_add_chunk(size_t index, td::Slice hash) {
  CHECK(index < total_blocks_);
  CHECK(proof_[index + n_].is_null());
  proof_[index + n_] = vm::CellBuilder().store_bytes(hash).finalize();
}

void MerkleTree::init_finish() {
  for (size_t i = n_ - 1; i >= 1; i--) {
    auto j = i * 2;
    if (proof_[j].is_null()) {
      continue;
    }
    if (i + 1 < n_ && proof_[i + 1].not_null() && proof_[j]->get_hash() == proof_[j + 2]->get_hash() &&
        proof_[j + 1]->get_hash() == proof_[j + 3]->get_hash()) {
      // minor optimization for same chunks
      proof_[i] = proof_[i + 1];
    } else {
      proof_[i] = vm::CellBuilder().store_ref(proof_[j]).store_ref(proof_[j + 1]).finalize();
    }
  }
  if (proof_[1].not_null()) {
    init_proof();
  }
  CHECK(root_hash_);
}

void MerkleTree::remove_chunk(std::size_t index) {
  CHECK(index < n_);
  index += n_;
  while (proof_[index].not_null()) {
    proof_[index] = {};
    index /= 2;
  }
}

bool MerkleTree::has_chunk(std::size_t index) const {
  CHECK(index < n_);
  index += n_;
  return proof_[index].not_null();
}

void MerkleTree::add_chunk(std::size_t index, td::Slice hash) {
  CHECK(hash.size() == 32);
  CHECK(index < n_);
  index += n_;
  auto cell = vm::CellBuilder().store_bytes(hash).finalize();
  CHECK(proof_[index].is_null());
  proof_[index] = std::move(cell);
  mark_[index] = mark_id_;
  for (index /= 2; index != 0; index /= 2) {
    CHECK(proof_[index].is_null());
    auto &left = proof_[index * 2];
    auto &right = proof_[index * 2 + 1];
    if (left.not_null() && right.not_null()) {
      proof_[index] = vm::CellBuilder().store_ref(left).store_ref(right).finalize();
      mark_[index] = mark_id_;
    }
  }
}

static td::Status do_validate(td::Ref<vm::Cell> ref, size_t depth) {
  vm::CellSlice cs(vm::NoVm(), std::move(ref));
  if (cs.is_special()) {
    if (cs.special_type() != vm::Cell::SpecialType::PrunnedBranch) {
      return td::Status::Error("Unexpected special cell");
    }
    return td::Status::OK();
  }
  if (depth == 0) {
    if (cs.size() != 256) {
      return td::Status::Error("List in proof must have 256 bits");
    }
    if (cs.size_refs() != 0) {
      return td::Status::Error("List in proof must have zero refs");
    }
  } else {
    if (cs.size() != 0) {
      return td::Status::Error("Node in proof must have zero bits");
    }
    if (cs.size_refs() != 2) {
      return td::Status::Error("Node in proof must have two refs");
    }
    TRY_STATUS(do_validate(cs.fetch_ref(), depth - 1));
    TRY_STATUS(do_validate(cs.fetch_ref(), depth - 1));
  }
  return td::Status::OK();
}

td::Status MerkleTree::validate_proof(td::Ref<vm::Cell> new_root) {
  // 1. depth <= log_n
  // 2. each non special node has two refs and nothing else
  // 3. each list contains only hash
  // 4. all special nodes are merkle proofs
  vm::CellSlice cs(vm::NoVm(), new_root);
  if (cs.special_type() != vm::Cell::SpecialType::MerkleProof) {
    return td::Status::Error("Proof must be a mekle proof cell");
  }
  auto root = cs.fetch_ref();
  if (root_hash_ && root->get_hash(0).as_slice() != root_hash_.value().as_slice()) {
    return td::Status::Error("Proof has invalid root hash");
  }
  return do_validate(std::move(root), log_n_);
}

td::Status MerkleTree::add_proof(td::Ref<vm::Cell> new_root) {
  CHECK(root_proof_.not_null() || root_hash_);
  TRY_STATUS(validate_proof(new_root));
  if (root_proof_.not_null()) {
    auto combined = vm::MerkleProof::combine_fast(root_proof_, std::move(new_root));
    if (combined.is_null()) {
      return td::Status::Error("Can't combine proofs");
    }
    root_proof_ = std::move(combined);
  } else {
    root_proof_ = std::move(new_root);
  }
  return td::Status::OK();
}

td::Status MerkleTree::validate_existing_chunk(const Chunk &chunk) {
  vm::CellSlice cs(vm::NoVm(), proof_[chunk.index + n_]);
  CHECK(cs.size() == chunk.hash.size());
  if (cs.as_bitslice().compare(chunk.hash.cbits()) != 0) {
    return td::Status::Error("Hash mismatch");
  }
  return td::Status::OK();
}

td::Status MerkleTree::try_add_chunks(td::Span<Chunk> chunks) {
  td::Bitset bitmask;
  add_chunks(chunks, bitmask);
  for (size_t i = 0; i < chunks.size(); i++) {
    if (!bitmask.get(i)) {
      return td::Status::Error(PSLICE() << "Invalid chunk #" << chunks[i].index);
    }
  }
  return td::Status::OK();
}

void MerkleTree::add_chunks(td::Span<Chunk> chunks, td::Bitset &bitmask) {
  if (root_proof_.is_null()) {
    return;
  }

  mark_id_++;
  bitmask.reserve(chunks.size());
  for (size_t i = 0; i < chunks.size(); i++) {
    const auto &chunk = chunks[i];
    if (has_chunk(chunk.index)) {
      if (validate_existing_chunk(chunk).is_ok()) {
        bitmask.set_one(i);
      }
      continue;
    }
    add_chunk(chunk.index, chunk.hash.as_slice());
  }

  root_proof_ = vm::CellBuilder::create_merkle_proof(merge(unpack_proof(root_proof_), 1));

  for (size_t i = 0; i < chunks.size(); i++) {
    const auto &chunk = chunks[i];
    if (has_chunk(chunk.index) && mark_[chunk.index + n_] == mark_id_) {
      bitmask.set_one(i);
    }
  }
}

td::Ref<vm::Cell> MerkleTree::merge(td::Ref<vm::Cell> root, size_t index) {
  const auto &down = proof_[index];
  if (down.not_null()) {
    if (down->get_hash() != root->get_hash(0)) {
      proof_[index] = {};
    } else {
      return down;
    }
  }

  if (mark_[index] != mark_id_ || index >= n_) {
    return root;
  }

  vm::CellSlice cs(vm::NoVm(), root);
  if (cs.is_special()) {
    cleanup_add(index);
    return root;
  }

  CHECK(cs.size_refs() == 2);
  vm::CellBuilder cb;
  cb.store_bits(cs.fetch_bits(cs.size()));
  auto left = merge(cs.fetch_ref(), index * 2);
  auto right = merge(cs.fetch_ref(), index * 2 + 1);
  cb.store_ref(std::move(left)).store_ref(std::move(right));
  return cb.finalize();
}

void MerkleTree::cleanup_add(size_t index) {
  if (mark_[index] != mark_id_) {
    return;
  }
  proof_[index] = {};
  if (index >= n_) {
    return;
  }
  cleanup_add(index * 2);
  cleanup_add(index * 2 + 1);
}

void MerkleTree::init_proof() {
  CHECK(proof_[1].not_null());
  td::Bits256 new_root_hash = proof_[1]->get_hash(0).as_array();
  CHECK(!root_hash_ || root_hash_.value() == new_root_hash);
  root_hash_ = new_root_hash;
  root_proof_ = vm::CellBuilder::create_merkle_proof(proof_[1]);
}

td::Result<td::Ref<vm::Cell>> MerkleTree::gen_proof(size_t l, size_t r) {
  if (root_proof_.is_null()) {
    return td::Status::Error("got no proofs yet");
  }
  auto usage_tree = std::make_shared<vm::CellUsageTree>();
  auto root_raw = vm::MerkleProof::virtualize(root_proof_, 1);
  auto usage_cell = vm::UsageCell::create(root_raw, usage_tree->root_ptr());
  TRY_STATUS(TRY_VM(do_gen_proof(std::move(usage_cell), 0, n_ - 1, l, r)));
  auto res = vm::MerkleProof::generate(root_raw, usage_tree.get());
  CHECK(res.not_null());
  return res;
}

td::Status MerkleTree::do_gen_proof(td::Ref<vm::Cell> node, size_t il, size_t ir, size_t l, size_t r) const {
  if (ir < l || il > r) {
    return td::Status::OK();
  }
  if (l <= il && ir <= r) {
    return td::Status::OK();
  }
  vm::CellSlice cs(vm::NoVm(), std::move(node));
  if (cs.is_special()) {
    return td::Status::Error("Can't generate a proof");
  }
  CHECK(cs.size_refs() == 2);
  auto ic = (il + ir) / 2;
  TRY_STATUS(do_gen_proof(cs.fetch_ref(), il, ic, l, r));
  TRY_STATUS(do_gen_proof(cs.fetch_ref(), ic + 1, ir, l, r));
  return td::Status::OK();
}
}  // namespace ton
