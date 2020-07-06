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

namespace ton {
// merkle_node$_ {n:#} left:^(ton::MerkleTree n) right:^(ton::MerkleTree n) = ton::MerkleTree (n + 1);
// merkle_leaf$_ hash:bits256 = ton::MerkleTree 0;

class MerkleTree {
 public:
  td::uint32 get_depth() const;
  td::Ref<vm::Cell> get_root(size_t depth_limit = std::numeric_limits<size_t>::max()) const;
  td::Bits256 get_root_hash() const;

  MerkleTree(size_t chunks_count, td::Bits256 root_hash);
  MerkleTree(size_t chunks_count, td::Ref<vm::Cell> root_proof);

  struct Chunk {
    td::size_t index{0};
    td::Bits256 hash;
  };

  explicit MerkleTree(td::Span<Chunk> chunks);

  MerkleTree() = default;
  void init_begin(size_t chunks_count);
  void init_add_chunk(td::size_t index, td::Slice hash);
  void init_finish();

  // merge external proof with an existing proof
  td::Status add_proof(td::Ref<vm::Cell> new_root);
  // generate proof for all chunks from l to r inclusive
  td::Result<td::Ref<vm::Cell>> gen_proof(size_t l, size_t r);

  // Trying to add and validate list of chunks simultaniously
  td::Status try_add_chunks(td::Span<Chunk> chunks);

  // Returns bitmask of successfully added chunks
  // Intended to be used during validation of a torrent.
  // We got arbitrary chunks read from disk, and we got an arbirary proof.
  // Now we can say about some chunks that they are correct. This ia a general way
  // to do this.
  //
  // NB: already added chunks are simply validated. One should be careful
  // not to process them twice
  void add_chunks(td::Span<Chunk> chunks, td::Bitset &bitmask);

 private:
  td::uint64 total_blocks_;
  td::size_t n_;  // n = 2^log_n
  td::uint32 log_n_;
  td::size_t mark_id_{0};
  std::vector<td::size_t> mark_;          // n_ * 2
  std::vector<td::Ref<vm::Cell>> proof_;  // n_ * 2

  td::optional<td::Bits256> root_hash_;
  td::Ref<vm::Cell> root_proof_;

  td::Status validate_proof(td::Ref<vm::Cell> new_root);
  bool has_chunk(td::size_t index) const;
  void remove_chunk(td::size_t index);

  void add_chunk(td::size_t index, td::Slice hash);
  void init_proof();

  td::Ref<vm::Cell> merge(td::Ref<vm::Cell> root, size_t index);
  void cleanup_add(size_t index);
  td::Status do_gen_proof(td::Ref<vm::Cell> node, size_t il, size_t ir, size_t l, size_t r) const;
  void do_gen_proof(td::Ref<vm::Cell> node, td::Ref<vm::Cell> node_raw, size_t depth_limit) const;
  td::Status validate_existing_chunk(const Chunk &chunk);
};

}  // namespace ton
