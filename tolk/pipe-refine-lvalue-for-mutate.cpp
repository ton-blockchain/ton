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
 *   This pipe refines rvalue/lvalue and checks `mutate` arguments validity.
 *   It happens after type inferring (after methods binding), because it uses fun_ref of calls.
 *
 *   Example: `a.increment().increment()`, the first `a.increment()` becomes lvalue (assume that increment mutates self).
 *   Example: `increment(a)` is invalid, should be `increment(mutate a)`.
 *
 *   Note, that explicitly specifying `mutate` for arguments, like `increment(mutate a)` is on purpose.
 * If we wished `increment(a)` to be valid (to work and mutate `a`, like passing by ref), it would also be done here,
 * refining `a` to be lvalue. But to avoid unexpected mutations, `mutate` keyword for an argument is required.
 * So, for mutated arguments, instead of setting lvalue, we check its presence.
 */

namespace tolk {

GNU_ATTRIBUTE_NORETURN GNU_ATTRIBUTE_COLD
static void fire_error_invalid_mutate_arg_passed(AnyV v, const FunctionData* fun_ref, const LocalVarData& p_sym, bool called_as_method, bool arg_passed_as_mutate, AnyV arg_expr) {
  std::string arg_str(arg_expr->type == ast_identifier ? arg_expr->as<ast_identifier>()->name : "obj");

  // case: `loadInt(cs, 32)`; suggest: `cs.loadInt(32)`
  if (p_sym.is_mutate_parameter() && !arg_passed_as_mutate && !called_as_method && p_sym.idx == 0 && fun_ref->does_accept_self()) {
    v->error("`" + fun_ref->name + "` is a mutating method; consider calling `" + arg_str + "." + fun_ref->name + "()`, not `" + fun_ref->name + "(" + arg_str + ")`");
  }
  // case: `cs.mutating_function()`; suggest: `mutating_function(mutate cs)` or make it a method
  if (p_sym.is_mutate_parameter() && called_as_method && p_sym.idx == 0 && !fun_ref->does_accept_self()) {
    v->error("function `" + fun_ref->name + "` mutates parameter `" + p_sym.name + "`; consider calling `" + fun_ref->name + "(mutate " + arg_str + ")`, not `" + arg_str + "." + fun_ref->name + "`(); alternatively, rename parameter to `self` to make it a method");
  }
  // case: `mutating_function(arg)`; suggest: `mutate arg`
  if (p_sym.is_mutate_parameter() && !arg_passed_as_mutate) {
    v->error("function `" + fun_ref->name + "` mutates parameter `" + p_sym.name + "`; you need to specify `mutate` when passing an argument, like `mutate " + arg_str + "`");
  }
  // case: `usual_function(mutate arg)`
  if (!p_sym.is_mutate_parameter() && arg_passed_as_mutate) {
    v->error("incorrect `mutate`, since `" + fun_ref->name + "` does not mutate this parameter");
  }
  throw Fatal("unreachable");
}


class RefineLvalueForMutateArgumentsVisitor final : public ASTVisitorFunctionBody {
  void visit(V<ast_function_call> v) override {
    // most likely it's a global function, but also may be `some_var(args)` or even `getF()(args)`
    const FunctionData* fun_ref = v->fun_maybe;
    if (!fun_ref) {
      parent::visit(v);
      for (int i = 0; i < v->get_num_args(); ++i) {
        auto v_arg = v->get_arg(i);
        if (v_arg->passed_as_mutate) {
          v_arg->error("`mutate` used for non-mutate argument");
        }
      }
      return;
    }

    tolk_assert(static_cast<int>(fun_ref->parameters.size()) == v->get_num_args());

    for (int i = 0; i < v->get_num_args(); ++i) {
      const LocalVarData& p_sym = fun_ref->parameters[i];
      auto arg_i = v->get_arg(i);
      if (p_sym.is_mutate_parameter() != arg_i->passed_as_mutate) {
        fire_error_invalid_mutate_arg_passed(arg_i, fun_ref, p_sym, false, arg_i->passed_as_mutate, arg_i->get_expr());
      }
      parent::visit(arg_i);
    }
  }

  void visit(V<ast_dot_method_call> v) override {
    parent::visit(v);

    const FunctionData* fun_ref = v->fun_ref;
    tolk_assert(static_cast<int>(fun_ref->parameters.size()) == 1 + v->get_num_args());

    if (fun_ref->does_mutate_self()) {
      bool will_be_extracted_as_tmp_var = v->get_obj()->type == ast_function_call || v->get_obj()->type == ast_dot_method_call;
      if (!will_be_extracted_as_tmp_var) {
        v->get_obj()->mutate()->assign_lvalue_true();
      }
    }

    if (!fun_ref->does_accept_self() && fun_ref->parameters[0].is_mutate_parameter()) {
      fire_error_invalid_mutate_arg_passed(v, fun_ref, fun_ref->parameters[0], true, false, v->get_obj());
    }

    for (int i = 0; i < v->get_num_args(); ++i) {
      const LocalVarData& p_sym = fun_ref->parameters[1 + i];
      auto arg_i = v->get_arg(i);
      if (p_sym.is_mutate_parameter() != arg_i->passed_as_mutate) {
        fire_error_invalid_mutate_arg_passed(arg_i, fun_ref, p_sym, false, arg_i->passed_as_mutate, arg_i->get_expr());
      }
    }
  }
};

void pipeline_refine_lvalue_for_mutate_arguments(const AllSrcFiles& all_src_files) {
  visit_ast_of_all_functions<RefineLvalueForMutateArgumentsVisitor>(all_src_files);
}

} // namespace tolk
