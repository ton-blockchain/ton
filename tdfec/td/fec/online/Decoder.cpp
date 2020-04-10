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
#include "td/fec/online/Decoder.h"

namespace td {
namespace online_code {
Result<std::unique_ptr<Decoder>> Decoder::create(Encoder::Parameters p) {
  return std::make_unique<Decoder>(p.symbol_size, p.data_size);
}

Decoder::Decoder(size_t symbol_size, size_t data_size)
    : parameters_(Rfc::get_parameters((data_size + symbol_size - 1) / symbol_size).move_as_ok())
    , symbol_size_(symbol_size)
    , data_size_(data_size) {
  MatrixGF256 d_{1, symbol_size_};
  d_.set_zero();
  std::vector<uint32> e_cnt(parameters_.outer_encoding_blocks_count(), 0);
  std::vector<std::vector<uint32>> e(parameters_.outer_encoding_blocks_count());
  parameters_.outer_encoding_for_each([&](uint32 i, uint32 j) { e_cnt[i]++; });
  for (uint32 i = 0; i < parameters_.outer_encoding_blocks_count(); i++) {
    e[i].reserve(e_cnt[i]);
  }
  parameters_.outer_encoding_for_each([&](uint32 i, uint32 j) { e[i].push_back(j); });
  for (uint32 i = 0; i < parameters_.outer_encoding_blocks_count(); i++) {
    decoding_.add_equation(e[i], d_.row(0));
  }
}

Status Decoder::add_symbol(raptorq::SymbolRef symbol_ref) {
  if (symbol_ref.data.size() != symbol_size_) {
    return Status::Error("Symbol has invalid length");
  }

  auto offset = decoding_.ready_symbols().size();
  decoding_.add_equation(parameters_.get_inner_encoding_row(symbol_ref.id), symbol_ref.data);

  //LOG(ERROR) << inner_decoding_.ready_symbols().size();
  for (auto symbol_id : decoding_.ready_symbols().substr(offset)) {
    if (symbol_id < parameters_.source_blocks_count()) {
      ready_cnt_++;
    }
  }
  return Status::OK();
}

bool Decoder::is_ready() const {
  return ready_cnt_ == parameters_.source_blocks_count();
}

BufferSlice Decoder::get_data() const {
  CHECK(is_ready());
  BufferSlice res(data_size_);
  for (uint32 i = 0; i < parameters_.source_blocks_count(); i++) {
    auto dest = res.as_slice().substr(i * symbol_size_);
    dest.copy_from(decoding_.get_symbol(i).truncate(dest.size()));
  }
  return res;
}

}  // namespace online_code
}  // namespace td
