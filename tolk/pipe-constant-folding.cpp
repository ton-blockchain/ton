/*
    This file is part of TON Blockchain source code.

    TON Blockchain is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation; either version 2
    of the License, or (at your option) any later version.

    TON Blockchain is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with TON Blockchain.  If not, see <http://www.gnu.org/licenses/>.
*/
#include "tolk.h"
#include "ast.h"
#include "ast-replacer.h"
#include "type-system.h"

/*
 *   This pipe is supposed to do constant folding, like replacing `2 + 3` with `5`.
 *   It happens after type inferring and validity checks, one of the last ones.
 *
 *   Currently, it just replaces `-1` (ast_unary_operator ast_int_const) with a number -1
 * and `!true` with false.
 *   More rich constant folding should be done some day, but even without this, IR optimizations
 * (operating low-level stack variables) pretty manage to do all related optimizations.
 * Constant folding in the future, done at AST level, just would slightly reduce amount of work for optimizer.
 */

namespace tolk {

class ConstantFoldingReplacer final : public ASTReplacerInFunctionBody {
  static V<ast_int_const> create_int_const(SrcLocation loc, td::RefInt256&& intval) {
    auto v_int = createV<ast_int_const>(loc, std::move(intval), {});
    v_int->assign_inferred_type(TypeDataInt::create());
    v_int->assign_rvalue_true();
    return v_int;
  }

  static V<ast_bool_const> create_bool_const(SrcLocation loc, bool bool_val) {
    auto v_bool = createV<ast_bool_const>(loc, bool_val);
    v_bool->assign_inferred_type(TypeDataBool::create());
    v_bool->assign_rvalue_true();
    return v_bool;
  }

  AnyExprV replace(V<ast_unary_operator> v) override {
    parent::replace(v);

    TokenType t = v->tok;
    // convert "-1" (tok_minus tok_int_const) to a const -1
    if (t == tok_minus && v->get_rhs()->type == ast_int_const) {
      td::RefInt256 intval = v->get_rhs()->as<ast_int_const>()->intval;
      tolk_assert(!intval.is_null());
      intval = -intval;
      if (intval.is_null() || !intval->signed_fits_bits(257)) {
        v->error("integer overflow");
      }
      return create_int_const(v->loc, std::move(intval));
    }
    // same for "+1"
    if (t == tok_plus && v->get_rhs()->type == ast_int_const) {
      return v->get_rhs();
    }

    // `!true` / `!false`
    if (t == tok_logical_not && v->get_rhs()->type == ast_bool_const) {
      return create_bool_const(v->loc, !v->get_rhs()->as<ast_bool_const>()->bool_val);
    }
    // `!0`
    if (t == tok_logical_not && v->get_rhs()->type == ast_int_const) {
      return create_bool_const(v->loc, v->get_rhs()->as<ast_int_const>()->intval == 0);
    }

    return v;
  }

public:
  bool should_visit_function(const FunctionData* fun_ref) override {
    return fun_ref->is_code_function() && !fun_ref->is_generic_function();
  }
};

void pipeline_constant_folding() {
  replace_ast_of_all_functions<ConstantFoldingReplacer>();
}

} // namespace tolk
