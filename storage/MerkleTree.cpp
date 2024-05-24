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
static td::Result<td::Ref<vm::Cell>> unpack_proof(td::Ref<vm::Cell> root) {
  vm::CellSlice cs(vm::NoVm(), root);
  if (cs.special_type() != vm::Cell::SpecialType::MerkleProof) {
    return td::Status::Error("Not a merkle proof");
  }
  return cs.fetch_ref();
}

MerkleTree::MerkleTree(size_t pieces_count, td::Bits256 root_hash)
    : pieces_count_(pieces_count), root_hash_(root_hash) {
  depth_ = 0;
  n_ = 1;
  while (n_ < pieces_count_) {
    ++depth_;
    n_ <<= 1;
  }
}

static td::Ref<vm::Cell> build_tree(td::Bits256 *hashes, size_t len) {
  if (len == 1) {
    return vm::CellBuilder().store_bytes(hashes[0].as_slice()).finalize();
  }
  td::Ref<vm::Cell> l = build_tree(hashes, len / 2);
  td::Ref<vm::Cell> r = build_tree(hashes + len / 2, len / 2);
  return vm::CellBuilder().store_ref(l).store_ref(r).finalize();
};

MerkleTree::MerkleTree(std::vector<td::Bits256> hashes) : pieces_count_(hashes.size()) {
  depth_ = 0;
  n_ = 1;
  while (n_ < pieces_count_) {
    ++depth_;
    n_ <<= 1;
  }
  hashes.resize(n_, td::Bits256::zero());
  td::Ref<vm::Cell> root = build_tree(hashes.data(), n_);
  root_hash_ = root->get_hash().bits();
  root_proof_ = vm::CellBuilder::create_merkle_proof(std::move(root));
}

static td::Status do_validate_proof(td::Ref<vm::Cell> node, size_t depth) {
  if (node->get_depth(0) != depth) {
    return td::Status::Error("Depth mismatch");
  }
  vm::CellSlice cs(vm::NoVm(), std::move(node));
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
    TRY_STATUS(do_validate_proof(cs.fetch_ref(), depth - 1));
    TRY_STATUS(do_validate_proof(cs.fetch_ref(), depth - 1));
  }
  return td::Status::OK();
}

td::Status MerkleTree::add_proof(td::Ref<vm::Cell> proof) {
  if (proof.is_null()) {
    return td::Status::OK();
  }
  TRY_RESULT(proof_raw, unpack_proof(proof));
  if (root_hash_ != proof_raw->get_hash(0).bits()) {
    return td::Status::Error("Root hash mismatch");
  }
  TRY_STATUS(do_validate_proof(proof_raw, depth_));
  if (root_proof_.is_null()) {
    root_proof_ = std::move(proof);
  } else {
    auto combined = vm::MerkleProof::combine_fast(root_proof_, std::move(proof));
    if (combined.is_null()) {
      return td::Status::Error("Can't combine proofs");
    }
    root_proof_ = std::move(combined);
  }
  return td::Status::OK();
}

td::Result<td::Bits256> MerkleTree::get_piece_hash(size_t idx) const {
  if (idx >= n_) {
    return td::Status::Error("Index is too big");
  }
  if (root_proof_.is_null()) {
    return td::Status::Error("Hash is not known");
  }
  size_t l = 0, r = n_ - 1;
  td::Ref<vm::Cell> node = unpack_proof(root_proof_).move_as_ok();
  while (true) {
    vm::CellSlice cs(vm::NoVm(), std::move(node));
    if (cs.is_special()) {
      return td::Status::Error("Hash is not known");
    }
    if (l == r) {
      td::Bits256 hash;
      CHECK(cs.fetch_bits_to(hash.bits(), 256));
      return hash;
    }
    CHECK(cs.size_refs() == 2);
    size_t mid = (l + r) / 2;
    if (idx <= mid) {
      node = cs.prefetch_ref(0);
      r = mid;
    } else {
      node = cs.prefetch_ref(1);
      l = mid + 1;
    }
  }
}

static td::Status do_gen_proof(td::Ref<vm::Cell> node, size_t il, size_t ir, size_t l, size_t r) {
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

td::Result<td::Ref<vm::Cell>> MerkleTree::gen_proof(size_t l, size_t r) const {
  if (root_proof_.is_null()) {
    return td::Status::Error("Got no proofs yet");
  }
  auto usage_tree = std::make_shared<vm::CellUsageTree>();
  auto root_raw = vm::MerkleProof::virtualize(root_proof_, 1);
  auto usage_cell = vm::UsageCell::create(root_raw, usage_tree->root_ptr());
  TRY_STATUS(TRY_VM(do_gen_proof(std::move(usage_cell), 0, n_ - 1, l, r)));
  auto res = vm::MerkleProof::generate(root_raw, usage_tree.get());
  CHECK(res.not_null());
  return res;
}

static void do_gen_proof(td::Ref<vm::Cell> node, td::Ref<vm::Cell> node_raw, size_t depth_limit) {
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

td::Ref<vm::Cell> MerkleTree::get_root(size_t depth_limit) const {
  if (depth_limit > depth_ || root_proof_.is_null()) {
    return root_proof_;
  }
  auto usage_tree = std::make_shared<vm::CellUsageTree>();
  auto root_raw = vm::MerkleProof::virtualize(root_proof_, 1);
  auto usage_cell = vm::UsageCell::create(root_raw, usage_tree->root_ptr());
  do_gen_proof(std::move(usage_cell), unpack_proof(root_proof_).move_as_ok(), depth_limit);
  auto res = vm::MerkleProof::generate(root_raw, usage_tree.get());
  CHECK(res.not_null());
  return res;
}

static td::Ref<vm::Cell> build_from_hashes(std::pair<size_t, td::Bits256> *p, std::pair<size_t, td::Bits256> *pend,
                                           size_t len) {
  if (len == 1) {
    return vm::CellBuilder().store_bytes((p < pend ? p->second : td::Bits256::zero()).as_slice()).finalize();
  }
  td::Ref<vm::Cell> l = build_from_hashes(p, pend, len / 2);
  td::Ref<vm::Cell> r = build_from_hashes(p + len / 2, pend, len / 2);
  return vm::CellBuilder().store_ref(l).store_ref(r).finalize();
}

td::Ref<vm::Cell> MerkleTree::do_add_pieces(td::Ref<vm::Cell> node, std::vector<size_t> &ok_pieces, size_t il,
                                            size_t ir, std::pair<size_t, td::Bits256> *pl,
                                            std::pair<size_t, td::Bits256> *pr) {
  if (pl == pr || il >= pieces_count_) {
    return node;
  }
  vm::CellSlice cs;
  if (node.is_null() || (cs = vm::CellSlice(vm::NoVm(), node)).is_special() || il + 1 == ir) {
    if ((size_t)(pr - pl) != std::min(ir, pieces_count_) - il) {
      return node;
    }
    td::Ref<vm::Cell> new_node = build_from_hashes(pl, pr, ir - il);
    td::Bits256 new_hash = new_node->get_hash().bits();
    if (new_hash != (node.is_null() ? root_hash_ : node->get_hash(0).bits())) {
      return node;
    }
    for (auto p = pl; p != pr; ++p) {
      ok_pieces.push_back(p->first);
    }
    if (node.is_null() || cs.is_special()) {
      node = std::move(new_node);
    }
    return node;
  }
  size_t imid = (il + ir) / 2;
  auto pmid = pl;
  while (pmid != pr && pmid->first < imid) {
    ++pmid;
  }
  td::Ref<vm::Cell> l = do_add_pieces(cs.prefetch_ref(0), ok_pieces, il, imid, pl, pmid);
  td::Ref<vm::Cell> r = do_add_pieces(cs.prefetch_ref(1), ok_pieces, imid, ir, pmid, pr);
  if (l != cs.prefetch_ref(0) || r != cs.prefetch_ref(1)) {
    node = vm::CellBuilder().store_ref(l).store_ref(r).finalize();
  }
  return node;
}

std::vector<size_t> MerkleTree::add_pieces(std::vector<std::pair<size_t, td::Bits256>> pieces) {
  if (pieces.empty()) {
    return {};
  }
  std::sort(pieces.begin(), pieces.end());
  for (size_t i = 0; i + 1 < pieces.size(); ++i) {
    CHECK(pieces[i].first != pieces[i + 1].first);
  }
  CHECK(pieces.back().first < pieces_count_);
  std::vector<size_t> ok_pieces;
  td::Ref<vm::Cell> root;
  if (!root_proof_.is_null()) {
    root = unpack_proof(root_proof_).move_as_ok();
  }
  root = do_add_pieces(root, ok_pieces, 0, n_, pieces.data(), pieces.data() + pieces.size());
  if (!root.is_null()) {
    root_proof_ = vm::CellBuilder::create_merkle_proof(std::move(root));
  }
  return ok_pieces;
}

}  // namespace ton
