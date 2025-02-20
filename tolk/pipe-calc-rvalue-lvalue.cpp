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
#include "ast-visitor.h"

/*
 *   This pipe assigns lvalue/rvalue flags for AST expressions.
 *   It happens after identifiers have been resolved, but before type inferring (before methods binding).
 *
 *   Example: `a = b`, `a` is lvalue, `b` is rvalue.
 *   Example: `a + b`, both are rvalue.
 *   Example: `a;`, it's none (not rvalue, not lvalue).
 *   For instance, difference between none and rvalue helps detect whether `match` is expression or statement.
 *
 *   Note, that this pass only assigns, not checks. So, for `f() = 4`, expr `f()` is lvalue.
 * Checking (firing this as incorrect later) is performed after type inferring, see pipe-check-rvalue-lvalue.
 */

namespace tolk {

enum class MarkingState {
  None,
  LValue,
  RValue,
  LValueAndRValue
};

class CalculateRvalueLvalueVisitor final : public ASTVisitorFunctionBody {
  MarkingState cur_state = MarkingState::None;

  MarkingState enter_state(MarkingState activated) {
    MarkingState saved = cur_state;
    cur_state = activated;
    return saved;
  }

  MarkingState enter_rvalue_if_none() {
    if (cur_state != MarkingState::None) {
      return cur_state;
    }
    cur_state = MarkingState::RValue;
    return MarkingState::None;
  }

  void restore_state(MarkingState saved) {
    cur_state = saved;
  }

  void mark_vertex(AnyExprV v) const {
    if (cur_state == MarkingState::LValue || cur_state == MarkingState::LValueAndRValue) {
      v->mutate()->assign_lvalue_true();
    }
    if (cur_state == MarkingState::RValue || cur_state == MarkingState::LValueAndRValue) {
      v->mutate()->assign_rvalue_true();
    }
  }

  void visit(V<ast_empty_expression> v) override {
    mark_vertex(v);
  }
  
  void visit(V<ast_parenthesized_expression> v) override {
    mark_vertex(v);
    MarkingState saved = enter_rvalue_if_none();
    parent::visit(v);
    restore_state(saved);
  }

  void visit(V<ast_braced_expression> v) override {
    mark_vertex(v);
    MarkingState saved = enter_state(MarkingState::None);
    parent::visit(v);
    restore_state(saved);
  }

  void visit(V<ast_tensor> v) override {
    mark_vertex(v);
    MarkingState saved = enter_rvalue_if_none();
    parent::visit(v);
    restore_state(saved);
  }
  
  void visit(V<ast_bracket_tuple> v) override {
    mark_vertex(v);
    MarkingState saved = enter_rvalue_if_none();
    parent::visit(v);
    restore_state(saved);
  }

  void visit(V<ast_reference> v) override {
    mark_vertex(v);
  }

  void visit(V<ast_int_const> v) override {
    mark_vertex(v);
  }

  void visit(V<ast_string_const> v) override {
    mark_vertex(v);
  }

  void visit(V<ast_bool_const> v) override {
    mark_vertex(v);
  }

  void visit(V<ast_null_keyword> v) override {
    mark_vertex(v);
  }

  void visit(V<ast_argument> v) override {
    mark_vertex(v);
    MarkingState saved = enter_state(v->passed_as_mutate ? MarkingState::LValueAndRValue : MarkingState::RValue);
    parent::visit(v);
    restore_state(saved);
  }

  void visit(V<ast_argument_list> v) override {
    tolk_assert(cur_state == MarkingState::RValue);
    mark_vertex(v);
    parent::visit(v);
  }

  void visit(V<ast_dot_access> v) override {
    mark_vertex(v);
    MarkingState saved = enter_state(MarkingState::RValue);
    parent::visit(v->get_obj());
    restore_state(saved);
  }

  void visit(V<ast_function_call> v) override {
    mark_vertex(v);
    MarkingState saved = enter_state(MarkingState::RValue);
    parent::visit(v);
    restore_state(saved);
  }

  void visit(V<ast_underscore> v) override {
    // underscore is a placeholder to ignore left side of assignment: `(a, _) = get2params()`
    // so, if current state is "lvalue", `_` will be marked as lvalue, and ok
    // but if used incorrectly, like `f(_)` or just `_;`, it will be marked rvalue
    // and will fire an error later, in pipe lvalue/rvalue check
    mark_vertex(v);
  }

  void visit(V<ast_assign> v) override {
    mark_vertex(v);
    MarkingState saved = enter_state(MarkingState::LValue);
    parent::visit(v->get_lhs());
    enter_state(MarkingState::RValue);
    parent::visit(v->get_rhs());
    restore_state(saved);
  }

  void visit(V<ast_set_assign> v) override {
    mark_vertex(v);
    MarkingState saved = enter_state(MarkingState::LValueAndRValue);
    parent::visit(v->get_lhs());
    enter_state(MarkingState::RValue);
    parent::visit(v->get_rhs());
    restore_state(saved);
  }

  void visit(V<ast_unary_operator> v) override {
    mark_vertex(v);
    MarkingState saved = enter_state(MarkingState::RValue);
    parent::visit(v);
    restore_state(saved);
  }

  void visit(V<ast_binary_operator> v) override {
    mark_vertex(v);
    MarkingState saved = enter_state(MarkingState::RValue);
    parent::visit(v);
    restore_state(saved);
  }

  void visit(V<ast_ternary_operator> v) override {
    mark_vertex(v);
    MarkingState saved = enter_state(MarkingState::RValue);
    parent::visit(v);  // both cond, when_true and when_false are rvalue, `(cond ? a : b) = 5` prohibited
    restore_state(saved);
  }

  void visit(V<ast_cast_as_operator> v) override {
    mark_vertex(v);
    parent::visit(v->get_expr());   // leave lvalue state unchanged, for `mutate (t.0 as int)` both `t.0 as int` and `t.0` are lvalue
  }

  void visit(V<ast_is_type_operator> v) override {
    mark_vertex(v);
    MarkingState saved = enter_state(MarkingState::RValue);
    parent::visit(v->get_expr());
    restore_state(saved);
  }

  void visit(V<ast_not_null_operator> v) override {
    mark_vertex(v);
    parent::visit(v->get_expr());   // leave lvalue state unchanged, for `mutate x!` both `x!` and `x` are lvalue
  }

  void visit(V<ast_match_expression> v) override {
    mark_vertex(v);
    MarkingState saved = enter_state(MarkingState::RValue);
    parent::visit(v);
    restore_state(saved);
  }

  void visit(V<ast_match_arm> v) override {
    tolk_assert(cur_state == MarkingState::RValue);
    mark_vertex(v);
    parent::visit(v);
  }

  void visit(V<ast_object_field> v) override {
    tolk_assert(cur_state == MarkingState::RValue);
    mark_vertex(v);
    parent::visit(v);
  }

  void visit(V<ast_object_body> v) override {
    tolk_assert(cur_state == MarkingState::RValue);
    mark_vertex(v);
    parent::visit(v);
  }

  void visit(V<ast_object_literal> v) override {
    mark_vertex(v);
    MarkingState saved = enter_state(MarkingState::RValue);
    parent::visit(v);
    restore_state(saved);
  }

  void visit(V<ast_local_var_lhs> v) override {
    tolk_assert(cur_state == MarkingState::LValue);
    mark_vertex(v);
    parent::visit(v);
  }

  void visit(V<ast_local_vars_declaration> v) override {
    tolk_assert(cur_state == MarkingState::LValue);
    mark_vertex(v);
    parent::visit(v);
  }

  void visit(V<ast_block_statement> v) override {
    MarkingState saved = enter_state(MarkingState::None);
    parent::visit(v);
    restore_state(saved);
  }

  void visit(V<ast_return_statement> v) override {
    MarkingState saved = enter_state(MarkingState::RValue);
    parent::visit(v);
    restore_state(saved);
  }

  void visit(V<ast_if_statement> v) override {
    MarkingState saved = enter_state(MarkingState::RValue);
    parent::visit(v);
    restore_state(saved);
  }

  void visit(V<ast_repeat_statement> v) override {
    MarkingState saved = enter_state(MarkingState::RValue);
    parent::visit(v);
    restore_state(saved);
  }

  void visit(V<ast_while_statement> v) override {
    MarkingState saved = enter_state(MarkingState::RValue);
    parent::visit(v);
    restore_state(saved);
  }

  void visit(V<ast_do_while_statement> v) override {
    MarkingState saved = enter_state(MarkingState::RValue);
    parent::visit(v);
    restore_state(saved);
  }

  void visit(V<ast_throw_statement> v) override {
    MarkingState saved = enter_state(MarkingState::RValue);
    parent::visit(v);
    restore_state(saved);
  }

  void visit(V<ast_assert_statement> v) override {
    MarkingState saved = enter_state(MarkingState::RValue);
    parent::visit(v);
    restore_state(saved);
  }

  void visit(V<ast_try_catch_statement> v) override {
    parent::visit(v->get_try_body());
    MarkingState saved = enter_state(MarkingState::LValue);
    parent::visit(v->get_catch_expr());
    restore_state(saved);
    parent::visit(v->get_catch_body());
  }

public:
  bool should_visit_function(FunctionPtr fun_ref) override {
    return fun_ref->is_code_function() && !fun_ref->is_generic_function();
  }
};

void pipeline_calculate_rvalue_lvalue() {
  visit_ast_of_all_functions<CalculateRvalueLvalueVisitor>();
}

void pipeline_calculate_rvalue_lvalue(FunctionPtr fun_ref) {
  CalculateRvalueLvalueVisitor visitor;
  if (visitor.should_visit_function(fun_ref)) {
    visitor.start_visiting_function(fun_ref, fun_ref->ast_root->as<ast_function_declaration>());
  }
}

} // namespace tolk
