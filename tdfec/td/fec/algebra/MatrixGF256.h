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
#include "td/fec/algebra/Octet.h"
#include "td/fec/algebra/Simd.h"

#include "td/utils/Span.h"
#include "td/utils/format.h"

namespace td {
class MatrixGF256 {
 public:
  MatrixGF256(size_t rows, size_t cols) : rows_(rows), cols_(cols) {
    stride_ = (cols_ + Simd::alignment() - 1) / Simd::alignment() * Simd::alignment();
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

  MatrixGF256 apply_row_permutation(Span<uint32> permutation) {
    MatrixGF256 res(rows_, cols_);
    for (size_t row = 0; row < rows_; row++) {
      res.row(row).copy_from(this->row(permutation[row]));
    }
    return res;
  }

  Octet get(size_t row, size_t col) const {
    DCHECK(row < rows_ && col < cols_);
    return Octet(matrix_[row * stride_ + col]);
  }

  void set(size_t row, size_t col, Octet o) {
    DCHECK(row < rows_ && col < cols_);
    matrix_[row * stride_ + col] = o.value();
  }

  void row_multiply(size_t row, Octet o) {
    uint8* p = row_ptr(row);
    Simd::gf256_mul(p, o.value(), stride_);
  }

  Slice row(size_t row) const {
    return Slice(row_ptr(row), cols());
  }
  MutableSlice row(size_t row) {
    return MutableSlice(row_ptr(row), cols());
  }

  template <class M>
  void set_from(const M& m, size_t row_offset, size_t col_offset) {
    auto to = block_view(row_offset, col_offset, rows() - row_offset, cols() - col_offset);
    for (size_t i = 0; i < m.rows(); i++) {
      to.row(i).copy_from(m.row(i));
    }
  }

  MatrixGF256 copy() {
    MatrixGF256 res(rows(), cols());
    res.set_from(*this, 0, 0);
    return res;
  }

  void add(const MatrixGF256& m) {
    CHECK(m.rows() == rows());
    CHECK(m.cols() == cols());
    for (size_t i = 0; i < m.rows(); i++) {
      auto* to = row_ptr(i);
      auto* from = m.row_ptr(i);
      row_add(to, from);
    }
  }

  // row(a) += row(b) * m
  void row_add_mul(size_t a, size_t b, Octet m) {
    row_add_mul(row_ptr(a), row_ptr(b), m);
  }
  void row_add_mul(size_t a, Slice b, Octet m) {
    row_add_mul(row_ptr(a), b.ubegin(), m);
  }

  // row(a) += row(b)
  void row_add(size_t a, size_t b) {
    row_add(row_ptr(a), row_ptr(b));
  }

  void row_add(size_t a, Slice b) {
    row_add(row_ptr(a), b.ubegin());
  }

  void row_set(size_t a, Slice b) {
    row(a).copy_from(b);
  }

  class BlockView {
   public:
    BlockView(size_t row_offset, size_t col_offset, size_t row_size, size_t col_size, MatrixGF256& m)
        : row_offset_(row_offset), col_offset_(col_offset), row_size_(row_size), col_size_(col_size), m_(m) {
    }
    size_t cols() const {
      return col_size_;
    }
    size_t rows() const {
      return row_size_;
    }
    Slice row(size_t row) const {
      return m_.row(row_offset_ + row).remove_prefix(col_offset_);
    }
    MutableSlice row(size_t row) {
      return m_.row(row_offset_ + row).remove_prefix(col_offset_);
    }

   private:
    size_t row_offset_;
    size_t col_offset_;
    size_t row_size_;
    size_t col_size_;
    MatrixGF256& m_;
  };

  BlockView block_view(size_t row_offset, size_t col_offset, size_t row_size, size_t col_size) {
    return BlockView(row_offset, col_offset, row_size, col_size, *this);
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

  void row_add_mul(uint8* ap, const uint8* bp, Octet m) {
    uint8 u = m.value();
    if (u == 0) {
      return;
    }

    if (u == 1) {
      return row_add(ap, bp);
    }

    Simd::gf256_add_mul(ap, bp, u, stride_);
  }

  void row_add(uint8* ap, const uint8* bp) {
    Simd::gf256_add(ap, bp, stride_);
  }
};

inline StringBuilder& operator<<(StringBuilder& sb, const MatrixGF256& m) {
  sb << "\n";
  for (uint32 i = 0; i < m.rows(); i++) {
    auto row = m.row(i);
    for (uint32 j = 0; j < m.cols(); j++) {
      uint8 x = row[j];
      sb << " " << format::hex_digit(x / 16) << format::hex_digit(x % 16);
    }
    sb << "\n";
  }
  return sb;
}
}  // namespace td
