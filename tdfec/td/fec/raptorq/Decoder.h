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
#include <set>

#include "td/fec/fec.h"
#include "td/fec/raptorq/Encoder.h"
#include "td/fec/raptorq/Solver.h"

namespace td {
namespace raptorq {

class Decoder {
 public:
  static Result<std::unique_ptr<Decoder>> create(Encoder::Parameters p);
  Decoder(const Rfc::Parameters &p, size_t symbol_size, size_t data_size);

  Status add_symbol(fec::Symbol symbol);

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
  std::vector<fec::Symbol> symbols_;
  vector<bool> small_symbols_mask_;
  std::set<uint32> big_symbols_set_;
  size_t data_size_;
  optional<BufferSlice> result_;

  bool made_symbol_refs_{false};
  std::vector<SymbolRef> symbol_refs_;
  BufferSlice zero_symbol_;

  void make_symbol_refs();
};

}  // namespace raptorq
}  // namespace td
