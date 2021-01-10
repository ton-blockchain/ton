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
#include "td/fec/online/Encoder.h"

#include "td/fec/common/SymbolsView.h"

namespace td {
namespace online_code {
Result<std::unique_ptr<Encoder>> Encoder::create(size_t symbol_size, BufferSlice data) {
  return std::make_unique<Encoder>(symbol_size, data.as_slice());
}

Encoder::Encoder(size_t symbol_size, Slice data)
    : parameters_(Rfc::get_parameters((data.size() + symbol_size - 1) / symbol_size).move_as_ok())
    , C_(parameters_.source_blocks_count() + parameters_.outer_encoding_blocks_count(), symbol_size)
    , d_(1, symbol_size)
    , data_size_(data.size()) {
  raptorq::SymbolsView view(parameters_.source_blocks_count() + parameters_.outer_encoding_blocks_count(), symbol_size,
                            data);
  for (auto &symbol : view.symbols()) {
    C_.row_set(symbol.id, symbol.data);
  }
  parameters_.outer_encoding_for_each([&](uint32 i, uint32 j) {
    if (j < parameters_.source_blocks_count()) {
      C_.row_add(i + parameters_.source_blocks_count(), C_.row(j));
    }
  });
}

Encoder::Parameters Encoder::get_parameters() const {
  Parameters p;
  p.symbols_count = parameters_.source_blocks_count();
  p.symbol_size = C_.cols();
  p.data_size = data_size_;
  return p;
}

Status Encoder::gen_symbol(uint32 symbol_id, MutableSlice slice) {
  d_.set_zero();
  for (auto id : parameters_.get_inner_encoding_row(symbol_id)) {
    d_.row_add(0, C_.row(id));
  }
  slice.copy_from(d_.row(0));
  return Status::OK();
}
}  // namespace online_code
}  // namespace td
