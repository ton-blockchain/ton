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
#include "td/fec/common/SymbolsView.h"
namespace td {
namespace raptorq {
SymbolsView::SymbolsView(size_t symbols_count, size_t symbol_size, Slice data) {
  symbols_.reserve(symbols_count);
  zero_symbol_ = std::string(symbol_size, '\0');
  last_symbol_ = std::string(symbol_size, '\0');

  for (uint32 symbol_i = 0; symbol_i < symbols_count; symbol_i++) {
    auto offset = symbol_i * symbol_size;
    Slice symbol;
    if (offset < data.size()) {
      symbol = data.substr(offset).truncate(symbol_size);
    }
    Slice slice;
    if (symbol.empty()) {
      slice = zero_symbol_;
    } else if (symbol.size() < symbol_size) {
      MutableSlice(last_symbol_).copy_from(symbol);
      slice = last_symbol_;
    } else {
      slice = symbol;
    }
    symbols_.push_back(SymbolRef{symbol_i, slice});
  }
}
}  // namespace raptorq
}  // namespace td
