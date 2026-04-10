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
#include "type-system.h"

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

static Error err_invalid_mutate_arg_passed(FunctionPtr fun_ref, const LocalVarData& p_sym, bool arg_passed_as_mutate, AnyV arg_expr) {
  std::string arg_str(arg_expr->kind == ast_reference ? arg_expr->as<ast_reference>()->get_name() : "obj");
  std::string param_name(p_sym.name);

  // built-in functions don't have parameter names, let it be `slice` / `builder` / etc.
  if (param_name.empty()) {
    param_name = p_sym.declared_type->as_human_readable();
  }

  if (p_sym.is_mutate_parameter() && !arg_passed_as_mutate) {
    // called `mutating_function(arg)`; suggest: `mutate arg`
    return err("function `{}` mutates parameter `{}`\nyou need to specify `mutate` when passing an argument, like `mutate {}`", fun_ref, param_name, arg_str);
  } else {
    // called `usual_function(mutate arg)`
    return err("incorrect `mutate`, since `{}` does not mutate parameter `{}`", fun_ref, param_name);
  }
}


void mark_lvalue_AnyV(AnyV v);    // implemented in `pipe-calc-rvalue-lvalue.cpp`

class RefineLvalueForMutateArgumentsVisitor final : public ASTVisitorFunctionBody {

  void visit(V<ast_function_call> v) override {
    // v is `globalF(args)` / `globalF<int>(args)` / `obj.method(args)` / `local_var(args)` / `getF()(args)`
    FunctionPtr fun_ref = v->fun_maybe;
    if (!fun_ref) {
      parent::visit(v);
      for (int i = 0; i < v->get_num_args(); ++i) {
        auto v_arg = v->get_arg(i);
        if (v_arg->passed_as_mutate) {
          err("`mutate` used for non-mutate parameter").collect(v_arg);
        }
      }
      return;
    }

    int delta_self = v->get_self_obj() != nullptr;
    tolk_assert(fun_ref->get_num_params() >= delta_self + v->get_num_args());

    if (delta_self && fun_ref->does_mutate_self()) {
      // for `b.storeInt()`, `b` should become lvalue, since `storeInt` is a method mutating self
      // but: `beginCell().storeInt()`, then `beginCell()` is not lvalue
      // (it will be extracted as tmp var when transforming AST to IR)
      bool will_be_extracted_as_tmp_var = v->get_self_obj()->kind == ast_function_call
              // and allow `StringBuilder{}.append()`,
              // but deny non-empty literals like `Point{x,y}.assign()` to avoid slots aliasing
              || (v->get_self_obj()->kind == ast_object_literal && v->get_self_obj()->as<ast_object_literal>()->get_body()->empty());
      // also deny `b.id().storeInt()` and `beginCell().id().storeInt()` — chained methods are not temporary, they return `self`
      if (auto inner = v->get_self_obj()->try_as<ast_function_call>();
          inner && inner->fun_maybe &&
          inner->fun_maybe->does_return_self() && !inner->fun_maybe->does_mutate_self()) {
        // marking `b.id()` as lvalue will fire "can not mutate a temporary expression" later, it's the goal
        will_be_extracted_as_tmp_var = false;
      }
      if (!will_be_extracted_as_tmp_var) {
        mark_lvalue_AnyV(v->get_self_obj());
      }
    }

    for (int i = 0; i < v->get_num_args(); ++i) {
      const LocalVarData& p_sym = fun_ref->parameters[delta_self + i];
      auto arg_i = v->get_arg(i);
      if (p_sym.is_mutate_parameter() != arg_i->passed_as_mutate) {
        err_invalid_mutate_arg_passed(fun_ref, p_sym, arg_i->passed_as_mutate, arg_i->get_expr()).collect(arg_i, cur_f);
      }
      parent::visit(arg_i);
    }
    parent::visit(v->get_callee());
  }

public:
  bool should_visit_function(FunctionPtr fun_ref) override {
    return fun_ref->is_code_function() && !fun_ref->is_generic_function();
  }
};

void pipeline_refine_lvalue_for_mutate_arguments() {
  RefineLvalueForMutateArgumentsVisitor visitor;
  visit_ast_of_all_functions(visitor);
}

} // namespace tolk
