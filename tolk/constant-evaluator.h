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

typedef std::variant<td::RefInt256, std::string> CompileTimeFunctionResult;

void check_expression_is_constant(AnyExprV v_expr);
std::string eval_string_const_standalone(AnyExprV v_string);
CompileTimeFunctionResult eval_call_to_compile_time_function(AnyExprV v_call);

} // namespace tolk
