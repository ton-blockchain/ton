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

struct ConstValInt;
struct ConstValBool;
struct ConstValSlice;
struct ConstValAddress;
struct ConstValTensor;
struct ConstValObject;
struct ConstValNullLiteral;

// `const a = 2 + 3` is okay, but `const a = foo()` is not;
// "okay" means "a constant expression", which can be evaluated at compile-time;
// default values of struct fields and enum members are also required to be constant;
// `field: (int, Obj) = (2, {v: true})` is also okay, `(2, {v: true})` is a valid constant expression;
//
// so, every const/enum/param default can be evaluated into ConstValExpression
// and later exported into ABI
typedef std::variant<
  ConstValInt,
  ConstValBool,
  ConstValSlice,
  ConstValAddress,
  ConstValTensor,
  ConstValObject,
  ConstValNullLiteral
> ConstValExpression;

struct ConstValInt {
  td::RefInt256 int_val;
};

struct ConstValBool {
  bool bool_val;
};

struct ConstValSlice {
  std::string str_hex;
};

struct ConstValAddress {
  std::string std_addr_hex;
};

struct ConstValTensor {
  std::vector<AnyExprV> items;
};

struct ConstValObject {
  StructPtr struct_ref;
  std::vector<std::pair<StructFieldPtr, AnyExprV>> fields;
};

struct ConstValNullLiteral {
};

ConstValExpression eval_constant_expression_or_fire(AnyExprV v_expr);
ConstValExpression eval_and_cache_const_init_val(GlobalConstPtr const_ref);
ConstValExpression eval_call_to_compile_time_function(AnyExprV v_call);

std::vector<td::RefInt256> calculate_enum_members_with_values(EnumDefPtr enum_ref);

void check_expression_is_constant_or_fire(AnyExprV v_expr);
std::string eval_string_const_standalone(AnyExprV v_string);

} // namespace tolk
