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
#include "td/fec/algebra/MatrixGF256.h"

#include "td/utils/buffer.h"

namespace td {
namespace online_code {
class Encoder {
 public:
  struct Parameters {
    size_t symbols_count;
    size_t symbol_size;
    size_t data_size;
  };
  static Result<std::unique_ptr<Encoder>> create(size_t symbol_size, BufferSlice data);

  Encoder(size_t symbol_size, Slice data);
  Parameters get_parameters() const;

  Status gen_symbol(uint32 symbol_id, MutableSlice slice);

 private:
  Rfc::Parameters parameters_;
  MatrixGF256 C_;
  MatrixGF256 d_;
  size_t data_size_;
};
}  // namespace online_code
}  // namespace td
