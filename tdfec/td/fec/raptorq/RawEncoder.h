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

#include "td/fec/raptorq/Rfc.h"
#include "td/fec/algebra/MatrixGF256.h"

namespace td {
namespace raptorq {
class RawEncoder {
 public:
  RawEncoder(Rfc::Parameters p, MatrixGF256 C) : p_(p), C_(std::move(C)), d_{1, symbol_size()} {
  }

  size_t symbol_size() const {
    return C_.cols();
  }
  void gen_symbol(uint32 id, MutableSlice to) const;

 private:
  Rfc::Parameters p_;
  MatrixGF256 C_;
  mutable MatrixGF256 d_;
};
}  // namespace raptorq
}  // namespace td
