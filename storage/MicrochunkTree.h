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

#pragma once

#include "td/utils/optional.h"
#include "td/utils/Slice.h"
#include "vm/cells.h"

#include "Bitset.h"
#include <map>

namespace ton {

class Torrent;

class MicrochunkTree {
 public:
  static const size_t MICROCHUNK_SIZE = 64;

  class Builder {
   public:
    explicit Builder(td::uint64 file_size, td::uint64 prun_size = 1 << 17);
    void add_data(td::Slice s);
    MicrochunkTree finalize();

    static td::Result<MicrochunkTree> build_for_torrent(Torrent &torrent, td::uint64 prun_size = 1 << 17);
   private:
    td::uint64 file_size_;
    td::uint64 prun_size_;
    td::uint64 total_size_;
    std::vector<td::Ref<vm::Cell>> proof_;
    unsigned char cur_microchunk_[MICROCHUNK_SIZE];
    td::uint64 cur_size_ = 0;

    void add_microchunk(td::Slice s);
  };

  MicrochunkTree() = default;
  MicrochunkTree(td::Ref<vm::Cell> root_proof);

  td::Result<td::Ref<vm::Cell>> get_proof(td::uint64 l, td::uint64 r, Torrent &torrent) const;

  td::Ref<vm::Cell> get_root() const {
    return root_proof_;
  }
  td::Bits256 get_root_hash() const {
    return root_hash_;
  }
  td::uint64 get_total_size() const {
    return total_size_;
  }

 private:
  td::Bits256 root_hash_ = td::Bits256::zero();
  td::uint64 total_size_ = 0;
  td::Ref<vm::Cell> root_proof_ = {};
};

}  // namespace ton
