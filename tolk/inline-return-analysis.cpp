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
#include "inline-return-analysis.h"
#include "ast-visitor.h"
#include "type-system.h"

/*
 *   This file answers a single question: "can a function be inlined in place, and if yes,
 * where does the control flow go after every `return` in the middle?".
 * Outcome:
 * - either `InlineReturnPlan`: "here are the if/match nodes that route the flow"
 * - or "no, because ..."
 * It's used by pipe-detect-inline-in-place (to decide / to fire an error) and by lowering AST->IR.
 *
 *   Inlining in place means "paste a function body into a caller". A `return` inside that body
 * can not emit RET, since RET would exit the caller. Instead, `return` is "assign to OUT IR vars".
 *   What's the problem? Assigning OUT is easy. The hard part is:
 *   - everything below must not execute, and there is no goto/jump at IR level to skip it
 *
 *   The compiler supports returns in the middle — not everywhere, but in practical cases.
 *
 *   Examples:                               =>        Rewritten as (at lowering)
 *
 *      fun demo(x: int) {                             fun demo(x: int) {
 *          if (x < 0) { return 0 }                        if (x < 0) { OUT = 0 }
 *          return 2;                                      else { OUT = 2 }
 *      }                                              }
 *
 *      fun demo() {                                   fun demo() {
 *          if (cond1) { return }                          if (cond1) { }
 *          if (cond2) { xxx; return }                     else { if (cond2) { xxx }
 *          else { yyy }                                          else { yyy; zzz } }
 *          zzz                                        }
 *      }
 *
 *      fun demo() {                                   fun demo() {
 *          match (subj) {                                 match (subj) {
 *              0 => return 0,                                 0 => { OUT = 0 },
 *              1 => throw 1,                                  1 => { throw 1 },
 *          }                                                  else => {
 *          if (cond) { return 2 }                               if (cond) { OUT = 2 }
 *          zzz                                                  else { zzz;
 *          return 3;                                                   OUT = 3; }
 *      }                                              } } }
 *
 *  So, what is supported:
 *  - `if/else` where both branches terminate (or neither of them contains `return` at all)
 *  - `if/else` where one branch terminates and the other does not: tail goes to the other
 *  - `match` where all arms terminate (or none of them contains `return` at all);
 *    if it's not exhaustive, tail goes to its implicit `else`
 *  - `match` where all but one arms terminate, and it's exhaustive: tail goes to non-terminating
 *
 *  What is NOT supported:
 *  - unbalanced if/else, e.g. `if (cond) { if (cond) { return 1 } }`
 *  - unbalanced match, e.g. 2 arms return, 3 do not
 *  - `return` in other positions: in a loop, in try/catch, in a standalone block, etc.
 *  - `return` hidden inside an expression, e.g. `if (match(subj) { 0 => return 0 })`
 *
 *  In one sentence:
 *  Inlinable <=> every if/match that contain `return` leaves AT MOST ONE FALLTHROUGH PATH —
 *                a single place where all the code below gets relocated.
 *
 *   The analysis is a walk over statements classifying each one as `InlineStmtFlow` (see below).
 * Only `goesToBranch` statements are interesting: each is saved into `plan.nodes` as a pair
 * [ pivot -> the branch that continues ], and lowering uses it to know where to place the tail.
 */

namespace tolk {

class FindFirstReturnVisitor final : public ASTVisitorFunctionBody {
  AnyV found = nullptr;

  void visit(V<ast_return_statement> v) override {
    if (!found) {
      found = v;
    }
  }

public:
  bool should_visit_function(FunctionPtr) override {
    return false;
  }

  AnyV find(AnyV v) {
    found = nullptr;
    parent::visit(v);
    return found;
  }
};

AnyV find_first_return(AnyV v) {
  FindFirstReturnVisitor visitor;
  return visitor.find(v);
}

// How control flow leaves a statement (or a list of statements).
// A statement that contains a `return` but fits none of these shapes is unsupported:
// it makes the whole function non-inlinable, see `fail()`.
enum class InlineStmtFlow {
  // control flow reaches the next statement; no `return` that needs routing
  fallsThrough,
  // all paths leave via `return` (possibly mixed with `throw`); all below should not be lowered
  returns,
  // all paths leave via `throw`; no `return` on any path
  throws,
  // contains `return`, but exactly one path still falls through (one if/else/match branch);
  // control flow reaches the next statement only after that specific branch executes;
  // added to plan nodes for lowering
  goesToBranch,
};

// Counters over if/match arms: alive = fallsThrough | goesToBranch; dead = returns | throws.
struct InlineArmCounts {
  int n_fall_through = 0;
  int n_goes_to_branch = 0;
  int n_returns = 0;
  AnyV last_alive = nullptr;    // the last arm/body that does not fully leave
};

class InlineReturnPlanBuilder {
  InlineReturnPlan plan;

  void fail(const char* because, SrcRange at = SrcRange::undefined()) {
    if (plan.ok()) {        // store the first "because" reason
      plan.cant_inline_because = because;
      plan.cant_inline_at = at;
    }
  }

  void reject_return_inside(AnyV subtree) {
    if (AnyV ret = find_first_return(subtree)) {
      fail("because of `return` in non-standard, unsupported position", ret->range);
    }
  }

  // after a terminal statement, we immediately return from `analyze`, but still check statements after it
  void analyze_terminal_and_unreachable_suffix(const std::vector<AnyV>& statements, size_t terminal_idx, InlineStmtFlow terminal_flow) {
    for (size_t i = terminal_idx; i < statements.size(); ++i) {
      AnyV stmt = statements[i];
      InlineStmtFlow stmt_flow = i == terminal_idx ? terminal_flow : analyze(stmt);
      if (stmt_flow == InlineStmtFlow::returns && stmt->kind != ast_return_statement && i + 1 < statements.size()) {
        // we do not allow such code (to avoid extra state in lowering, since `next` should be erased)
        // > if (...) { return }
        // > else { return }
        // > next
        fail("because a terminal `if/else/match` is followed by unreachable statements", statements[i + 1]->range);
        break;
      }
    }
  }

  InlineStmtFlow analyze(const std::vector<AnyV>& statements) {
    bool has_return = false;
    for (size_t i = 0; i < statements.size(); ++i) {
      AnyV stmt = statements[i];
      InlineStmtFlow stmt_flow = analyze(stmt);
      switch (stmt_flow) {
        case InlineStmtFlow::fallsThrough:
          break;                          // proceed to the next statement
        case InlineStmtFlow::returns:
          analyze_terminal_and_unreachable_suffix(statements, i, stmt_flow);
          return InlineStmtFlow::returns;
        case InlineStmtFlow::throws:
          analyze_terminal_and_unreachable_suffix(statements, i, stmt_flow);
          return has_return               // example: `if { if { return } throw }`
               ? InlineStmtFlow::returns  // then mark an outer if as "returns"
               : InlineStmtFlow::throws;
        case InlineStmtFlow::goesToBranch:
          has_return = true;  // the tail below will be moved after that fallthrough branch
          break;
      }
    }
    return has_return ? InlineStmtFlow::goesToBranch : InlineStmtFlow::fallsThrough;
  }

  void analyze_and_add(InlineArmCounts& c, const std::vector<AnyV>& statements, AnyV body) {
    switch (analyze(statements)) {
      case InlineStmtFlow::fallsThrough:
        c.n_fall_through++;
        c.last_alive = body;
        break;
      case InlineStmtFlow::goesToBranch:
        c.n_goes_to_branch++;
        c.last_alive = body;
        break;
      case InlineStmtFlow::returns:
        c.n_returns++;
        break;
      case InlineStmtFlow::throws:
        break;
    }
  }

  InlineStmtFlow analyze_if(V<ast_if_statement> v) {
    reject_return_inside(v->get_cond());
    // an `if` is a 2-arm exhaustive match: if-body and else-body (else may be empty)
    InlineArmCounts c;
    analyze_and_add(c, v->get_if_body()->get_items(), v->get_if_body());
    analyze_and_add(c, v->get_else_body()->get_items(), v->get_else_body());

    // no branch contains `return` — paste as-is, no routing
    if (c.n_returns == 0 && c.n_goes_to_branch == 0) {
      bool both_branches_terminate = c.n_fall_through == 0;
      return both_branches_terminate ? InlineStmtFlow::throws : InlineStmtFlow::fallsThrough;
    }
    // both branches leave (returns / throws, at least one returns)
    if (c.n_fall_through == 0 && c.n_goes_to_branch == 0) {
      return InlineStmtFlow::returns;
    }
    // one branch leaves, tail goes to the other
    if (c.n_fall_through + c.n_goes_to_branch == 1) {
      plan.nodes.emplace_back(TailRoutingNode{v, c.last_alive});
      return InlineStmtFlow::goesToBranch;
    }

    fail("because `if/else` has complicated `return` and multiple exit points", v->keyword_range());
    return InlineStmtFlow::fallsThrough;    // the plan is already invalid, the value doesn't matter
  }

  InlineStmtFlow analyze_match(V<ast_match_expression> v) {
    reject_return_inside(v->get_subject());
    tolk_assert(v->is_statement());

    InlineArmCounts c;
    for (int i = 0; i < v->get_arms_count(); ++i) {
      auto v_arm = v->get_arm(i);
      if (v_arm->pattern_kind == MatchArmKind::const_expression) {
        reject_return_inside(v_arm->get_pattern_expr());
      }
      analyze_and_add(c, v_arm->get_body()->get_block_statement()->get_items(), v_arm);
    }

    // no arm contains `return` — paste as-is, no routing
    if (c.n_returns == 0 && c.n_goes_to_branch == 0) {
      bool all_arms_terminate = c.n_fall_through == 0 && v->is_exhaustive;
      return all_arms_terminate ? InlineStmtFlow::throws : InlineStmtFlow::fallsThrough;
    }
    // all arms leave (via `return` / `throw`)
    if (c.n_fall_through == 0 && c.n_goes_to_branch == 0 && v->is_exhaustive) {
      return InlineStmtFlow::returns;
    }
    // all arms leave, tail goes to implicit `else`
    if (c.n_fall_through == 0 && c.n_goes_to_branch == 0) {
      plan.nodes.emplace_back(TailRoutingNode{v, nullptr});
      return InlineStmtFlow::goesToBranch;
    }
    // all but one arms leave, tail goes to that arm
    if (c.n_fall_through + c.n_goes_to_branch == 1 && v->is_exhaustive) {
      plan.nodes.emplace_back(TailRoutingNode{v, c.last_alive});
      return InlineStmtFlow::goesToBranch;
    }

    fail("because some `match` arms do `return`, some do not", v->keyword_range());
    return InlineStmtFlow::fallsThrough;    // the plan is already invalid, the value doesn't matter
  }

  InlineStmtFlow analyze(AnyV stmt) {
    if (auto v_return = stmt->try_as<ast_return_statement>()) {
      reject_return_inside(v_return->get_return_value());
      return InlineStmtFlow::returns;
    }
    if (auto v_throw = stmt->try_as<ast_throw_statement>()) {
      reject_return_inside(v_throw);
      return InlineStmtFlow::throws;
    }
    if (auto v_if = stmt->try_as<ast_if_statement>()) {
      return analyze_if(v_if);
    }
    if (auto v_match = stmt->try_as<ast_match_expression>()) {
      return analyze_match(v_match);
    }
    // any other statement is okay, unless it hides a `return` in a position we can't route
    if (AnyV ret = find_first_return(stmt)) {
      // make a reasonable message for common cases
      switch (stmt->kind) {
        case ast_repeat_statement:
        case ast_while_statement:
        case ast_do_while_statement:
          fail("because of `return` inside a loop", ret->range);
          break;
        case ast_try_catch_statement:
          fail("because of `return` inside try/catch", ret->range);
          break;
        default:
          fail("because of `return` in non-standard, unsupported position", ret->range);
          break;
      }
      return InlineStmtFlow::fallsThrough;    // the plan is already invalid, the value doesn't matter
    }

    // a call to a `never`-returning function ends control flow, just like `throw`
    if (auto v_call = stmt->try_as<ast_function_call>()) {
      bool is_never = v_call->fun_maybe && v_call->fun_maybe->inferred_return_type == TypeDataNever::create();
      return is_never ? InlineStmtFlow::throws : InlineStmtFlow::fallsThrough;
    }

    // (more sophisticated cases like `cond ? alwaysThrows() : alwaysThrows()` are impractical to detect)
    return InlineStmtFlow::fallsThrough;
  }

public:
  InlineReturnPlan build(FunctionPtr fun_ref) {
    tolk_assert(fun_ref->ast_root);
    auto v_body = fun_ref->ast_root->as<ast_function_declaration>()->get_body()->try_as<ast_block_statement>();
    if (v_body == nullptr) {
      fail("because `@inline` is applicable only to code functions, not to `asm`");
    } else if (fun_ref->n_times_called >= 9999) {
      fail("because a function recursively calls itself");
    } else if (fun_ref->has_tvm_method_id()) {
      fail("because `@inline` is incompatible with `@method_id` and `get fun`");
    } else if (fun_ref->is_used_as_noncall()) {
      fail("because it's used as a reference, like `var callback = myFunction` or `f(myFunction)`");
    } else {
      analyze(v_body->get_items());
    }
    return std::move(plan);
  }
};

InlineReturnPlan build_inlining_plan_for_function(FunctionPtr fun_ref) {
  InlineReturnPlanBuilder builder;
  return builder.build(fun_ref);
}

} // namespace tolk
