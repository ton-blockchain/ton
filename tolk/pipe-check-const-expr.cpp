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
#include "ast-visitor.h"
#include "type-system.h"
#include "constant-evaluator.h"

/*
 *   This pipe checks that expressions expected to be constant, are actually constant.
 *   For example, `const a = 2 + 3` is okay, but `const a = foo()` is not.
 *   For example, field defaults and parameters defaults are also required to be constant.
 *
 *   Also, this pipe calculates and assigns values for every `enum` members.
 */

namespace tolk {

class ConstantExpressionsChecker final : public ASTVisitorFunctionBody {

  void visit(V<ast_function_call> v) override {
    // check `ton("0.05")` and others for correctness (not `ton(local_var)`, etc.)
    if (v->fun_maybe && v->fun_maybe->is_compile_time_const_val()) {
      // on invalid usage, this call will fire
      eval_call_to_compile_time_function(v);
      // note that in AST tree, it's still left as `ton("0.05")`, `stringCrc32("...")`, etc.
      // later, when transforming to IR, such compile-time functions are handled specially
    }

    parent::visit(v);
  }

 void visit(V<ast_match_arm> v) override {
    // check `2 + 3 => ...` (before =>)
    // non-constant expressions like `foo() => ...` fire an error here
    if (v->pattern_kind == MatchArmKind::const_expression) {
      check_expression_is_constant_or_fire(v->get_pattern_expr());
    }

    parent::visit(v);
  }

public:
  bool should_visit_function(FunctionPtr fun_ref) override {
    return fun_ref->is_code_function() && !fun_ref->is_generic_function();
  }
};

void pipeline_check_constant_expressions() {
  // here (after type inferring) check that `const a = 2 + 3` is a valid constant expression
  // non-constant expressions like `const a = foo()` fire an error here
  for (GlobalConstPtr const_ref : get_all_declared_constants()) {
    eval_and_cache_const_init_val(const_ref);
  }
  // do the same for default values of struct fields, they must be constant expressions
  for (StructPtr struct_ref : get_all_declared_structs()) {
    for (StructFieldPtr field_ref : struct_ref->fields) {
      if (field_ref->has_default_value() && !struct_ref->is_generic_struct()) {
        check_expression_is_constant_or_fire(field_ref->default_value);
      }
    }
  }
  // and for default values of parameters
  for (FunctionPtr fun_ref : get_all_not_builtin_functions()) {
    for (int i = 0; i < fun_ref->get_num_params(); ++i) {
      LocalVarPtr param_ref = &fun_ref->get_param(i);
      if (param_ref->has_default_value() && !fun_ref->is_generic_function()) {
        check_expression_is_constant_or_fire(param_ref->default_value);
      }
    }
  }
  
  // assign `enum` members values (either auto-compute sequentially or use manual initializers)
  for (EnumDefPtr enum_ref : get_all_declared_enums()) {
    std::vector<td::RefInt256> values = calculate_enum_members_with_values(enum_ref);
    for (EnumMemberPtr member_ref : enum_ref->members) {
      member_ref->mutate()->assign_computed_value(values[member_ref->member_idx]);
    }
  }

  visit_ast_of_all_functions<ConstantExpressionsChecker>();
}

} // namespace tolk
