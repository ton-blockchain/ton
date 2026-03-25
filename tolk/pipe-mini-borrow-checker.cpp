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
#include "smart-casts-cfg.h"
#include "compilation-errors.h"
#include <forward_list>

/*
 *   This pipe prevents concurrent mutations of one and the same object at a time, because it's UB (undefined behavior).
 *   Example 1:
 *   > f(mutate x, mutate x)
 *   Example 2:
 *   > items.add(items.remove(), true)   (both methods are mutating)
 *   Example 3:
 *   > x += ([x] = [0]).0
 *   In all cases above, an error "can not borrow XXX for mutation once again" is printed.
 *
 *   We analyze and prevent mutating not only for a single variable, but also for fields of an object/tensor.
 * Independent fields can be safely mutated:
 *   > obj.x = (obj.y = ...)     // ok, independent fields
 * But for example, mutating `d.nested.field` while mutating `d.nested` or `d` itself, is an error:
 *   > d.mutatingMethod(d.nested.field = 10)    // error, can not borrow `d.nested.field`, because `d` already borrowed
 *   > p.x += (p.mut().x = 5)                   // error, can not borrow `p`, because `p.x` already borrowed
 *
 *   To track which variables/fields are being mutated, we use is_valid_lvalue_path() with sink collection:
 * it traverses the lvalue path and collects SinkExpression for each "leaf" variable being mutated.
 * For tensors like `(a, b)`, both `a` and `b` are collected. For `(a, b).0`, only `a` is collected.
 */

namespace tolk {

struct BorrowedVarOrField {
  SinkExpression s_expr;        // `v` / `v.field` / `v.0.nested`
  FunctionPtr by_function;      // exists for `f(mutate v)`, nullptr for `v = rhs`
};

class BorrowedForWriteCtx {
  std::forward_list<BorrowedVarOrField> expressions;
  std::forward_list<std::forward_list<BorrowedVarOrField>::iterator> frame_heads;

public:
  bool empty() const {
    return expressions.empty();
  }

  void push_frame() {
    frame_heads.emplace_front(expressions.begin());
  }

  void pop_frame() {
    std::forward_list<BorrowedVarOrField>::iterator target = frame_heads.front();
    frame_heads.pop_front();
    while (expressions.begin() != target) {
      expressions.pop_front();
    }
  }

  void borrow_or_fire_if_twice(FunctionPtr cur_f, SinkExpression s_expr, AnyExprV where, FunctionPtr by_function) {
    for (const BorrowedVarOrField& existing : expressions) {
      std::string by_str = existing.by_function ? existing.by_function->as_human_readable() : "assignment operator";
      if (existing.s_expr == s_expr) {
        err("can not borrow `{}` for mutation once again, it is already being mutated by `{}`\n""hint: split a complex expression into several simple ones", s_expr.to_string(), by_str).collect(where, cur_f);
      }
      if (existing.s_expr.is_child_of(s_expr) || s_expr.is_child_of(existing.s_expr)) {
        err("can not borrow `{}` for mutation, because `{}` is already being mutated by `{}`\n""hint: split a complex expression into several simple ones", s_expr.to_string(), existing.s_expr.to_string(), by_str).collect(where, cur_f);
      }
    }
    expressions.emplace_front(BorrowedVarOrField{s_expr, by_function});
  }

  void borrow_all_from_lvalue(FunctionPtr cur_f, AnyExprV lhs, FunctionPtr called_f) {
    // for `v = rhs`, sinks = [v]; for `(a, b.field) = rhs`, sinks = [a, b.field]
    std::vector<SinkExpression> sinks;
    is_valid_lvalue_path(lhs, &sinks);    // not assert, it may be false (an error is already collected)
    for (const SinkExpression& s : sinks) {
      borrow_or_fire_if_twice(cur_f, s, lhs, called_f);
    }
  }
};

class CheckMutationNotHappensTwiceVisitor final : public ASTVisitorFunctionBody {
  BorrowedForWriteCtx borrow_ctx;

  void visit(V<ast_function_call> v) override {
    FunctionPtr fun_ref = v->fun_maybe;
    if (!fun_ref) {
      parent::visit(v);
      return;
    }

    borrow_ctx.push_frame();

    int delta_self = v->get_self_obj() != nullptr;

    if (delta_self) {
      AnyExprV self_obj = v->get_self_obj();
      parent::visit(self_obj);
      if (fun_ref->does_mutate_self()) {
        borrow_ctx.borrow_all_from_lvalue(cur_f, self_obj, fun_ref);
      }
    }
    for (int i = 0; i < v->get_num_args(); ++i) {
      AnyExprV ith_arg = v->get_arg(i)->get_expr();
      parent::visit(ith_arg);
      if (fun_ref->parameters[delta_self + i].is_mutate_parameter()) {
        borrow_ctx.borrow_all_from_lvalue(cur_f, ith_arg, fun_ref);
      }
    }

    borrow_ctx.pop_frame();
  }

  void visit(V<ast_assign> v) override {
    borrow_ctx.push_frame();
    borrow_ctx.borrow_all_from_lvalue(cur_f, v->get_lhs(), nullptr);
    borrow_ctx.pop_frame();
    parent::visit(v->get_rhs());
  }

  void visit(V<ast_set_assign> v) override {
    borrow_ctx.push_frame();
    borrow_ctx.borrow_all_from_lvalue(cur_f, v->get_lhs(), nullptr);
    parent::visit(v);
    borrow_ctx.pop_frame();
  }

public:
  bool should_visit_function(FunctionPtr fun_ref) override {
    return fun_ref->is_code_function() && !fun_ref->is_generic_function();
  }

  void on_exit_function(V<ast_function_declaration> v_function) override {
    tolk_assert(borrow_ctx.empty());
  }
};

void pipeline_mini_borrow_checker_for_mutate() {
  CheckMutationNotHappensTwiceVisitor visitor;
  visit_ast_of_all_functions(visitor);
}

} // namespace tolk
