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
static void fire_error_impure_operation_inside_pure_function(AnyV v) {
  v->error("an impure operation in a pure function");
}

class CheckImpureOperationsInPureFunctionVisitor final : public ASTVisitorFunctionBody {
  static void fire_if_global_var(AnyExprV v) {
    if (auto v_ident = v->try_as<ast_identifier>()) {
      if (v_ident->sym->try_as<GlobalVarData>()) {
        fire_error_impure_operation_inside_pure_function(v);
      }
    }
  }

  void visit(V<ast_local_var> v) override {
    if (v->marked_as_redef) {
      fire_if_global_var(v->get_identifier());
    }
  }

  void visit(V<ast_binary_operator> v) override {
    if (v->is_set_assign() || v->is_assign()) {
      fire_if_global_var(v->get_lhs());
    }

    parent::visit(v);
  }

  void visit(V<ast_function_call> v) override {
    // most likely it's a global function, but also may be `some_var(args)` or even `getF()(args)`
    if (!v->fun_maybe) {
      // calling variables is always impure, no considerations about what's there at runtime
      fire_error_impure_operation_inside_pure_function(v);
    }

    if (!v->fun_maybe->is_marked_as_pure()) {
      fire_error_impure_operation_inside_pure_function(v);
    }

    parent::visit(v);
  }

  void visit(V<ast_dot_method_call> v) override {
    if (!v->fun_ref->is_marked_as_pure()) {
      fire_error_impure_operation_inside_pure_function(v);
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
    fire_error_impure_operation_inside_pure_function(v);
  }

  void visit(V<ast_assert_statement> v) override {
    fire_error_impure_operation_inside_pure_function(v);
  }

public:
  void start_visiting_function(V<ast_function_declaration> v_function) override {
    if (v_function->marked_as_pure) {
      parent::visit(v_function->get_body());
    }
  }
};

void pipeline_check_pure_impure_operations(const AllSrcFiles& all_src_files) {
  visit_ast_of_all_functions<CheckImpureOperationsInPureFunctionVisitor>(all_src_files);
}

} // namespace tolk
