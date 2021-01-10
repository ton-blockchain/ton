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
#include "td/fec/online/Rfc.h"
#include "td/fec/common/SymbolRef.h"
#include "td/fec/algebra/BeliefPropagationDecoding.h"

#include "td/fec/online/Encoder.h"

namespace td {
namespace online_code {
class Decoder {
 public:
  static Result<std::unique_ptr<Decoder>> create(Encoder::Parameters p);

  Decoder(size_t symbol_size, size_t data_size);
  Status add_symbol(raptorq::SymbolRef symbol_ref);

  bool is_ready() const;

  BufferSlice get_data() const;

 private:
  Rfc::Parameters parameters_;
  size_t symbol_size_;
  size_t data_size_;
  size_t ready_cnt_{0};

  BeliefPropagationDecoding decoding_{parameters_.source_blocks_count() + parameters_.outer_encoding_blocks_count(),
                                      symbol_size_};
};
}  // namespace online_code
}  // namespace td
