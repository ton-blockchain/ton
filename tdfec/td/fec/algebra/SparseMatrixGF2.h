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
#include "td/fec/algebra/MatrixGF2.h"

namespace td {
inline std::vector<uint32> inverse_permutation(Span<uint32> p) {
  std::vector<uint32> res(p.size());
  for (size_t i = 0; i < p.size(); i++) {
    res[p[i]] = narrow_cast<uint32>(i);
  }
  return res;
}

class IdentityGenerator {
 public:
  IdentityGenerator(uint32 N) : N(N) {
  }
  template <class F>
  void generate(F&& f) const {
    for (uint32 col = 0; col < N; col++) {
      f(col, col);
    }
  }
  uint32 non_zeroes() const {
    return N;
  }

  uint32 cols() const {
    return N;
  }
  uint32 rows() const {
    return N;
  }

 private:
  uint32 N;
};

template <class GeneratorT>
class PermutationGenerator {
 public:
  PermutationGenerator(const GeneratorT& m, Span<uint32> p) : m_(m), p_(inverse_permutation(p)) {
  }
  uint32 non_zeroes() const {
    return m_.non_zeroes();
  }
  uint32 rows() const {
    return m_.rows();
  }
  uint32 cols() const {
    return m_.cols();
  }
  template <class F>
  void generate(F&& f) const {
    m_.generate([&f, this](auto row, auto col) { f(row, p_[col]); });
  }

 private:
  const GeneratorT& m_;
  std::vector<uint32> p_;
};

template <class GeneratorT>
class TransposeGenerator {
 public:
  explicit TransposeGenerator(const GeneratorT& m) : m_(m) {
  }
  uint32 non_zeroes() const {
    return m_.non_zeroes();
  }
  uint32 rows() const {
    return m_.cols();
  }
  uint32 cols() const {
    return m_.rows();
  }
  template <class F>
  void generate(F&& f) const {
    m_.generate([&f](auto row, auto col) { f(col, row); });
  }

 private:
  const GeneratorT& m_;
  std::vector<uint32> p_;
};
class SparseMatrixGF2 {
 public:
  uint32 non_zeroes() const {
    return narrow_cast<uint32>(data_.size());
  }
  Span<uint32> col(uint32 i) const {
    return Span<uint32>(data_.data() + col_offset_[i], col_size(i));
  }
  uint32 col_size(uint32 i) const {
    return col_offset_[i + 1] - col_offset_[i];
  }
  uint32 cols() const {
    return cols_;
  }
  uint32 rows() const {
    return rows_;
  }

  template <class F>
  void generate(F&& f) const {
    return block_for_each(0, 0, rows_, cols_, f);
  }

  template <class F>
  void block_for_each(uint32 row_from, uint32 col_from, uint32 row_size, uint32 col_size, F&& f) const {
    auto col_till = col_from + col_size;
    auto row_till = row_from + row_size;
    for (uint32 col_i = col_from; col_i < col_till; col_i++) {
      auto col_span = col(col_i);
      auto* it = row_from == 0 ? col_span.begin() : std::lower_bound(col_span.begin(), col_span.end(), row_from);
      while (it != col_span.end() && *it < row_till) {
        f(*it - row_from, col_i - col_from);
        it++;
      }
    }
  }

  MatrixGF2 block_dense(uint32 row_from, uint32 col_from, uint32 row_size, uint32 col_size) const {
    MatrixGF2 res(row_size, col_size);
    res.set_zero();
    block_for_each(row_from, col_from, row_size, col_size, [&](auto row, auto col) { res.set_one(row, col); });
    return res;
  }

  template <class GeneratorT>
  explicit SparseMatrixGF2(GeneratorT&& generator) : rows_(generator.rows()), cols_(generator.cols()) {
    data_.resize(generator.non_zeroes());
    col_offset_.resize(cols_ + 1, 0);
    generator.generate([&](uint32 row, uint32 col) {
      LOG_DCHECK(row < rows_ && col < cols_) << "(" << row << "," << col << ") (" << rows_ << "," << cols_ << ")";
      col_offset_[col + 1]++;
    });
    for (uint32 i = 1; i < col_offset_.size(); i++) {
      col_offset_[i] += col_offset_[i - 1];
    }
    auto col_pos = col_offset_;
    generator.generate([&](uint32 row, uint32 col) { data_[col_pos[col]++] = row; });

    for (uint32 col_i = 0; col_i < cols_; col_i++) {
      auto c = col(col_i);
      for (size_t j = 1; j < c.size(); j++) {
        LOG_DCHECK(c[j] > c[j - 1]) << c[j] << " > " << c[j - 1] << tag("row", col_i);
      }
    }
  }

  class BlockView {
   public:
    BlockView(uint32 row_offset, uint32 col_offset, uint32 row_size, uint32 col_size, const SparseMatrixGF2& m)
        : row_offset_(row_offset), col_offset_(col_offset), row_size_(row_size), col_size_(col_size), m_(m) {
    }
    uint32 cols() const {
      return col_size_;
    }
    uint32 rows() const {
      return row_size_;
    }
    uint32 non_zeroes() const {
      uint32 res = 0;
      m_.block_for_each(row_offset_, col_offset_, row_size_, col_size_, [&res](auto row, auto col) { res++; });
      return res;
    }

    template <class F>
    void generate(F&& f) const {
      m_.block_for_each(row_offset_, col_offset_, row_size_, col_size_, [&f](auto row, auto col) { f(row, col); });
    }

   private:
    uint32 row_offset_;
    uint32 col_offset_;
    uint32 row_size_;
    uint32 col_size_;
    const SparseMatrixGF2& m_;
  };

  SparseMatrixGF2 block_sparse(uint32 row_from, uint32 col_from, uint32 row_size, uint32 col_size) const {
    return SparseMatrixGF2(BlockView(row_from, col_from, row_size, col_size, *this));
  }
  SparseMatrixGF2 transpose() const {
    return SparseMatrixGF2(TransposeGenerator<SparseMatrixGF2>(*this));
  }

  SparseMatrixGF2 apply_col_permutation(Span<uint32> p) const {
    return SparseMatrixGF2(PermutationGenerator<SparseMatrixGF2>(*this, p));
  }
  SparseMatrixGF2 apply_row_permutation(Span<uint32> p) const {
    return transpose().apply_col_permutation(p).transpose();
  }

 private:
  uint32 rows_{0};
  uint32 cols_{0};
  std::vector<uint32> data_;
  std::vector<uint32> col_offset_;
};

template <class M>
M operator*(const SparseMatrixGF2& a, const M& b) {
  M res(a.rows(), b.cols());
  res.set_zero();
  a.generate([&](auto row, auto col) { res.row_add(row, b.row(col)); });
  return res;
}

template <class... ArgsT>
class BlockGenerator {
 public:
  BlockGenerator(uint32 rows, uint32 cols, ArgsT&&... args)
      : rows_(rows), cols_(cols), tuple_(std::forward_as_tuple(std::forward<ArgsT>(args)...)) {
  }
  template <class F>
  void generate(F&& f) const {
    uint32 row_offset = 0;
    uint32 next_row_offset = 0;
    uint32 col_offset = 0;
    tuple_for_each(tuple_, [&](auto& g) {
      if (col_offset == 0) {
        next_row_offset = row_offset + g.rows();
      } else {
        CHECK(next_row_offset == row_offset + g.rows());
      }
      g.generate([&](auto row, auto col) { f(row_offset + row, col_offset + col); });
      col_offset += g.cols();
      if (col_offset >= cols_) {
        CHECK(col_offset == cols_);
        col_offset = 0;
        row_offset = next_row_offset;
      }
    });
  }
  uint32 non_zeroes() const {
    uint32 res = 0;
    tuple_for_each(tuple_, [&](auto& g) { res += g.non_zeroes(); });
    return res;
  }

  uint32 cols() const {
    return cols_;
  }
  uint32 rows() const {
    return rows_;
  }

 private:
  uint32 rows_;
  uint32 cols_;
  std::tuple<ArgsT...> tuple_;
};

template <class... ArgsT>
auto block_generator(uint32 rows, uint32 cols, ArgsT&&... args) {
  return BlockGenerator<ArgsT...>(rows, cols, std::forward<ArgsT>(args)...);
}
}  // namespace td
