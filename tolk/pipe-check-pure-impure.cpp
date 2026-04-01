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
#include "compilation-errors.h"

/*
 *   This pipe checks for impure operations inside pure functions.
 *   It happens after type inferring (after methods binding) since it operates fun_ref of calls.
 *   It does not check for globals modification, it's done in another pipe (see a comment below).
 */

namespace tolk {

static Error err_impure_operation_inside_pure_function() {
  return err("an impure operation in a pure function");
}

class CheckImpureOperationsInPureFunctionVisitor final : public ASTVisitorFunctionBody {

  void visit(V<ast_reference> v) override {
    // `globalVar = rhs` is also denied in pure functions, but it's analyzed not here,
    // it's checked in `pipe-check-rvalue-lvalue.cpp` to detect `(globVar!).0 = rhs` and other
    // (it's the same as detecting modification of immutable variables)
    parent::visit(v);
  }

  void visit(V<ast_function_call> v) override {
    // v is `globalF(args)` / `globalF<int>(args)` / `obj.method(args)` / `local_var(args)` / `getF()(args)`
    // `local_var(args)` is always impure, no considerations about what's there at runtime
    if (!v->fun_maybe || !v->fun_maybe->is_marked_as_pure()) {
      err_impure_operation_inside_pure_function().collect(v, cur_f);
    }

    parent::visit(v);
  }

  void visit(V<ast_throw_statement> v) override {
    err_impure_operation_inside_pure_function().collect(v, cur_f);
  }

  void visit(V<ast_assert_statement> v) override {
    err_impure_operation_inside_pure_function().collect(v, cur_f);
  }

public:
  bool should_visit_function(FunctionPtr fun_ref) override {
    return fun_ref->is_code_function() && !fun_ref->is_generic_function() && fun_ref->is_marked_as_pure();
  }
};

void pipeline_check_pure_impure_operations() {
  CheckImpureOperationsInPureFunctionVisitor visitor;
  visit_ast_of_all_functions(visitor);
}

} // namespace tolk
