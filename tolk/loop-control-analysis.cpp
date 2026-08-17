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
#include "loop-control-analysis.h"
#include "ast-visitor.h"
#include "type-system.h"

/*
 *   This file answers a single question: "can break/continue be compiled in this loop, and if yes,
 * where does the control flow go after every `continue`?".
 * Outcome:
 * - either `LoopContinuePlan`: "here are the if/match nodes that route the flow"
 * - or "no, because ..."
 * It's used by pipe-loop-break-continue (to fire an error) and by lowering AST->IR.
 *
 *   Note: an algorithm is VERY similar to "inline with multiple returns", almost identical.
 *
 *   `continue` can not emit a jump at IR level. Instead, the remaining suffix of the loop
 * is relocated into the single live if/match branch — the same FallthroughTail as inline returns.
 *   `break` is different: it emits native *BRK / RETALT, so it does not need routing.
 *
 *   Examples:                               =>        Rewritten as (at lowering)
 *
 *      while (true) {                                 while (true) {
 *          if (x) { continue }                            if (x) { }
 *          yyy                                            else { yyy }
 *      }                                              }
 *
 *      while (true) {                                 while (true) {
 *          if (cond1) { continue }                        if (cond1) { }
 *          if (cond2) { xxx; continue }                   else { if (cond2) { xxx }
 *          else { yyy }                                          else { yyy; zzz } }
 *          zzz                                        }
 *      }
 *
 *      while (true) {                                 while (true) {
 *          match (subj) {                                 match (subj) {
 *              0 => continue,                                 0 => { },
 *              1 => throw 1,                                  1 => { throw 1 },
 *          }                                                  else => {
 *          zzz                                                  zzz
 *      }                                              } }
 *
 *  See `inline-return-analysis.cpp` for what is supported and what is not.
 *
 *  In one sentence:
 *  Compilable continue <=> every if/match that contain `continue` leaves AT MOST ONE FALLTHROUGH PATH —
 *                          a single place where all the code below gets relocated.
 *
 *  Nested loops are opaque: their transfers belong to them, not to the outer loop.
 */

namespace tolk {

class ContainsContinueVisitor final : public ASTVisitorFunctionBody {
  bool found = false;

  void visit(V<ast_continue_statement>) override {
    found = true;
  }

  // in the current loop only: nested `continue` is their, not ours
  void visit(V<ast_repeat_statement>) override {}
  void visit(V<ast_while_statement>) override {}
  void visit(V<ast_do_while_statement>) override {}

public:
  bool should_visit_function(FunctionPtr) override {
    return false;
  }

  bool check(AnyV v) {
    found = false;
    parent::visit(v);
    return found;
  }
};

static bool contains_continue(AnyV v) {
  ContainsContinueVisitor visitor;
  return visitor.check(v);
}

// How control flow leaves a statement (or a list of statements).
// A statement that contains a `continue` but fits none of these shapes is unsupported:
// it makes the whole loop non-compilable, see `fail()`.
enum class LoopStmtFlow {
  // control flow reaches the next statement; no `continue` that needs routing
  fallsThrough,
  // all paths leave via `continue` (possibly mixed with break / return / throw); all below should not be lowered
  continues,
  // all paths leave via break / return / throw / never-call; no `continue` on any path
  terminates,
  // contains `continue`, but exactly one path still falls through (one if/else/match branch);
  // control flow reaches the next statement only after that specific branch executes;
  // added to plan nodes for lowering
  goesToBranch,
};

// Counters over if/match arms: alive = fallsThrough | goesToBranch; dead = continues | terminates.
struct LoopArmCounts {
  int n_fall_through = 0;
  int n_goes_to_branch = 0;
  int n_continues = 0;
  AnyV last_alive = nullptr;    // the last arm/body that does not fully leave
};

class LoopContinuePlanBuilder {
  LoopContinuePlan plan;

  void fail(const char* because) {
    if (plan.ok()) {        // store the first "because" reason
      plan.cant_continue_because = because;
    }
  }

  void reject_continue_inside(AnyV subtree) {
    if (contains_continue(subtree)) {
      fail("because of `continue` in non-standard, unsupported position");
    }
  }

  // after a terminal statement, we immediately return from `analyze`, but still check statements after it
  void analyze_terminal_and_unreachable_suffix(const std::vector<AnyV>& statements, size_t terminal_idx, LoopStmtFlow terminal_flow) {
    for (size_t i = terminal_idx; i < statements.size(); ++i) {
      AnyV stmt = statements[i];
      LoopStmtFlow stmt_flow = i == terminal_idx ? terminal_flow : analyze(stmt);
      if (stmt_flow == LoopStmtFlow::continues && stmt->kind != ast_continue_statement && i + 1 < statements.size()) {
        // we do not allow such code (to avoid extra state in lowering, since `next` should be erased)
        // > if (...) { continue }
        // > else { break }
        // > next
        fail("because a terminal `if/else/match` is followed by unreachable statements");
        break;
      }
    }
  }

  LoopStmtFlow analyze(const std::vector<AnyV>& statements) {
    bool has_continue = false;
    for (size_t i = 0; i < statements.size(); ++i) {
      AnyV stmt = statements[i];
      LoopStmtFlow stmt_flow = analyze(stmt);
      switch (stmt_flow) {
        case LoopStmtFlow::fallsThrough:
          break;                          // proceed to the next statement
        case LoopStmtFlow::continues:
          analyze_terminal_and_unreachable_suffix(statements, i, stmt_flow);
          return LoopStmtFlow::continues;
        case LoopStmtFlow::terminates:
          analyze_terminal_and_unreachable_suffix(statements, i, stmt_flow);
          return has_continue             // example: `if { if { continue } throw }`
               ? LoopStmtFlow::continues  // then mark an outer if as "continues"
               : LoopStmtFlow::terminates;
        case LoopStmtFlow::goesToBranch:
          has_continue = true;  // the tail below will be moved after that fallthrough branch
          break;
      }
    }
    return has_continue ? LoopStmtFlow::goesToBranch : LoopStmtFlow::fallsThrough;
  }

  void analyze_and_add(LoopArmCounts& c, const std::vector<AnyV>& statements, AnyV body) {
    switch (analyze(statements)) {
      case LoopStmtFlow::fallsThrough:
        c.n_fall_through++;
        c.last_alive = body;
        break;
      case LoopStmtFlow::goesToBranch:
        c.n_goes_to_branch++;
        c.last_alive = body;
        break;
      case LoopStmtFlow::continues:
        c.n_continues++;
        break;
      case LoopStmtFlow::terminates:
        break;
    }
  }

  LoopStmtFlow analyze_if(V<ast_if_statement> v) {
    reject_continue_inside(v->get_cond());
    // an `if` is a 2-arm exhaustive match: if-body and else-body (else may be empty)
    LoopArmCounts c;
    analyze_and_add(c, v->get_if_body()->get_items(), v->get_if_body());
    analyze_and_add(c, v->get_else_body()->get_items(), v->get_else_body());

    // no branch contains `continue` — paste as-is, no routing
    if (c.n_continues == 0 && c.n_goes_to_branch == 0) {
      bool both_branches_terminate = c.n_fall_through == 0;
      return both_branches_terminate ? LoopStmtFlow::terminates : LoopStmtFlow::fallsThrough;
    }
    // both branches leave (continues / terminates, at least one continues)
    if (c.n_fall_through == 0 && c.n_goes_to_branch == 0) {
      return LoopStmtFlow::continues;
    }
    // one branch leaves, tail goes to the other
    if (c.n_fall_through + c.n_goes_to_branch == 1) {
      plan.nodes.emplace_back(TailRoutingNode{v, c.last_alive});
      return LoopStmtFlow::goesToBranch;
    }

    fail("because `if/else` has complicated `continue` and multiple exit points");
    return LoopStmtFlow::fallsThrough;    // the plan is already invalid, the value doesn't matter
  }

  LoopStmtFlow analyze_match(V<ast_match_expression> v) {
    reject_continue_inside(v->get_subject());
    tolk_assert(v->is_statement());

    LoopArmCounts c;
    for (int i = 0; i < v->get_arms_count(); ++i) {
      auto v_arm = v->get_arm(i);
      if (v_arm->pattern_kind == MatchArmKind::const_expression) {
        reject_continue_inside(v_arm->get_pattern_expr());
      }
      analyze_and_add(c, v_arm->get_body()->get_block_statement()->get_items(), v_arm);
    }

    // no arm contains `continue` — paste as-is, no routing
    if (c.n_continues == 0 && c.n_goes_to_branch == 0) {
      bool all_arms_terminate = c.n_fall_through == 0 && v->is_exhaustive;
      return all_arms_terminate ? LoopStmtFlow::terminates : LoopStmtFlow::fallsThrough;
    }
    // all arms leave (via `continue` / break / return / throw)
    if (c.n_fall_through == 0 && c.n_goes_to_branch == 0 && v->is_exhaustive) {
      return LoopStmtFlow::continues;
    }
    // all arms leave, tail goes to implicit `else`
    if (c.n_fall_through == 0 && c.n_goes_to_branch == 0) {
      plan.nodes.emplace_back(TailRoutingNode{v, nullptr});
      return LoopStmtFlow::goesToBranch;
    }
    // all but one arms leave, tail goes to that arm
    if (c.n_fall_through + c.n_goes_to_branch == 1 && v->is_exhaustive) {
      plan.nodes.emplace_back(TailRoutingNode{v, c.last_alive});
      return LoopStmtFlow::goesToBranch;
    }

    fail("because some `match` arms do `continue`, some do not");
    return LoopStmtFlow::fallsThrough;    // the plan is already invalid, the value doesn't matter
  }

  LoopStmtFlow analyze(AnyV stmt) {
    if (stmt->try_as<ast_continue_statement>()) {
      return LoopStmtFlow::continues;
    }
    if (stmt->try_as<ast_break_statement>()) {
      return LoopStmtFlow::terminates;
    }
    if (auto v_return = stmt->try_as<ast_return_statement>()) {
      reject_continue_inside(v_return->get_return_value());
      return LoopStmtFlow::terminates;
    }
    if (auto v_throw = stmt->try_as<ast_throw_statement>()) {
      reject_continue_inside(v_throw);
      return LoopStmtFlow::terminates;
    }
    if (auto v_if = stmt->try_as<ast_if_statement>()) {
      return analyze_if(v_if);
    }
    if (auto v_match = stmt->try_as<ast_match_expression>()) {
      return analyze_match(v_match);
    }
    // any other statement is okay, unless it hides a `continue` in a position we can't route;
    // nested loops are skipped by contains_continue (their transfers are not ours)
    if (contains_continue(stmt)) {
      fail("because of `continue` in non-standard, unsupported position");
      return LoopStmtFlow::fallsThrough;    // the plan is already invalid, the value doesn't matter
    }

    // a call to a `never`-returning function ends control flow, just like `throw`
    if (auto v_call = stmt->try_as<ast_function_call>()) {
      bool is_never = v_call->fun_maybe && v_call->fun_maybe->inferred_return_type == TypeDataNever::create();
      return is_never ? LoopStmtFlow::terminates : LoopStmtFlow::fallsThrough;
    }

    // (more sophisticated cases like `cond ? alwaysThrows() : alwaysThrows()` are impractical to detect)
    return LoopStmtFlow::fallsThrough;
  }

public:
  LoopContinuePlan build(V<ast_block_statement> loop_body) {
    analyze(loop_body->get_items());
    return std::move(plan);
  }
};

LoopContinuePlan build_continue_plan_for_loop(AnyV loop_body_block) {
  LoopContinuePlanBuilder builder;
  return builder.build(loop_body_block->as<ast_block_statement>());
}

} // namespace tolk
