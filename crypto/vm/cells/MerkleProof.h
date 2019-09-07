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

    Copyright 2017-2019 Telegram Systems LLP
*/
#pragma once
#include "vm/cells/Cell.h"

#include <utility>
#include <functional>

namespace vm {
class MerkleProof {
 public:
  using IsPrunnedFunction = std::function<bool(const Ref<Cell> &)>;

  // works with proofs wrapped in MerkleProof special cell
  // cells must have zero level
  static Ref<Cell> generate(Ref<Cell> cell, IsPrunnedFunction is_prunned);
  static Ref<Cell> generate(Ref<Cell> cell, CellUsageTree *usage_tree);

  // cell must have zero level and must be a MerkleProof
  static Ref<Cell> virtualize(Ref<Cell> cell, int virtualization);

  static Ref<Cell> combine(Ref<Cell> a, Ref<Cell> b);

  // works with upwrapped proofs
  // works fine with cell of non-zero level, but this is not supported (yet?) in MerkeProof special cell
  static Ref<Cell> generate_raw(Ref<Cell> cell, IsPrunnedFunction is_prunned);
  static Ref<Cell> generate_raw(Ref<Cell> cell, CellUsageTree *usage_tree);
  static Ref<Cell> virtualize_raw(Ref<Cell> cell, Cell::VirtualizationParameters virt);
};
}  // namespace vm
