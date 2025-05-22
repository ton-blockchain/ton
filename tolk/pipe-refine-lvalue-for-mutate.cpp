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
static void fire_error_invalid_mutate_arg_passed(FunctionPtr cur_f, SrcLocation loc, FunctionPtr fun_ref, const LocalVarData& p_sym, bool arg_passed_as_mutate, AnyV arg_expr) {
  std::string arg_str(arg_expr->kind == ast_reference ? arg_expr->as<ast_reference>()->get_name() : "obj");

  if (p_sym.is_mutate_parameter() && !arg_passed_as_mutate) {
    // called `mutating_function(arg)`; suggest: `mutate arg`
    fire(cur_f, loc, "function `" + fun_ref->as_human_readable() + "` mutates parameter `" + p_sym.name + "`\nyou need to specify `mutate` when passing an argument, like `mutate " + arg_str + "`");
  } else {
    // called `usual_function(mutate arg)`
    fire(cur_f, loc, "incorrect `mutate`, since `" + fun_ref->as_human_readable() + "` does not mutate this parameter");
  }
}


class RefineLvalueForMutateArgumentsVisitor final : public ASTVisitorFunctionBody {
  FunctionPtr cur_f = nullptr;

  void visit(V<ast_function_call> v) override {
    // v is `globalF(args)` / `globalF<int>(args)` / `obj.method(args)` / `local_var(args)` / `getF()(args)`
    FunctionPtr fun_ref = v->fun_maybe;
    if (!fun_ref) {
      parent::visit(v);
      for (int i = 0; i < v->get_num_args(); ++i) {
        auto v_arg = v->get_arg(i);
        if (v_arg->passed_as_mutate) {
          v_arg->error("`mutate` used for non-mutate parameter");
        }
      }
      return;
    }

    int delta_self = v->get_self_obj() != nullptr;
    tolk_assert(fun_ref->get_num_params() == delta_self + v->get_num_args());

    if (delta_self && fun_ref->does_mutate_self()) {
      // for `b.storeInt()`, `b` should become lvalue, since `storeInt` is a method mutating self
      // but: `beginCell().storeInt()`, then `beginCell()` is not lvalue
      // (it will be extracted as tmp var when transforming AST to IR)
      AnyExprV leftmost_obj = v->get_self_obj();
      while (true) {
        if (auto as_par = leftmost_obj->try_as<ast_parenthesized_expression>()) {
          leftmost_obj = as_par->get_expr();
        } else if (auto as_cast = leftmost_obj->try_as<ast_cast_as_operator>()) {
          leftmost_obj = as_cast->get_expr();
        } else if (auto as_nn = leftmost_obj->try_as<ast_not_null_operator>()) {
          leftmost_obj = as_nn->get_expr();
        } else {
          break;
        }
      }
      bool will_be_extracted_as_tmp_var = leftmost_obj->kind == ast_function_call;
      if (!will_be_extracted_as_tmp_var) {
        leftmost_obj->mutate()->assign_lvalue_true();
        v->get_self_obj()->mutate()->assign_lvalue_true();
      }
    }

    for (int i = 0; i < v->get_num_args(); ++i) {
      const LocalVarData& p_sym = fun_ref->parameters[delta_self + i];
      auto arg_i = v->get_arg(i);
      if (p_sym.is_mutate_parameter() != arg_i->passed_as_mutate) {
        fire_error_invalid_mutate_arg_passed(cur_f, arg_i->loc, fun_ref, p_sym, arg_i->passed_as_mutate, arg_i->get_expr());
      }
      parent::visit(arg_i);
    }
    parent::visit(v->get_callee());
  }

public:
  bool should_visit_function(FunctionPtr fun_ref) override {
    return fun_ref->is_code_function() && !fun_ref->is_generic_function();
  }

  void start_visiting_function(FunctionPtr fun_ref, V<ast_function_declaration> v_function) override {
    cur_f = fun_ref;
    parent::visit(v_function->get_body());
  }
};

void pipeline_refine_lvalue_for_mutate_arguments() {
  visit_ast_of_all_functions<RefineLvalueForMutateArgumentsVisitor>();
}

} // namespace tolk
