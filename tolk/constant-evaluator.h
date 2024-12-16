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
*/
#pragma once

#include "fwd-declarations.h"
#include "crypto/common/refint.h"
#include <variant>

namespace tolk {

struct ConstantValue {
  std::variant<td::RefInt256, std::string> value;

  bool is_int() const { return std::holds_alternative<td::RefInt256>(value); }
  bool is_slice() const { return std::holds_alternative<std::string>(value); }

  td::RefInt256 as_int() const { return std::get<td::RefInt256>(value); }
  const std::string& as_slice() const { return std::get<std::string>(value); }

  static ConstantValue from_int(int value) {
    return {td::make_refint(value)};
  }

  static ConstantValue from_int(td::RefInt256 value) {
    return {std::move(value)};
  }
};

ConstantValue eval_const_init_value(AnyExprV init_value);

} // namespace tolk
