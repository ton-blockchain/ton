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
#include "td/fec/raptorq/RawEncoder.h"
#include "td/fec/common/SymbolsView.h"

#include "td/utils/optional.h"
#include "td/utils/buffer.h"

#include <atomic>

namespace td {
namespace raptorq {

class Encoder {
 public:
  struct Parameters {
    size_t symbols_count;
    size_t symbol_size;
    size_t data_size;
  };

  struct Info {
    uint32 symbol_count;
    uint32 ready_symbol_count;
  };

  static Result<std::unique_ptr<Encoder>> create(size_t symbol_size, BufferSlice data);
  static std::unique_ptr<Encoder> create(size_t symbol_size, RawEncoder raw_encoder);

  Encoder(Rfc::Parameters p, size_t symbol_size, BufferSlice data, optional<RawEncoder> raw_encoder = {});
  Status gen_symbol(uint32 id, MutableSlice slice);
  Parameters get_parameters() const;
  Info get_info() const;

  bool has_precalc() const;

  // Must be called only once. No concurrent calls are allowed.
  // Also it may be and should be called from another thread.
  void precalc();

 private:
  Rfc::Parameters p_;
  size_t symbol_size_;
  BufferSlice data_;
  SymbolsView first_symbols_{p_.K_padded, symbol_size_, data_.as_slice()};

  optional<RawEncoder> raw_encoder_;
  std::atomic<bool> has_encoder_{false};
};
}  // namespace raptorq
}  // namespace td
