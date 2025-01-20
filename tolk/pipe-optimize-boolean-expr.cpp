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
 *   This pipe does some optimizations related to booleans.
 *   It happens after type inferring, when we know types of all expressions.
 *
 *   Example: `boolVar == true` -> `boolVar`.
 *   Example: `!!boolVar` -> `boolVar`.
 *   Also in unwraps parenthesis inside if condition and similar: `assert(((x)), 404)` -> `assert(x, 404)`
 *
 *   todo some day, replace && || with & | when it's safe (currently, && always produces IFs in Fift)
 * It's tricky to implement whether replacing is safe.
 * For example, safe: `a > 0 && a < 10` / `a != 3 && a != 5`
 * For example, unsafe: `cached && calc()` / `a > 0 && log(a)` / `b != 0 && a / b > 1` / `i >= 0 && arr[idx]` / `f != null && close(f)`
 */

namespace tolk {

static AnyExprV unwrap_parenthesis(AnyExprV v) {
  while (v->type == ast_parenthesized_expression) {
    v = v->as<ast_parenthesized_expression>()->get_expr();
  }
  return v;
}

struct OptimizerBooleanExpressionsReplacer final : ASTReplacerInFunctionBody {
  static V<ast_int_const> create_int_const(SrcLocation loc, td::RefInt256&& intval) {
    auto v_int = createV<ast_int_const>(loc, std::move(intval), {});
    v_int->assign_inferred_type(TypeDataInt::create());
    v_int->assign_rvalue_true();
    return v_int;
  }

  static V<ast_bool_const> create_bool_const(SrcLocation loc, bool bool_val) {
    auto v_bool = createV<ast_bool_const>(loc, bool_val);
    v_bool->assign_inferred_type(TypeDataInt::create());
    v_bool->assign_rvalue_true();
    return v_bool;
  }

  static V<ast_unary_operator> create_logical_not_for_bool(SrcLocation loc, AnyExprV rhs) {
    auto v_not = createV<ast_unary_operator>(loc, "!", tok_logical_not, rhs);
    v_not->assign_inferred_type(TypeDataBool::create());
    v_not->assign_rvalue_true();
    v_not->assign_fun_ref(lookup_global_symbol("!b_")->as<FunctionData>());
    return v_not;
  }

protected:

  AnyExprV replace(V<ast_unary_operator> v) override {
    parent::replace(v);

    if (v->tok == tok_logical_not) {
      if (auto inner_not = v->get_rhs()->try_as<ast_unary_operator>(); inner_not && inner_not->tok == tok_logical_not) {
        AnyExprV cond_not_not = inner_not->get_rhs();
        // `!!boolVar` => `boolVar`
        if (cond_not_not->inferred_type == TypeDataBool::create()) {
          return cond_not_not;
        }
        // `!!intVar` => `intVar != 0`
        if (cond_not_not->inferred_type == TypeDataInt::create()) {
          auto v_zero = create_int_const(v->loc, td::make_refint(0));
          auto v_neq = createV<ast_binary_operator>(v->loc, "!=", tok_neq, cond_not_not, v_zero);
          v_neq->mutate()->assign_rvalue_true();
          v_neq->mutate()->assign_inferred_type(TypeDataBool::create());
          v_neq->mutate()->assign_fun_ref(lookup_global_symbol("_!=_")->as<FunctionData>());
          return v_neq;
        }
      }
      if (auto inner_bool = v->get_rhs()->try_as<ast_bool_const>()) {
        // `!true` / `!false`
        return create_bool_const(v->loc, !inner_bool->bool_val);
      }
    }

    return v;
  }

  AnyExprV replace(V<ast_binary_operator> v) override {
    parent::replace(v);

    if (v->tok == tok_eq || v->tok == tok_neq) {
      AnyExprV lhs = v->get_lhs();
      AnyExprV rhs = v->get_rhs();
      if (lhs->inferred_type == TypeDataBool::create() && rhs->type == ast_bool_const) {
        // `boolVar == true` / `boolVar != false`
        if (rhs->as<ast_bool_const>()->bool_val ^ (v->tok == tok_neq)) {
          return lhs;
        }
        // `boolVar != true` / `boolVar == false`
        return create_logical_not_for_bool(v->loc, lhs);
      }
    }

    return v;
  }

  AnyV replace(V<ast_if_statement> v) override {
    parent::replace(v);
    if (v->get_cond()->type == ast_parenthesized_expression) {
      v = createV<ast_if_statement>(v->loc, v->is_ifnot, unwrap_parenthesis(v->get_cond()), v->get_if_body(), v->get_else_body());
    }

    // `if (!x)` -> ifnot(x)
    while (auto v_cond_unary = v->get_cond()->try_as<ast_unary_operator>()) {
      if (v_cond_unary->tok != tok_logical_not) {
        break;
      }
      v = createV<ast_if_statement>(v->loc, !v->is_ifnot, v_cond_unary->get_rhs(), v->get_if_body(), v->get_else_body());
    }

    return v;
  }

  AnyV replace(V<ast_while_statement> v) override {
    parent::replace(v);

    if (v->get_cond()->type == ast_parenthesized_expression) {
      v = createV<ast_while_statement>(v->loc, unwrap_parenthesis(v->get_cond()), v->get_body());
    }
    return v;
  }

  AnyV replace(V<ast_do_while_statement> v) override {
    parent::replace(v);

    if (v->get_cond()->type == ast_parenthesized_expression) {
      v = createV<ast_do_while_statement>(v->loc,  v->get_body(), unwrap_parenthesis(v->get_cond()));
    }
    return v;
  }

  AnyV replace(V<ast_assert_statement> v) override {
    parent::replace(v);

    if (v->get_cond()->type == ast_parenthesized_expression) {
      v = createV<ast_assert_statement>(v->loc, unwrap_parenthesis(v->get_cond()), v->get_thrown_code());
    }
    return v;
  }

public:
  bool should_visit_function(const FunctionData* fun_ref) override {
    return fun_ref->is_code_function() && !fun_ref->is_generic_function();
  }
};

void pipeline_optimize_boolean_expressions() {
  replace_ast_of_all_functions<OptimizerBooleanExpressionsReplacer>();
}

} // namespace tolk
