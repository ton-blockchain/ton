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
#include "td/utils/Status.h"

#include "td/fec/algebra/MatrixGF256.h"
#include "td/fec/algebra/SparseMatrixGF2.h"

namespace td {
namespace raptorq {
class Rfc {
 public:
  struct RawParameters {
    uint32 K_padded;
    uint32 J;
    uint32 S;
    uint32 H;
    uint32 W;
  };
  struct EncodingRow {
    uint32 d;   // [1,30] LT degree
    uint32 a;   // [0,W)
    uint32 b;   // [0,W)
    uint32 d1;  // [2,3]  PI degree
    uint32 a1;  // [0,P1)
    uint32 b1;  // [0,P1)
  };
  struct Parameters {
    uint32 K;
    uint32 K_padded;
    uint32 J;
    uint32 S;
    uint32 H;
    uint32 W;
    uint32 L;
    uint32 P;
    uint32 P1;
    uint32 U;
    uint32 B;

    EncodingRow get_encoding_row(uint32 X /*ISI*/) const;

    uint32 encoding_row_size(const EncodingRow &t) const;

    template <class F>
    void encoding_row_for_each(EncodingRow t, F &&f) const {
      f(t.b);
      for (uint16 j = 1; j < t.d; ++j) {
        t.b = (t.b + t.a) % W;
        f(t.b);
      }

      while (t.b1 >= P)
        t.b1 = (t.b1 + t.a1) % P1;
      f(W + t.b1);
      for (uint16 j = 1; j < t.d1; ++j) {
        t.b1 = (t.b1 + t.a1) % P1;
        while (t.b1 >= P)
          t.b1 = (t.b1 + t.a1) % P1;
        f(W + t.b1);
      }
    }

    Parameters(uint32 K, RawParameters raw_parameters);

    class LDPC1 {
     public:
      LDPC1(uint32 S, uint32 B) : S(S), B(B) {
      }
      static void sort_inplace(uint32 &a, uint32 &b, uint32 &c) {
        if (a > c) {
          std::swap(a, c);
        }
        if (b > c) {
          std::swap(b, c);
        }
        if (a > b) {
          std::swap(a, b);
        }
        DCHECK(a < b && b < c);
      }
      template <class F>
      void generate(F &&f) const {
        for (uint32 col = 0; col < B; col++) {
          uint32 i = col / S;
          uint32 shift = col % S;
          uint32 a = shift;
          uint32 b = (i + 1 + shift) % S;
          uint32 c = (2 * (i + 1) + shift) % S;
          DCHECK(a != b);
          DCHECK(a != c);
          DCHECK(b != c);
          sort_inplace(a, b, c);
          f(a, col);
          f(b, col);
          f(c, col);
        }
      }
      uint32 non_zeroes() const {
        return B * 3;
      }

      uint32 cols() const {
        return B;
      }
      uint32 rows() const {
        return S;
      }

     private:
      uint32 S;

      uint32 B;
    };

    // 1100000
    // 0110000
    // 0011000
    // .......
    // 1100000
    class LDPC2 {
     public:
      LDPC2(uint32 rows, uint32 cols) : rows_(rows), cols_(cols) {
      }
      template <class F>
      void generate(F &&f) const {
        for (uint32 row = 0; row < rows_; row++) {
          f(row, row % cols_);
          f(row, (row + 1) % cols_);
        }
      }
      uint32 non_zeroes() const {
        return rows_ * 2;
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
    };

    static MatrixGF256 HDPC_multiply(const uint32 rows, MatrixGF256 v) {
      Octet alpha = Octet(Octet::oct_exp(1));
      for (uint32 i = 1; i < v.rows(); i++) {
        v.row_add_mul(i, i - 1, alpha);
      }

      MatrixGF256 u(rows, v.cols());
      u.set_zero();
      for (uint32 i = 0; i < rows; i++) {
        u.row_add_mul(i, v.row(narrow_cast<uint32>(v.rows() - 1)), Octet(Octet::oct_exp(i % 255)));
      }

      for (uint32 col = 0; col + 1 < v.rows(); col++) {
        auto a = Rfc::random(col + 1, 6, rows);
        auto b = (a + Rfc::random(col + 1, 7, rows - 1) + 1) % rows;
        u.row_add(a, v.row(col));
        u.row_add(b, v.row(col));
      }
      return u;
    }

    class ENC {
     public:
      ENC(const Rfc::Parameters &p, Span<Rfc::EncodingRow> encoding_rows) : p(p), encoding_rows(encoding_rows) {
      }
      template <class F>
      void generate(F &&f) const {
        uint32 row = 0;
        for (auto encoding_row : encoding_rows) {
          p.encoding_row_for_each(encoding_row, [&f, row](auto col) { f(row, col); });
          row++;
        }
      }
      uint32 non_zeroes() const {
        uint32 res = 0;
        for (auto encoding_row : encoding_rows) {
          res += p.encoding_row_size(encoding_row);
        }
        return res;
      }

      uint32 cols() const {
        return p.L;
      }
      uint32 rows() const {
        return narrow_cast<uint32>(encoding_rows.size());
      }

     private:
      const Rfc::Parameters &p;
      Span<Rfc::EncodingRow> encoding_rows;
    };

    LDPC1 get_LDPC1() const {
      return LDPC1(S, B);
    }
    LDPC2 get_LDPC2() const {
      return LDPC2(S, P);
    }
    ENC get_ENC(Span<EncodingRow> encoding_rows) const {
      return ENC(*this, encoding_rows);
    }

    MatrixGF256 HDPC_multiply(MatrixGF256 v) const {
      return HDPC_multiply(H, std::move(v));
    }
    SparseMatrixGF2 get_A_upper(Span<EncodingRow> encoding_rows) const {
      return SparseMatrixGF2(block_generator(narrow_cast<uint32>(S + encoding_rows.size()), L, get_LDPC1(),
                                             IdentityGenerator(S), get_LDPC2(), get_ENC(encoding_rows)));
    }

   private:
    uint32 get_degree(uint32 v) const;
  };

  static uint32 random(uint32 y, uint32 i, uint32 m);
  static Result<Parameters> get_parameters(size_t K);
};
}  // namespace raptorq
}  // namespace td
