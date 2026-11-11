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

#include <utility>
#include <vector>

#include "ton/ton-types.h"
#include "vm/cells/Cell.h"

namespace ton::validator {

struct CandidateRoots : td::CntObject {
  CandidateRoots(FileHash block_file_hash, FileHash collated_file_hash, td::Ref<vm::Cell> block_root,
                 std::vector<td::Ref<vm::Cell>> collated_roots)
      : block_file_hash(block_file_hash)
      , collated_file_hash(collated_file_hash)
      , block_root(std::move(block_root))
      , collated_roots(std::move(collated_roots)) {
  }

  FileHash block_file_hash;
  FileHash collated_file_hash;
  td::Ref<vm::Cell> block_root;
  std::vector<td::Ref<vm::Cell>> collated_roots;
};

using CandidateRootsRef = td::Ref<CandidateRoots>;

}  // namespace ton::validator
