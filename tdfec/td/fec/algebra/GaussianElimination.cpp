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
#include "td/fec/algebra/GaussianElimination.h"
namespace td {
Result<MatrixGF256> GaussianElimination::run(MatrixGF256 A, MatrixGF256 D) {
  const size_t cols = A.cols();
  const size_t rows = A.rows();

  CHECK(cols <= rows);

  std::vector<uint32> row_perm(rows);
  for (uint32 i = 0; i < rows; i++) {
    row_perm[i] = i;
  }
  for (size_t row = 0; row < cols; row++) {
    size_t non_zero_row = row;
    for (; non_zero_row < rows && A.get(row_perm[non_zero_row], row).is_zero(); non_zero_row++) {
    }
    if (non_zero_row == rows) {
      return Status::Error("Non solvable");
    }
    if (non_zero_row != row) {
      std::swap(row_perm[non_zero_row], row_perm[row]);
    }
    auto mul = A.get(row_perm[row], row).inverse();
    A.row_multiply(row_perm[row], mul);
    D.row_multiply(row_perm[row], mul);
    CHECK(A.get(row_perm[row], row).value() == 1);
    for (size_t zero_row = 0; zero_row < rows; zero_row++) {
      if (zero_row == row) {
        continue;
      }
      auto x = A.get(row_perm[zero_row], row);
      if (!x.is_zero()) {
        A.row_add_mul(row_perm[zero_row], row_perm[row], x);
        D.row_add_mul(row_perm[zero_row], row_perm[row], x);
      }
    }
  }

  return D.apply_row_permutation(row_perm);
}
}  // namespace td
