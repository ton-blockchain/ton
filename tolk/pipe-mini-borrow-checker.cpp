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
 * To support fields and independency check, we use SinkExpression — the same struct that is used for smart casts.
 * Both variables `v` and fields `obj.f1.f2` are sink expressions. They can be extracted from vertices and compared.
 *   Note, that operators `as` and `!` are not valid sink expressions. As a consequence, `!` can be used to overcome
 * compiler checks here: `items.add(items!.remove())` becomes okay.
 *
 *   In order to prevent `x += ([x] = rhs).0`, we need to carefully dig into lhs of assignment. Traversing top-down,
 * we can't just mark "we are inside lhs of assignment" and treat all references as mutated there, because
 * `getObj(x).field` is a valid lhs, where `x` is not mutated. 
 */

namespace tolk {

struct BorrowedVarOrField {
  SinkExpression s_expr;
  FunctionPtr by_function;

  std::string stringify_by_function() const {
    if (by_function != nullptr) {
      return by_function->as_human_readable();
    }
    // only operators `+=` and similar may borrow a variable except a mutating function
    return "assignment operator";
  }
};

// A context holding currently borrowed expressions.
// When entering a function or lhs of assignment, mutated expressions are added here.
// Therefore, while traversing top-down, other attempts to add the same expression will result in an error.
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
      if (existing.s_expr == s_expr) {
        err("can not borrow `{}` for mutation once again, it is already being mutated by `{}`\n""hint: split a complex expression into several simple ones", s_expr.to_string(), existing.stringify_by_function()).fire(where, cur_f); 
      }
      if (existing.s_expr.is_child_of(s_expr) || s_expr.is_child_of(existing.s_expr)) {
        err("can not borrow `{}` for mutation, because `{}` is already being mutated by `{}`\n""hint: split a complex expression into several simple ones", s_expr.to_string(), existing.s_expr.to_string(), existing.stringify_by_function()).fire(where, cur_f); 
      }
    }
    expressions.emplace_front(BorrowedVarOrField{s_expr, by_function});
  }
};

class CheckMutationNotHappensTwiceVisitor final : public ASTVisitorFunctionBody {
  BorrowedForWriteCtx borrow_ctx;

  void visit(V<ast_function_call> v) override {
    FunctionPtr fun_ref = v->fun_maybe;
    if (!fun_ref) {       // a "call" to a variable, it can't be mutating
      parent::visit(v);
      return;
    }

    borrow_ctx.push_frame();

    int delta_self = v->get_self_obj() != nullptr;

    // obj.mutatingMethod() — borrow obj while calculating all arguments
    if (v->dot_obj_is_self) {
      AnyExprV self_obj = v->get_self_obj();
      parent::visit(self_obj);
      if (fun_ref->does_mutate_self()) {
        if (SinkExpression s_expr = extract_sink_expression_from_vertex(self_obj)) {
          borrow_ctx.borrow_or_fire_if_twice(cur_f, s_expr, self_obj, fun_ref);
        }
      }
    }
    // f(mutate x) — borrow x while calculating rest arguments
    for (int i = 0; i < v->get_num_args(); ++i) {
      AnyExprV ith_arg = v->get_arg(i)->get_expr();
      parent::visit(ith_arg);
      if (fun_ref->parameters[delta_self + i].is_mutate_parameter()) {
        if (SinkExpression s_expr = extract_sink_expression_from_vertex(ith_arg)) {
          borrow_ctx.borrow_or_fire_if_twice(cur_f, s_expr, ith_arg, fun_ref);
        }
      }
    }

    borrow_ctx.pop_frame();
  }

  void visit(V<ast_assign> v) override {
    // recursively analyze assignment lhs to find not only `x = rhs`, but also `(([_, x], _)) = rhs`
    // note, that rhs CAN mutate x, because assignment is happening only after evaluating it
    // (unlike `x += rhs`, which can't mutate x)
    process_assignment_lhs(v->get_lhs());
    parent::visit(v->get_rhs());
  }

  void visit(V<ast_set_assign> v) override {
    // unlike assignment `x = rhs`, operators `+=` and similar don't allow tensors and similar
    borrow_ctx.push_frame();
    if (SinkExpression lhs_s_expr = extract_sink_expression_from_vertex(v->get_lhs())) {
      borrow_ctx.borrow_or_fire_if_twice(cur_f, lhs_s_expr, v->get_lhs(), nullptr);
    }

    // borrow lhs while calculating rhs, because `x += rhs` is actually `x = x + rhs`
    // (rhs can't mutate x, it's copied before evaluating rhs)
    parent::visit(v);
    borrow_ctx.pop_frame();
  }

  void process_assignment_lhs(AnyExprV lhs) {
    // we are not interested in `var x = rhs`, only in assigning to existing `x = rhs`
    if (auto lhs_tensor = lhs->try_as<ast_tensor>()) {
      for (int i = 0; i < lhs_tensor->size(); ++i) {
        process_assignment_lhs(lhs_tensor->get_item(i));
      }
      return;
    }
    if (auto lhs_tuple = lhs->try_as<ast_bracket_tuple>()) {
      for (int i = 0; i < lhs_tuple->size(); ++i) {
        process_assignment_lhs(lhs_tuple->get_item(i));
      }
      return;
    }

    // note, that for `x = rhs` we ALLOW rhs to mutate x, because assignment happens after evaluating rhs;
    // for example, `b = b.storeInt()` is common and correct;
    // what we do here is checking that assignment is allowed in this exact place, it's not already borrowed:
    // `point.mutate(..., point.x = 10)`   // can't borrow `point.x`, because `point` is already being mutated
    if (SinkExpression lhs_s_expr = extract_sink_expression_from_vertex(lhs)) {
      borrow_ctx.push_frame();
      borrow_ctx.borrow_or_fire_if_twice(cur_f, lhs_s_expr, lhs, nullptr);
      borrow_ctx.pop_frame();
    }

    parent::visit(lhs);
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
  visit_ast_of_all_functions<CheckMutationNotHappensTwiceVisitor>();
}

} // namespace tolk
