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
#include "td/fec/raptorq/Solver.h"
#include "td/fec/algebra/GaussianElimination.h"
#include "td/fec/algebra/InactivationDecoding.h"

#include "td/utils/Timer.h"
#include <map>

namespace td {
namespace raptorq {

MatrixGF256 create_D(const Rfc::Parameters &p, Span<SymbolRef> symbols) {
  auto symbol_size = symbols[0].data.size();
  MatrixGF256 D(p.S + p.H + symbols.size(), symbol_size);
  D.set_zero();

  auto offset = p.S;
  for (auto &symbol : symbols) {
    for (size_t i = 0; i < symbol_size; i++) {
      D.set(offset, i, Octet(symbol.data[i]));
    }
    offset++;
  }
  return D;
}

Result<MatrixGF256> Solver::run(const Rfc::Parameters &p, Span<SymbolRef> symbols) {
  if (0) {  // turns out gauss is slower even for small symbols count
    auto encoding_rows = transform(symbols, [&p](auto &symbol) { return p.get_encoding_row(symbol.id); });
    MatrixGF256 A(p.S + p.H + symbols.size(), p.L);
    A.set_zero();
    auto A_upper = p.get_A_upper(encoding_rows);
    A_upper.block_for_each(0, 0, A_upper.rows(), A_upper.cols(), [&](auto x, auto y) { A.set(x, y, Octet(1)); });

    MatrixGF256 tmp(A.cols() - p.H, A.cols() - p.H);
    tmp.set_zero();
    for (size_t i = 0; i < tmp.cols(); i++) {
      tmp.set(i, i, Octet(1));
    }
    auto HDCP = p.HDPC_multiply(std::move(tmp));
    MatrixGF256 HDCP2(p.H, p.L - p.H);

    MatrixGF256 IH(p.H, p.H);
    IH.set_zero();
    for (size_t i = 0; i < p.H; i++) {
      IH.set(i, i, Octet(1));
    }

    A.set_from(HDCP, A_upper.rows(), 0);
    A.set_from(IH, A_upper.rows(), HDCP.cols());

    auto D = create_D(p, symbols);
    auto C = GaussianElimination::run(std::move(A), std::move(D));
    return C;
  }
  PerfWarningTimer x("solve");
  Timer timer;
  auto perf_log = [&](Slice message) {
    if (GET_VERBOSITY_LEVEL() > VERBOSITY_NAME(DEBUG)) {
      static std::map<std::string, double> total;
      static double total_all = 0;
      auto elapsed = timer.elapsed();
      auto current_total = total[message.str()] += elapsed;
      total_all += elapsed;
      LOG(DEBUG) << "PERF: " << message << " " << timer << " " << current_total / total_all * 100;
      timer = {};
    }
  };
  // Solve linear system
  // A * C = D
  // C - intermeidate symbols
  // D - encoded symbols and restriction symbols.
  //
  // A:
  // +--------+-----+-------+
  // | LDPC1  | I_S |  LDPC2|
  // +--------+-----+-------+
  // | ENC                  |
  // +---------------+------+
  // | HDCP          | I_H  |
  // +---------------+------+
  CHECK(p.K_padded <= symbols.size());
  auto encoding_rows = transform(symbols, [&p](auto &symbol) { return p.get_encoding_row(symbol.id); });

  // Generate matrix A_upper: sparse part of A, first S + K_padded rows.
  SparseMatrixGF2 A_upper = p.get_A_upper(encoding_rows);
  auto D = create_D(p, symbols);
  perf_log("Generate sparse matrix");

  // Run indactivation decoding.
  // Lets call resulting lower triangualr matrix U
  auto decoding_result = InactivationDecoding(A_upper, p.P).run();
  perf_log("Inactivation decoding");
  uint32 U_size = decoding_result.size;

  auto row_permutation = std::move(decoding_result.p_rows);
  while (row_permutation.size() < D.rows()) {
    row_permutation.push_back(narrow_cast<uint32>(row_permutation.size()));
  }
  auto col_permutation = std::move(decoding_result.p_cols);

  // +--------+---------+        +---------+
  // | U      | E       |        | D_upper |
  // +--------+---------+        +---------+
  // | G_left | G_right | * C =  |         |
  // +--------+--+------+        | D_lower |
  // |HDCP       | I_H  |        |         |
  // +-----------+------+        +---------+

  D = D.apply_row_permutation(row_permutation);
  perf_log("D: apply permutation");
  A_upper = A_upper.apply_row_permutation(row_permutation).apply_col_permutation(col_permutation);
  perf_log("A_upper: apply permutation");

  auto E = A_upper.block_dense(0, U_size, U_size, p.L - U_size);
  perf_log("Calc E");

  MatrixGF256 C(A_upper.cols(), D.cols());
  C.set_from(D.block_view(0, 0, U_size, D.cols()), 0, 0);
  // Make U Identity matrix and calculate E and D_upper.
  for (uint32 i = 0; i < U_size; i++) {
    for (auto row : A_upper.col(i)) {
      if (row == i) {
        continue;
      }
      if (row >= U_size) {
        break;
      }
      E.row_add(row, i);
      D.row_add(row, i);  // this is SLOW
    }
  }
  perf_log("Triangular -> Identity");

  auto HDPC_left_multiply = [&](const MatrixGF256 &m) {
    MatrixGF256 T(p.K_padded + p.S, m.cols());
    T.set_zero();
    for (uint32 i = 0; i < m.rows(); i++) {
      T.row_set(col_permutation[i], m.row(i));
    }
    return p.HDPC_multiply(std::move(T));
  };

  SparseMatrixGF2 G_left = A_upper.block_sparse(U_size, 0, A_upper.rows() - U_size, U_size);
  perf_log("G_left");

  // Calculate small_A_upper
  // small_A_upper = G_right
  MatrixGF256 small_A_upper(A_upper.rows() - U_size, A_upper.cols() - U_size);
  A_upper.block_for_each(U_size, U_size, A_upper.rows() - U_size, A_upper.cols() - U_size,
                         [&](auto row, auto col) { small_A_upper.set(row, col, Octet(1)); });
  // small_A_upper += G_left * E
  small_A_upper.add((G_left * E).to_gf256());
  perf_log("small_A_upper");

  // Calculate small_A_lower
  MatrixGF256 small_A_lower(p.H, A_upper.cols() - U_size);
  small_A_lower.set_zero();
  for (uint32 i = 1; i <= p.H; i++) {
    small_A_lower.set(small_A_lower.rows() - i, small_A_lower.cols() - i, Octet(1));
  }

  // Calculate HDPC_right and set it into small_A_lower
  MatrixGF256 T(p.K_padded + p.S, p.K_padded + p.S - U_size);
  T.set_zero();
  for (uint32 i = 0; i < T.cols(); i++) {
    T.set(col_permutation[i + T.rows() - T.cols()], i, Octet(1));
  }
  MatrixGF256 HDCP_right = p.HDPC_multiply(std::move(T));
  small_A_lower.set_from(HDCP_right, 0, 0);
  perf_log("small_A_lower");

  // small_A_lower += HDPC_left * E
  auto t = E.to_gf256();
  perf_log("t");
  small_A_lower.add(HDPC_left_multiply(std::move(t)));
  perf_log("small_A_lower += HDPC_left * E");

  MatrixGF256 D_upper(U_size, D.cols());
  D_upper.set_from(D.block_view(0, 0, D_upper.rows(), D_upper.cols()), 0, 0);

  // small_D_upper
  MatrixGF256 small_D_upper(A_upper.rows() - U_size, D.cols());
  small_D_upper.set_from(D.block_view(U_size, 0, small_D_upper.rows(), small_D_upper.cols()), 0, 0);
  small_D_upper.add(G_left * D_upper);
  perf_log("small_D_upper");

  // small_D_lower
  MatrixGF256 small_D_lower(p.H, D.cols());
  small_D_lower.set_from(D.block_view(A_upper.rows(), 0, small_D_lower.rows(), small_D_lower.cols()), 0, 0);
  perf_log("small_D_lower");

  small_D_lower.add(HDPC_left_multiply(D_upper));
  perf_log("small_D_lower += HDPC_left * D_upper");

  // Combine small_A from small_A_lower and small_A_upper
  MatrixGF256 small_A(small_A_upper.rows() + small_A_lower.rows(), small_A_upper.cols());
  small_A.set_from(small_A_upper, 0, 0);
  small_A.set_from(small_A_lower, small_A_upper.rows(), 0);

  // Combine small_D from small_D_lower and small_D_upper
  MatrixGF256 small_D(small_D_upper.rows() + small_D_lower.rows(), small_D_upper.cols());
  small_D.set_from(small_D_upper, 0, 0);
  small_D.set_from(small_D_lower, small_D_upper.rows(), 0);

  TRY_RESULT(small_C, GaussianElimination::run(std::move(small_A), std::move(small_D)));
  perf_log("gauss");

  C.set_from(small_C.block_view(0, 0, C.rows() - U_size, C.cols()), U_size, 0);

  SparseMatrixGF2 A_upper_t = A_upper.transpose();
  for (uint32 row = 0; row < U_size; row++) {
    for (auto col : A_upper_t.col(row)) {
      if (col == row) {
        continue;
      }
      C.row_add(row, col);
    }
  }
  perf_log("Calc result");

  auto res = C.apply_row_permutation(inverse_permutation(col_permutation));
  perf_log("Apply permutation");
  return std::move(res);
}
}  // namespace raptorq
}  // namespace td
