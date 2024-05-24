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

#pragma once

#include "td/utils/optional.h"
#include "td/utils/Slice.h"
#include "vm/cells.h"

#include "Bitset.h"
#include <map>

namespace ton {
// merkle_node$_ {n:#} left:^(ton::MerkleTree n) right:^(ton::MerkleTree n) = ton::MerkleTree (n + 1);
// merkle_leaf$_ hash:bits256 = ton::MerkleTree 0;

class MerkleTree {
 public:
  MerkleTree() = default;
  MerkleTree(size_t pieces_count, td::Bits256 root_hash);
  explicit MerkleTree(std::vector<td::Bits256> hashes);

  td::Status add_proof(td::Ref<vm::Cell> proof);
  td::Result<td::Bits256> get_piece_hash(size_t idx) const;
  td::Result<td::Ref<vm::Cell>> gen_proof(size_t l, size_t r) const;
  td::Ref<vm::Cell> get_root(size_t depth_limit = std::numeric_limits<size_t>::max()) const;

  std::vector<size_t> add_pieces(std::vector<std::pair<size_t, td::Bits256>> pieces);

  size_t get_depth() const {
    return depth_;
  }

  td::Bits256 get_root_hash() const {
    return root_hash_;
  }

 private:
  size_t pieces_count_{0};
  td::Bits256 root_hash_ = td::Bits256::zero();
  size_t depth_{0}, n_{1};
  td::Ref<vm::Cell> root_proof_;

  td::Ref<vm::Cell> do_add_pieces(td::Ref<vm::Cell> node, std::vector<size_t> &ok_pieces, size_t il, size_t ir,
                                  std::pair<size_t, td::Bits256> *pl, std::pair<size_t, td::Bits256> *pr);
};

}  // namespace ton
