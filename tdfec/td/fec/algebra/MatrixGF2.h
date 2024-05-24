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
#include "td/fec/algebra/MatrixGF256.h"

namespace td {
class MatrixGF2 {
 public:
  MatrixGF2(size_t rows, size_t cols) : rows_(rows), cols_(cols) {
    CHECK(Simd::alignment() % 8 == 0);
    stride_ = ((cols_ + 7) / 8 + Simd::alignment() - 1) / Simd::alignment() * Simd::alignment();
    CHECK(stride_ * 8 >= cols_);
    storage_ = std::make_unique<uint8[]>(stride_ * rows + Simd::alignment() - 1);
    matrix_ = storage_.get();
    while (!Simd::is_aligned_pointer(matrix_)) {
      matrix_++;
    }
    CHECK(Simd::is_aligned_pointer(matrix_));
    CHECK(Simd::is_aligned_pointer(matrix_ + stride_));
    CHECK(static_cast<size_t>(matrix_ - storage_.get()) < Simd::alignment());
  }
  void set_zero() {
    std::fill(matrix_, matrix_ + stride_ * rows_, 0);
  }
  size_t rows() const {
    return rows_;
  }
  size_t cols() const {
    return cols_;
  }

  void set_one(size_t row, size_t col) {
    DCHECK(row < rows_ && col < cols_);
    matrix_[row * stride_ + col / 8] |= uint8(1 << (col % 8));
  }
  bool get(size_t row, size_t col) const {
    DCHECK(row < rows_ && col < cols_);
    return (matrix_[row * stride_ + col / 8] & (uint8(1) << (col % 8))) != 0;
  }

  // row(a) += row(b)
  void row_add(size_t a, size_t b) {
    row_add(row_ptr(a), row_ptr(b));
  }

  void row_add(size_t a, Slice b) {
    DCHECK(b.size() == stride_);
    row_add(row_ptr(a), b.ubegin());
  }

  Slice row(size_t a) const {
    return Slice(row_ptr(a), stride_);
  }
  MutableSlice row(size_t a) {
    return MutableSlice(row_ptr(a), stride_);
  }
  void row_set(size_t a, Slice b) {
    row(a).copy_from(b);
  }

  MatrixGF256 to_gf256() const {
    MatrixGF256 res(rows(), cols());
    for (size_t i = 0; i < rows(); i++) {
      Simd::gf256_from_gf2(res.row(i).data(), row(i).data(), ((cols_ + 7) / 8 + 3) / 4 * 4);
    }
    return res;
  }

 private:
  uint8* matrix_;
  size_t rows_;
  size_t cols_;
  size_t stride_;
  std::unique_ptr<uint8[]> storage_;

  uint8* row_ptr(size_t row) {
    return matrix_ + stride_ * row;
  }
  const uint8* row_ptr(size_t row) const {
    return matrix_ + stride_ * row;
  }
  void row_add(uint8* pa, const uint8* pb) {
    Simd::gf256_add(pa, pb, stride_);
  }
};

}  // namespace td
