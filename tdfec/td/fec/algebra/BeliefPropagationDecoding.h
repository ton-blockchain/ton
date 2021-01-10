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
#include "td/utils/buffer.h"
#include "td/utils/Span.h"
#include "td/fec/algebra/MatrixGF256.h"

namespace td {
class BeliefPropagationDecoding {
 public:
  explicit BeliefPropagationDecoding(size_t symbols_count, size_t symbol_size);

  void add_equation(Span<uint32> symbol_ids, Slice data);

  bool is_ready() const;
  Span<uint32> ready_symbols() const;
  Slice get_symbol(uint32 symbol_id) const;

 private:
  struct SymbolInfo {
    bool is_ready{false};
    uint32 head_ = 0;
  };

  struct EquationInfo {
    uint32 symbols_xor{0};
    uint32 symbols_count{0};
  };

  size_t max_equation_count_;
  MatrixGF256 C_;
  MatrixGF256 D_;
  std::vector<SymbolInfo> symbols_;
  std::vector<EquationInfo> equations_;
  std::vector<uint32> ready_equations_;
  std::vector<uint32> ready_symbols_;

  struct Edge {
    uint32 value;
    uint32 next;
  };
  std::vector<Edge> edges_;

  void loop();
};
}  // namespace td
