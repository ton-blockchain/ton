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
#include "td/fec/raptorq/Solver.h"
#include "td/fec/raptorq/Encoder.h"

#include <set>

namespace td {
namespace raptorq {

class Decoder {
 public:
  static Result<std::unique_ptr<Decoder>> create(Encoder::Parameters p);
  Decoder(const Rfc::Parameters &p, size_t symbol_size, size_t data_size);

  Status add_symbol(SymbolRef symbol);

  struct DataWithEncoder {
    BufferSlice data;
    std::unique_ptr<Encoder> encoder;
  };
  Result<DataWithEncoder> try_decode(bool need_encoder);
  bool may_try_decode() const;

 private:
  Rfc::Parameters p_;
  size_t symbol_size_;

  bool may_decode_{false};
  vector<bool> mask_;
  size_t mask_size_{0};
  BufferSlice data_;
  size_t data_size_;

  bool flush_symbols_{false};
  bool slow_path_{false};
  size_t slow_symbols_;
  BufferSlice buffer_;
  std::vector<SymbolRef> symbols_;
  std::set<uint32> slow_symbols_set_;
  std::string zero_symbol_;

  void add_small_symbol(SymbolRef symbol);
  void add_big_symbol(SymbolRef symbol);

  void update_may_decode();

  void on_first_slow_path();

  void flush_symbols();
};

}  // namespace raptorq
}  // namespace td
