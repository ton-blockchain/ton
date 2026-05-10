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
#include "ast.h"
#include "ast-replacer.h"
#include "type-system.h"

/*
 *   AST-level normalizations of boolean conditions.
 *   1. `!!boolVar` -> `boolVar`
 *   2. compile-time fold of `!true` / `!false`
 *   3. `boolVar == true`  / `boolVar != false`  ->  `boolVar`
 *      `boolVar != true`  / `boolVar == false`  ->  `!boolVar`
 *   4. `if (!x)`               ->  if(x) with `is_ifnot`
 *   5. `if (x !is null)`       ->  if(x is null) with `is_ifnot`
 *      `if (addr1 != addr2)`   ->  if(addr1 == addr2) with `is_ifnot`
 *
 *   Note: it's an AST-level pass, but it's still needed.
 * At IR level (see `optimize_conditional_branches`) we simplify `(x != 0)` / `(x == 0)` in conditions.
 * But it cannot recognize higher-level shapes and booleans.
 *
 *   Without this pass `if (!x)` compiles to `NOT IFJMP` instead of `IFNOTJMP`,
 * and null checks keep an extra `0 NEQINT`.
 *
 *   Some day, this pass should also be IR-level.
 *
 *   todo some day, replace && || with & | when it's safe (currently, && always produces IFs in Fift)
 * It's tricky to implement whether replacing is safe.
 * For example, safe: `a > 0 && a < 10` / `a != 3 && a != 5`
 * For example, unsafe: `cached && calc()` / `a > 0 && log(a)` / `b != 0 && a / b > 1` / `i >= 0 && arr[idx]` / `f != null && close(f)`
 */

namespace tolk {

class OptimizerBooleanExpressionsReplacer final : public ASTReplacerInFunctionBody {

  static V<ast_bool_const> create_bool_const(SrcRange range, bool bool_val) {
    auto v_bool = createV<ast_bool_const>(range, bool_val);
    v_bool->assign_inferred_type(TypeDataBool::create());
    v_bool->assign_rvalue_true();
    return v_bool;
  }

  static V<ast_unary_operator> create_logical_not_for_bool(SrcRange range, SrcRange operator_range, AnyExprV rhs) {
    auto v_not = createV<ast_unary_operator>(range, operator_range, "!", tok_logical_not, rhs);
    v_not->assign_inferred_type(TypeDataBool::create());
    v_not->assign_rvalue_true();
    v_not->assign_fun_ref(lookup_function("!b_"));
    return v_not;
  }

  static bool expect_boolean(TypePtr inferred_type) {
    if (inferred_type == TypeDataBool::create()) {
      return true;
    }
    if (const TypeDataAlias* as_alias = inferred_type->try_as<TypeDataAlias>()) {
      return expect_boolean(as_alias->underlying_type);
    }
    return false;
  }

  static bool is_compile_time_true_false(AnyExprV cond) {
    return cond->is_always_true || cond->is_always_false;
  }

  AnyExprV replace(V<ast_unary_operator> v) override {
    parent::replace(v);

    if (v->tok == tok_logical_not) {
      // `!!boolVar` => `boolVar` (the inner operand is always bool, integers disallowed by type checker)
      if (auto inner_not = v->get_rhs()->try_as<ast_unary_operator>(); inner_not && inner_not->tok == tok_logical_not) {
        return inner_not->get_rhs();
      }
      // `!true` / `!false`
      if (auto inner_bool = v->get_rhs()->try_as<ast_bool_const>()) {
        return create_bool_const(v->range, !inner_bool->bool_val);
      }
    }

    return v;
  }

  AnyExprV replace(V<ast_binary_operator> v) override {
    parent::replace(v);

    if (v->tok == tok_eq || v->tok == tok_neq) {
      AnyExprV lhs = v->get_lhs();
      AnyExprV rhs = v->get_rhs();
      if (expect_boolean(lhs->inferred_type) && rhs->kind == ast_bool_const) {
        // `boolVar == true` / `boolVar != false`
        if (rhs->as<ast_bool_const>()->bool_val ^ (v->tok == tok_neq)) {
          return lhs;
        }
        // `boolVar != true` / `boolVar == false`
        return create_logical_not_for_bool(v->range, v->operator_range, lhs);
      }
    }

    return v;
  }

  AnyV replace(V<ast_if_statement> v) override {
    parent::replace(v);

    // don't deal with always true/false conditions, they will anyway be erased at compile-time later;
    // because below, we swap v->is_ifnot and is_negated flags, it's hard to keep them in sync
    if (is_compile_time_true_false(v->get_cond())) {
      return v;
    }

    // `if (!x)` -> ifnot(x)
    while (auto v_cond_unary = v->get_cond()->try_as<ast_unary_operator>()) {
      if (v_cond_unary->tok != tok_logical_not) {
        break;
      }
      v = createV<ast_if_statement>(v->range, !v->is_ifnot, v_cond_unary->get_rhs(), v->get_if_body(), v->get_else_body());
    }
    if (is_compile_time_true_false(v->get_cond())) {
      return v;
    }

    // `if (x != null)` -> ifnot(x == null)
    if (auto v_cond_istype = v->get_cond()->try_as<ast_is_type_operator>(); v_cond_istype && v_cond_istype->is_negated) {
      v_cond_istype->mutate()->assign_is_negated(!v_cond_istype->is_negated);
      v = createV<ast_if_statement>(v->range, !v->is_ifnot, v_cond_istype, v->get_if_body(), v->get_else_body());
    }
    // `if (addr1 != addr2)` -> ifnot(addr1 == addr2)
    if (auto v_cond_neq = v->get_cond()->try_as<ast_binary_operator>()) {
      if (v_cond_neq->tok == tok_neq && v_cond_neq->get_lhs()->inferred_type->unwrap_alias()->try_as<TypeDataAddress>() && v_cond_neq->get_rhs()->inferred_type->unwrap_alias()->try_as<TypeDataAddress>()) {
        auto v_cond_eq = createV<ast_binary_operator>(v_cond_neq->range, v_cond_neq->operator_range, "==", tok_eq, v_cond_neq->get_lhs(), v_cond_neq->get_rhs());
        v_cond_eq->mutate()->assign_inferred_type(v_cond_neq->inferred_type);
        v_cond_eq->mutate()->assign_rvalue_true();
        v = createV<ast_if_statement>(v->range, !v->is_ifnot, v_cond_eq, v->get_if_body(), v->get_else_body());
      }
    }

    return v;
  }

public:
  bool should_visit_function(FunctionPtr fun_ref) override {
    return fun_ref->is_code_function() && !fun_ref->is_generic_function();
  }
};

void pipeline_optimize_boolean_expressions() {
  OptimizerBooleanExpressionsReplacer replacer;
  replace_ast_of_all_functions(replacer);
}

} // namespace tolk
