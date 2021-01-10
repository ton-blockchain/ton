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

#include "FecHelper.h"

#include "td/utils/check.h"

namespace ton {
namespace rldp2 {
td::uint32 FecHelper::get_fec_symbols_count() const {
  constexpr td::uint32 x = 5;
  constexpr td::uint32 y = 5;
  // smallest (symbols_count + x + y * i) >  received_symbols_count
  if (symbols_count + x > received_symbols_count) {
    return symbols_count + x;
  }
  td::uint32 i = (received_symbols_count - (symbols_count + x)) / y + 1;
  return symbols_count + x + i * y;
}

td::uint32 FecHelper::get_left_fec_symbols_count() const {
  auto fec_symbols_count = get_fec_symbols_count();
  CHECK(fec_symbols_count > received_symbols_count);
  return fec_symbols_count - received_symbols_count;
}
}  // namespace rldp2
}  // namespace ton
