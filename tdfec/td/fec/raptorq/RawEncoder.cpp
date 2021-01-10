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
#include "td/fec/raptorq/RawEncoder.h"

namespace td {
namespace raptorq {
void RawEncoder::gen_symbol(uint32 id, MutableSlice to) const {
  CHECK(to.size() == symbol_size());
  d_.set_zero();
  p_.encoding_row_for_each(p_.get_encoding_row(id), [&](auto row) { d_.row_add(0, C_.row(row)); });
  to.copy_from(d_.row(0).truncate(symbol_size()));
}
}  // namespace raptorq
}  // namespace td
