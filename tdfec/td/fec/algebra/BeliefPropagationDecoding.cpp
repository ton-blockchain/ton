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
#include "td/fec/algebra/BeliefPropagationDecoding.h"

namespace td {
BeliefPropagationDecoding::BeliefPropagationDecoding(size_t symbols_count, size_t symbol_size)
    : max_equation_count_{static_cast<size_t>(static_cast<double>(symbols_count) * 1.1 + 5)}
    , C_{symbols_count, symbol_size}
    , D_{max_equation_count_, symbol_size} {
  equations_.reserve(max_equation_count_);
  symbols_.resize(symbols_count);

  edges_.resize(1);
}

Slice BeliefPropagationDecoding::get_symbol(uint32 symbol_id) const {
  CHECK(symbols_[symbol_id].is_ready);
  return C_.row(symbol_id);
}

void BeliefPropagationDecoding::add_equation(Span<uint32> symbol_ids, Slice data) {
  if (equations_.size() >= D_.rows()) {
    MatrixGF256 new_D(D_.rows() * 2, D_.cols());
    new_D.set_from(D_, 0, 0);
    D_ = std::move(new_D);
  }
  CHECK(symbol_ids.size() != 0);

  uint32 equation_id = static_cast<uint32>(equations_.size());
  D_.row_set(equation_id, data);

  EquationInfo equation;
  for (auto symbol_id : symbol_ids) {
    CHECK(symbol_id < symbols_.size());
    auto &symbol = symbols_[symbol_id];
    if (symbol.is_ready) {
      D_.row_add(equation_id, C_.row(symbol_id));
    } else {
      equation.symbols_xor ^= symbol_id;
      equation.symbols_count++;

      edges_.push_back({equation_id, symbol.head_});
      symbol.head_ = uint32(edges_.size() - 1);
    }
  }

  if (equation.symbols_count == 0) {
    return;
  }

  equations_.push_back(equation);
  if (equation.symbols_count == 1) {
    ready_equations_.push_back(equation_id);
    loop();
  }
}
bool BeliefPropagationDecoding::is_ready() const {
  return ready_symbols().size() == C_.rows();
}

Span<uint32> BeliefPropagationDecoding::ready_symbols() const {
  return ready_symbols_;
}

void BeliefPropagationDecoding::loop() {
  while (!is_ready() && !ready_equations_.empty()) {
    auto equation_id = ready_equations_.back();
    ready_equations_.pop_back();
    auto &equation = equations_[equation_id];
    LOG_CHECK(equation.symbols_count <= 1) << equation.symbols_count;
    if (equation.symbols_count == 0) {
      continue;
    }
    auto symbol_id = equation.symbols_xor;
    auto &symbol = symbols_[symbol_id];
    LOG_CHECK(symbol_id < symbols_.size())
        << equation.symbols_xor << " " << equation.symbols_count << " " << equation_id;
    if (symbol.is_ready) {
      continue;
    }
    C_.row_set(symbol_id, D_.row(equation_id));
    symbol.is_ready = true;
    ready_symbols_.push_back(symbol_id);
    for (auto i = symbol.head_; i != 0;) {
      auto &edge = edges_[i];
      auto next_equation_id = edge.value;
      i = edge.next;
      D_.row_add(next_equation_id, C_.row(symbol_id));
      auto &next_equation = equations_[next_equation_id];
      next_equation.symbols_xor ^= symbol_id;
      next_equation.symbols_count--;
      if (next_equation.symbols_count == 1) {
        ready_equations_.push_back(next_equation_id);
      }
    }
  }
}
}  // namespace td
