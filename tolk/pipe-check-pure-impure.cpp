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
#include "platform-utils.h"

/*
 *   This pipe checks for impure operations inside pure functions.
 *   It happens after type inferring (after methods binding) since it operates fun_ref of calls.
 */

namespace tolk {

GNU_ATTRIBUTE_NORETURN GNU_ATTRIBUTE_COLD
static void fire_error_impure_operation_inside_pure_function(FunctionPtr cur_f, SrcLocation loc) {
  fire(cur_f, loc, "an impure operation in a pure function");
}

class CheckImpureOperationsInPureFunctionVisitor final : public ASTVisitorFunctionBody {
  FunctionPtr cur_f = nullptr;

  void fire_if_global_var(AnyExprV v) const {
    if (auto v_ident = v->try_as<ast_reference>()) {
      if (v_ident->sym->try_as<GlobalVarPtr>()) {
        fire_error_impure_operation_inside_pure_function(cur_f, v->loc);
      }
    }
  }

  void visit(V<ast_assign> v) override {
    fire_if_global_var(v->get_lhs());
    parent::visit(v);
  }

  void visit(V<ast_set_assign> v) override {
    fire_if_global_var(v->get_lhs());
    parent::visit(v);
  }

  void visit(V<ast_function_call> v) override {
    // v is `globalF(args)` / `globalF<int>(args)` / `obj.method(args)` / `local_var(args)` / `getF()(args)`
    if (!v->fun_maybe) {
      // `local_var(args)` is always impure, no considerations about what's there at runtime
      fire_error_impure_operation_inside_pure_function(cur_f, v->loc);
    }

    if (!v->fun_maybe->is_marked_as_pure()) {
      fire_error_impure_operation_inside_pure_function(cur_f, v->loc);
    }

    parent::visit(v);
  }

  void visit(V<ast_argument> v) override {
    if (v->passed_as_mutate) {
      fire_if_global_var(v->get_expr());
    }

    parent::visit(v);
  }

  void visit(V<ast_throw_statement> v) override {
    fire_error_impure_operation_inside_pure_function(cur_f, v->loc);
  }

  void visit(V<ast_assert_statement> v) override {
    fire_error_impure_operation_inside_pure_function(cur_f, v->loc);
  }

public:
  bool should_visit_function(FunctionPtr fun_ref) override {
    return fun_ref->is_code_function() && !fun_ref->is_generic_function() && fun_ref->is_marked_as_pure();
  }

  void start_visiting_function(FunctionPtr fun_ref, V<ast_function_declaration> v_function) override {
    cur_f = fun_ref;
    parent::visit(v_function->get_body());
  }
};

void pipeline_check_pure_impure_operations() {
  visit_ast_of_all_functions<CheckImpureOperationsInPureFunctionVisitor>();
}

} // namespace tolk
