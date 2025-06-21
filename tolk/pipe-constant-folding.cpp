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
#include "constant-evaluator.h"

/*
 *   This pipe is supposed to do constant folding, like replacing `2 + 3` with `5`.
 *   It happens after type inferring and validity checks, one of the last ones.
 *
 *   Currently, it just replaces `-1` (ast_unary_operator ast_int_const) with a number -1
 * and `!true` with false.
 *   Also, all parenthesized `((expr))` are replaced with `expr`, it's a constant transformation.
 * (not to handle parenthesized in optimization passes, like `((x)) == true`)
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

  AnyExprV replace(V<ast_parenthesized_expression> v) override {
    AnyExprV inner = parent::replace(v->get_expr());
    if (v->is_lvalue) {
      inner->mutate()->assign_lvalue_true();
    }
    return inner;
  }

  static V<ast_string_const> create_string_const(SrcLocation loc, std::string&& literal_value, TypePtr inferred_type) {
    auto v_string = createV<ast_string_const>(loc, literal_value);
    v_string->assign_inferred_type(inferred_type);
    v_string->assign_literal_value(std::move(literal_value));
    v_string->assign_rvalue_true();
    return v_string;
  }

  AnyExprV replace(V<ast_unary_operator> v) override {
    parent::replace(v);

    TokenType t = v->tok;
    // convert "-1" (tok_minus tok_int_const) to a const -1
    if (t == tok_minus && v->get_rhs()->kind == ast_int_const) {
      td::RefInt256 intval = v->get_rhs()->as<ast_int_const>()->intval;
      tolk_assert(!intval.is_null());
      intval = -intval;
      if (intval.is_null() || !intval->signed_fits_bits(257)) {
        v->error("integer overflow");
      }
      return create_int_const(v->loc, std::move(intval));
    }
    // same for "+1"
    if (t == tok_plus && v->get_rhs()->kind == ast_int_const) {
      return v->get_rhs();
    }

    // `!true` / `!false`
    if (t == tok_logical_not && v->get_rhs()->kind == ast_bool_const) {
      return create_bool_const(v->loc, !v->get_rhs()->as<ast_bool_const>()->bool_val);
    }
    // `!0`
    if (t == tok_logical_not && v->get_rhs()->kind == ast_int_const) {
      return create_bool_const(v->loc, v->get_rhs()->as<ast_int_const>()->intval == 0);
    }

    return v;
  }

  AnyExprV replace(V<ast_is_type_operator> v) override {
    parent::replace(v);

    // `null == null` / `null != null`
    if (v->get_expr()->kind == ast_null_keyword && v->type_node->resolved_type == TypeDataNullLiteral::create()) {
      return create_bool_const(v->loc, !v->is_negated);
    }

    return v;
  }

  AnyExprV replace(V<ast_function_call> v) override {
    parent::replace(v);

    // replace `ton("0.05")` with 50000000 / `stringCrc32("some_str")` with calculated value / etc.
    if (v->fun_maybe && v->fun_maybe->is_compile_time_const_val()) {
      CompileTimeFunctionResult value = eval_call_to_compile_time_function(v);
      if (std::holds_alternative<td::RefInt256>(value)) {
        return create_int_const(v->loc, std::move(std::get<td::RefInt256>(value)));
      } else {
        return create_string_const(v->loc, std::move(std::get<std::string>(value)), v->fun_maybe->declared_return_type);
      }
    }

    return v;
  }

  AnyExprV replace(V<ast_string_const> v) override {
    // when "some_str" occurs as a standalone constant (not inside `stringCrc32("some_str")`,
    // it's actually a slice
    std::string literal_value = eval_string_const_standalone(v);
    v->mutate()->assign_literal_value(std::move(literal_value));

    return v;
  }

 AnyExprV replace(V<ast_match_arm> v) override {
    parent::replace(v);

    // replace `2 + 3 => ...` with `5 => ...`
    // non-constant expressions like `foo() => ...` fire an error here
    if (v->pattern_kind == MatchArmKind::const_expression && v->get_pattern_expr()->kind != ast_int_const) {
      check_expression_is_constant(v->get_pattern_expr());
    }

    return v;
  }

public:
  bool should_visit_function(FunctionPtr fun_ref) override {
    return fun_ref->is_code_function() && !fun_ref->is_generic_function();
  }

  void start_replacing_in_function(FunctionPtr fun_ref, V<ast_function_declaration> v_function) override {
    // visit default values of parameters
    for (int i = 0; i < fun_ref->get_num_params(); ++i) {
      if (LocalVarPtr param_ref = &fun_ref->get_param(i); param_ref->has_default_value()) {
        check_expression_is_constant(param_ref->default_value);
        AnyExprV replaced = replace_in_expression(param_ref->default_value);
        param_ref->mutate()->assign_default_value(replaced);
      }
    }

    parent::replace(v_function->get_body());
  }

  // used to replace `ton("0.05")` and other compile-time functions inside fields defaults, etc.
  AnyExprV replace_in_expression(AnyExprV init_value) {
    return parent::replace(init_value);
  }
};

void pipeline_constant_folding() {
  ConstantFoldingReplacer replacer;

  // here (after type inferring) check that `const a = 2 + 3` is a valid constant expression
  // non-constant expressions like `const a = foo()` fire an error here
  // also, replace `const a = ton("0.05")` with `const a = 50000000`
  for (GlobalConstPtr const_ref : get_all_declared_constants()) {
    check_expression_is_constant(const_ref->init_value);
    AnyExprV replaced = replacer.replace_in_expression(const_ref->init_value);
    const_ref->mutate()->assign_init_value(replaced);
  }
  // do the same for default values of struct fields, they must be constant expressions
  for (StructPtr struct_ref : get_all_declared_structs()) {
    for (StructFieldPtr field_ref : struct_ref->fields) {
      if (field_ref->has_default_value() && !struct_ref->is_generic_struct()) {
        check_expression_is_constant(field_ref->default_value);
        AnyExprV replaced = replacer.replace_in_expression(field_ref->default_value);
        field_ref->mutate()->assign_default_value(replaced);
      }
    }
  }

  replace_ast_of_all_functions<ConstantFoldingReplacer>();
}

} // namespace tolk
