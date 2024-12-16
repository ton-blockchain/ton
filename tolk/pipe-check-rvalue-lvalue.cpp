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
 *   This pipe checks lvalue/rvalue for validity.
 *   It happens after type inferring (after methods binding) and after lvalue/rvalue are refined based on fun_ref.
 *
 *   Example: `f() = 4`, `f()` was earlier marked as lvalue, it's incorrect.
 *   Example: `f(mutate 5)`, `5` was marked also, it's incorrect.
 */

namespace tolk {

GNU_ATTRIBUTE_NORETURN GNU_ATTRIBUTE_COLD
static void fire_error_cannot_be_used_as_lvalue(AnyV v, const std::string& details) {
  // example: `f() = 32`
  // example: `loadUint(c.beginParse(), 32)` (since `loadUint()` mutates the first argument)
  v->error(details + " can not be used as lvalue");
}

// handle when a function used as rvalue, like `var cb = f`
static void handle_function_used_as_noncall(AnyExprV v, const FunctionData* fun_ref) {
  fun_ref->mutate()->assign_is_used_as_noncall();
  if (!fun_ref->arg_order.empty() || !fun_ref->ret_order.empty()) {
    v->error("saving `" + fun_ref->name + "` into a variable will most likely lead to invalid usage, since it changes the order of variables on the stack");
  }
  if (fun_ref->has_mutate_params()) {
    v->error("saving `" + fun_ref->name + "` into a variable is impossible, since it has `mutate` parameters and thus can only be called directly");
  }
}

class CheckRValueLvalueVisitor final : public ASTVisitorFunctionBody {
  void visit(V<ast_binary_operator> v) override {
    if (v->is_lvalue) {
      fire_error_cannot_be_used_as_lvalue(v, "operator `" + static_cast<std::string>(v->operator_name));
    }
    parent::visit(v);
  }

  void visit(V<ast_unary_operator> v) override {
    if (v->is_lvalue) {
      fire_error_cannot_be_used_as_lvalue(v, "operator `" + static_cast<std::string>(v->operator_name));
    }
    parent::visit(v);
  }

  void visit(V<ast_ternary_operator> v) override {
    if (v->is_lvalue) {
      fire_error_cannot_be_used_as_lvalue(v, "operator ?:");
    }
    parent::visit(v);
  }

  void visit(V<ast_int_const> v) override {
    if (v->is_lvalue) {
      fire_error_cannot_be_used_as_lvalue(v, "literal");
    }
  }

  void visit(V<ast_string_const> v) override {
    if (v->is_lvalue) {
      fire_error_cannot_be_used_as_lvalue(v, "literal");
    }
  }

  void visit(V<ast_bool_const> v) override {
    if (v->is_lvalue) {
      fire_error_cannot_be_used_as_lvalue(v, "literal");
    }
  }

  void visit(V<ast_null_keyword> v) override {
    if (v->is_lvalue) {
      fire_error_cannot_be_used_as_lvalue(v, "literal");
    }
  }

  void visit(V<ast_function_call> v) override {
    if (v->is_lvalue) {
      fire_error_cannot_be_used_as_lvalue(v, "function call");
    }
    if (!v->fun_maybe) {
      parent::visit(v->get_called_f());
    }
    // for `f(...)` don't visit identifier `f`, to detect `f` usage as non-call, like `var cb = f`

    for (int i = 0; i < v->get_num_args(); ++i) {
      parent::visit(v->get_arg(i));
    }
  }

  void visit(V<ast_dot_method_call> v) override {
    if (v->is_lvalue) {
      fire_error_cannot_be_used_as_lvalue(v, "method call");
    }

    parent::visit(v->get_obj());

    for (int i = 0; i < v->get_num_args(); ++i) {
      parent::visit(v->get_arg(i));
    }
  }

  void visit(V<ast_local_var> v) override {
    if (v->marked_as_redef) {
      tolk_assert(v->var_maybe);    // always filled, but for `var g_var redef` might point not to a local
      if (const LocalVarData* var_ref = v->var_maybe->try_as<LocalVarData>(); var_ref && var_ref->is_immutable()) {
        v->error("`redef` for immutable variable");
      }
    }
  }

  void visit(V<ast_identifier> v) override {
    if (v->is_lvalue) {
      tolk_assert(v->sym);
      if (const auto* var_ref = v->sym->try_as<LocalVarData>(); var_ref && var_ref->is_immutable()) {
        v->error("modifying immutable variable `" + var_ref->name + "`");
      } else if (v->sym->try_as<GlobalConstData>()) {
        v->error("modifying immutable constant");
      } else if (v->sym->try_as<FunctionData>()) {
        v->error("function can't be used as lvalue");
      }
    }

    // a reference to a function used as rvalue, like `var v = someFunction`
    if (const FunctionData* fun_ref = v->sym->try_as<FunctionData>(); fun_ref && v->is_rvalue) {
      handle_function_used_as_noncall(v, fun_ref);
    }
  }

  void visit(V<ast_self_keyword> v) override {
    if (v->is_lvalue && v->param_ref->is_immutable()) {
      v->error("modifying `self`, which is immutable by default; probably, you want to declare `mutate self`");
    }
  }

  void visit(V<ast_underscore> v) override {
    if (v->is_rvalue) {
      v->error("`_` can't be used as a value; it's a placeholder for a left side of assignment");
    }
  }

  void visit(V<ast_try_catch_statement> v) override {
    parent::visit(v->get_try_body());
    // skip catch(_,excNo), there are always vars due to grammar, lvalue/rvalue aren't set to them
    parent::visit(v->get_catch_body());
  }
};

void pipeline_check_rvalue_lvalue(const AllSrcFiles& all_src_files) {
  visit_ast_of_all_functions<CheckRValueLvalueVisitor>(all_src_files);
}

} // namespace tolk
