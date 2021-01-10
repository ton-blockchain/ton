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

#include "td/utils/common.h"
#include "td/fec/algebra/SparseMatrixGF2.h"

namespace td {
struct InactivationDecodingResult {
  uint32 size;
  vector<uint32> p_rows;
  vector<uint32> p_cols;
};

class InactivationDecoding {
 public:
  InactivationDecoding(const SparseMatrixGF2 &L, uint32 PI) : L_(L), PI_(PI) {
  }
  InactivationDecodingResult run();

 private:
  const SparseMatrixGF2 &L_;
  uint32 PI_;

  const SparseMatrixGF2 L_rows_{L_.transpose()};
  const uint32 cols_ = L_.cols() - PI_;
  const uint32 rows_ = L_.rows();
  vector<bool> was_row_;
  vector<bool> was_col_;

  vector<uint32> col_cnt_;
  vector<uint32> row_cnt_;
  vector<uint32> row_xor_;

  vector<uint32> sorted_rows_;
  vector<uint32> row_cnt_offset_;
  vector<uint32> row_pos_;

  vector<uint32> p_rows_;
  vector<uint32> p_cols_;
  vector<uint32> inactive_cols_;

  void init();

  void loop();

  void check_sorted();

  uint32 choose_col(uint32 row);

  void inactivate_col(uint32 col);

  void sort_rows();

  void check(uint32 side);
};
}  // namespace td
