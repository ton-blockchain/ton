/*
    This file is part of TON Blockchain Library.

    TON Blockchain Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    TON Blockchain Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with TON Blockchain Library.  If not, see <http://www.gnu.org/licenses/>.
*/
#include "ast-visitor.h"
#include "compilation-errors.h"
#include "loop-control-analysis.h"

/*
 *   This pipe checks break/continue placement and extra restrictions LoopContinuePlan plan does not cover:
 *   - break/continue outside a loop
 *   - inside try/catch
 *   - `return` in a loop that already uses `break` (*BRK occupies c1)
 *
 *   It also asks `build_continue_plan_for_loop()` whether structural `continue` can be compiled.
 */

namespace tolk {

class LoopBreakContinueValidator final : public ASTVisitorFunctionBody {
  struct LoopFrame {
    AnyV break_stmt = nullptr;
    AnyV return_stmt = nullptr;
  };

  std::vector<LoopFrame> loop_frames;
  int inside_try_catch = 0;             // we are in `try` or `catch` body, deny break/continue
  int inside_loop_condition = 0;        // we are `while(HERE)`, deny break/continue/return

  void visit(V<ast_break_statement> v) override {
    if (inside_loop_condition || loop_frames.empty()) {
      err("`break` used outside a loop").collect(v, cur_f);
    } else if (inside_try_catch) {
      err("`break` is not allowed inside `try/catch`").collect(v, cur_f);
    } else {
      loop_frames.back().break_stmt = v;
    }
  }

  void visit(V<ast_continue_statement> v) override {
    if (inside_loop_condition || loop_frames.empty()) {
      err("`continue` used outside a loop").collect(v, cur_f);
    } else if (inside_try_catch) {
      err("`continue` is not allowed inside `try/catch`").collect(v, cur_f);
    }
  }

  void visit(V<ast_return_statement> v) override {
    if (inside_loop_condition) {
      err("`return` is not allowed in a loop condition").collect(v, cur_f);
    } else {
      for (LoopFrame& frame : loop_frames) {
        frame.return_stmt = v;
      }
    }
    parent::visit(v);
  }

  void visit(V<ast_try_catch_statement> v) override {
    inside_try_catch++;
    parent::visit(v);
    inside_try_catch--;
  }

  void analyze_loop(SrcRange keyword_range, AnyV condition, V<ast_block_statement> body) {
    inside_loop_condition++;
    parent::visit(condition);
    inside_loop_condition--;

    loop_frames.push_back(LoopFrame{});
    parent::visit(body);
    const LoopFrame& frame = loop_frames.back();
    if (frame.break_stmt && frame.return_stmt) {
      err("`return` is not allowed inside a loop that uses `break`")
        .with_secondary(frame.break_stmt, "`break` is used here")
        .collect(frame.return_stmt, cur_f);
    }
    LoopContinuePlan plan = build_continue_plan_for_loop(body);
    if (!plan.ok()) {
      err("can not compile `continue`, {}", plan.cant_continue_because)
        .with_secondary(plan.cant_continue_at, "this prevents compiling `continue`")
        .collect(keyword_range, cur_f);
    }
    loop_frames.pop_back();
  }

  void visit(V<ast_repeat_statement> v) override {
    analyze_loop(v->keyword_range(), v->get_cond(), v->get_body());
  }

  void visit(V<ast_while_statement> v) override {
    analyze_loop(v->keyword_range(), v->get_cond(), v->get_body());
  }

  void visit(V<ast_do_while_statement> v) override {
    analyze_loop(v->keyword_range(), v->get_cond(), v->get_body());
  }

public:
  bool should_visit_function(FunctionPtr fun_ref) override {
    return fun_ref->is_code_function() && !fun_ref->is_generic_function();
  }
};

void pipeline_check_loop_break_continue() {
  LoopBreakContinueValidator visitor;
  visit_ast_of_all_functions(visitor);
}

} // namespace tolk
