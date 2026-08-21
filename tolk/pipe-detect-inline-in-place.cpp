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
#include "inline-return-analysis.h"
#include "pack-unpack-api.h"
#include <algorithm>
#include <functional>
#include <unordered_map>
#include <unordered_set>

/*
 *   This pipe detects whether each function should be inlined in-place or not.
 * Outcome: call `fun_ref->assign_inline_mode_in_place()` for "lightweight" or "called only once" functions,
 * and they will be inlined in-place while converting AST to IR (to Ops), and won't be generated to Fift.
 *
 *   Given AST only, there is no definite algorithm to predict whether a function is "simple" ("lightweight"),
 * so that inlining it will do better. There are no correct metrics at AST level that can be mapped onto TVM complexity.
 *   So, instead of overcomplicating and fine-tuning an algorithm, we're heading a simple way:
 *   - if a function is tiny, inline it always
 *   - if a function is called only once, inline it (only there, obviously)
 *   - if a function is marked `@inline` (intended by the user), inline it in place (if possible)
 *   - see `should_auto_inline_if_not_annotated()`
 *
 *   What can prevent a function from inlining? For example:
 *   - it's recursive
 *   - it's used as non-call (a reference to it is taken)
 *   - it has non-primitive control flow with return statements (e.g. `return` from a loop)
 *   - all reasons are in `build_inlining_plan_for_function()`, both this pipe and lowering ask it
 *
 *   The `@inline` annotation means "user intention", so the compiler tries to inline it in-place
 * without considering AST metrics. If any reason prevents inlining, an error is shown.
 * So, `@inline` is either inlined or fired, it's not silently skipped if impossible.
 *
 *   Besides inline detection, this pipe populates `fun_ref->n_times_called` (while building call graph).
 * It's used in Fift output inside comments.
 *
 *   The same call graph is reused to assign `requires_callxargs`: a function needs CALLXARGS if it
 * contains `try/catch`, or inlines in-place another function that does.
 */

namespace tolk {

// when traversing a function, collect some AST metrics used to detect whether it's lightweight
struct StateWhileTraversingFunction {
  FunctionPtr fun_ref;
  int n_statements = 0;
  int n_function_calls = 0;
  int n_binary_operators = 0;
  int n_control_flow = 0;
  int n_globals = 0;
  int max_block_depth = 0;

  explicit StateWhileTraversingFunction(FunctionPtr fun_ref)
    : fun_ref(fun_ref) {}

  int calculate_ast_cost() const {
    return n_function_calls + n_binary_operators + n_statements * 2
         + n_control_flow * 10 + n_globals * 5 + (max_block_depth - 1) * 10;
  }

  // if a user specified `@inline`, check that it's possible; if not, show an error why
  void try_inline_or_collect_error() const {
    InlineReturnPlan inlining_plan = build_inlining_plan_for_function(fun_ref);
    if (!inlining_plan.ok()) {
      Error diagnostic = err("function `{}` can't be inlined\n""hint: `@inline` is impossible, {}", fun_ref, inlining_plan.cant_inline_because);
      if (inlining_plan.cant_inline_at.is_defined()) {
        diagnostic.with_secondary(inlining_plan.cant_inline_at, "this prevents inlining");
      }
      diagnostic.collect(fun_ref);
    }
  }

  // if no annotation specified, detect whether to auto-inline a function
  bool should_auto_inline_if_not_annotated() const {
    // a function can not be inlined (for example, it's recursive or contains `return` in a loop)
    if (!build_inlining_plan_for_function(fun_ref).ok()) {
      return false;
    }

    // if a function is called only once, inline it regardless of its size
    // (to prevent this, `@inline_ref` can be used, for example)
    if (fun_ref->n_times_called == 1) {
      return true;
    }

    // if a function is lightweight, inline in regardless of how many times it's called
    // (for instance, `Storage.load` is always inlined)
    int approx_cost_per_call = calculate_ast_cost();
    if (approx_cost_per_call < 30) {
      return true;
    }

    // try to _somehow_ detect whether to inline it or not
    return approx_cost_per_call * fun_ref->n_times_called < 150;
  }
};

// traverse the AST, collect metrics, and in the end, probably set the inline flag
class DetectIfToInlineFunctionInPlaceVisitor final : public ASTVisitorFunctionBody {
  StateWhileTraversingFunction cur_state{nullptr};
  int block_depth = 0;

  void visit(V<ast_function_call> v) override {
    bool is_compiler_expect = v->fun_maybe && v->fun_maybe->is_builtin() && v->fun_maybe->name.starts_with("__expect");
    if (!is_compiler_expect) {    // don't treat `__expect_inline` and `__expect_type` as function calls
      cur_state.n_function_calls++;
    }
    parent::visit(v);
  }

  void visit(V<ast_binary_operator> v) override {
    if (v->tok == tok_logical_and || v->tok == tok_logical_not) {
      cur_state.n_control_flow++;
    } else {
      cur_state.n_binary_operators++;
    }
    parent::visit(v);
  }

  void visit(V<ast_reference> v) override {
    if (v->sym->try_as<GlobalVarPtr>()) {
      cur_state.n_globals++;
    }
  }

  void visit(V<ast_block_statement> v) override {
    block_depth++;
    cur_state.n_statements += v->size();
    cur_state.max_block_depth = std::max(cur_state.max_block_depth, block_depth);
    parent::visit(v);
    block_depth--;
  }

  void visit(V<ast_if_statement> v) override {
    cur_state.n_control_flow++;
    parent::visit(v);
  }

  void visit(V<ast_repeat_statement> v) override {
    cur_state.n_control_flow++;
    parent::visit(v);
  }

  void visit(V<ast_while_statement> v) override {
    cur_state.n_control_flow++;
    parent::visit(v);
  }

  void visit(V<ast_do_while_statement> v) override {
    cur_state.n_control_flow++;
    parent::visit(v);
  }

  void visit(V<ast_throw_statement> v) override {
    cur_state.n_control_flow++;
    parent::visit(v);
  }

  void visit(V<ast_assert_statement> v) override {
    cur_state.n_control_flow++;
    parent::visit(v);
  }

  void visit(V<ast_try_catch_statement> v) override {
    cur_state.n_control_flow++;
    parent::visit(v);
  }

  void visit(V<ast_match_expression> v) override {
    cur_state.n_control_flow++;
    parent::visit(v);
  }

public:
  bool should_visit_function(FunctionPtr fun_ref) override {
    if (!fun_ref->is_code_function() || fun_ref->is_generic_function()) {
      return false;
    }
    // has `@inline` annotation: we should check it can be inlined actually
    if (fun_ref->inline_mode == FunctionInlineMode::inlineInPlace) {
      return true;
    }
    // has other annotations, e.g. `@inline_ref` or `@noinline`
    if (fun_ref->inline_mode != FunctionInlineMode::notAnnotated) {
      return false;
    }
    // okay, we need to auto-detect whether to inline this function; filter out obviously false
    if (fun_ref->has_tvm_method_id() || fun_ref->is_used_as_noncall() || fun_ref->is_lambda()) {
      return false;
    }
    // start auto-detection
    return true;
  }

  void on_enter_function(V<ast_function_declaration> v_function) override {
    cur_state = StateWhileTraversingFunction(cur_f);
  }

  void on_exit_function(V<ast_function_declaration> v_function) override {
    if (cur_f->inline_mode == FunctionInlineMode::inlineInPlace) {
      cur_state.try_inline_or_collect_error();
    } else if (cur_state.should_auto_inline_if_not_annotated()) {
      cur_f->mutate()->assign_inline_mode_in_place();
    }
  }
};

// this visitor (called once for a function):
// 1) fills call_graph[cur_f] (all function calls from cur_f)
// 2) increments n_times_called
// 3) records functions that contain `try/catch`
// as a result of applying it to every function, we get a full call graph and how many times each function was called;
// we'll use this call graph to detect recursive components (functions within recursions can not be inlined)
// and to propagate `requires_callxargs` through in-place inline edges
class CallGraphBuilderVisitor final : public ASTVisitorFunctionBody {

  void register_call(FunctionPtr called_f) {
    tolk_assert(!called_f->is_generic_function());
    if (called_f->is_code_function()) {
      call_graph[cur_f].emplace_back(called_f);
    }
    called_f->mutate()->n_times_called++;
  }

  void visit(V<ast_function_call> v) override {
    if (FunctionPtr called_f = v->fun_maybe) {
      register_call(called_f);

      // calling `SomeStruct.toCell()` implicitly calls custom `packToBuilder()` serializers for nested fields;
      // include them in call graph to detect recursive serializers and try/catch within them
      if (called_f->is_compile_time_special_gen() && called_f->is_instantiation_of_generic_function()) {
        std::vector<FunctionPtr> implicit_pack_unpack;
        collect_recursive_pack_unpack_when_f_called(called_f, nullptr, &implicit_pack_unpack);
        for (FunctionPtr implicit_f : implicit_pack_unpack) {
          register_call(implicit_f);
        }
      }
    }
    parent::visit(v);
  }

  void visit(V<ast_try_catch_statement> v) override {
    functions_with_try_catch.insert(cur_f);
    parent::visit(v);
  }

public:
  // populated while visiting: maps [ fun_ref -> list of functions it calls ]
  std::unordered_map<FunctionPtr, std::vector<FunctionPtr>> call_graph;
  std::unordered_set<FunctionPtr> functions_with_try_catch;

  bool should_visit_function(FunctionPtr fun_ref) override {
    // don't include asm functions, we don't need them in calculations
    return fun_ref->is_code_function() && !fun_ref->is_generic_function();
  }

  void on_enter_function(V<ast_function_declaration> v_function) override {
    call_graph[cur_f] = std::vector<FunctionPtr>{};
  }
};

static void detect_recursive_functions(const std::unordered_map<FunctionPtr, std::vector<FunctionPtr>>& call_graph) {
  // using call_graph, detect cycles (the smallest, non-optimized algorithm, okay for our needs)
  for (const auto& it : call_graph) {
    FunctionPtr f_start_from = it.first;
    std::unordered_set<FunctionPtr> visited;
    std::function<bool(FunctionPtr)> is_recursive_dfs = [&](FunctionPtr cur) -> bool {
      for (FunctionPtr f_called : call_graph.at(cur)) {
        if (f_called == f_start_from)
          return true;
        if (!visited.insert(f_called).second)
          continue;
        if (is_recursive_dfs(f_called))
          return true;
      }
      return false;
    };
    if (!it.second.empty() && is_recursive_dfs(f_start_from)) {
      f_start_from->mutate()->n_times_called = 9999;      // means "recursive"
      if (f_start_from->is_packToBuilder() || f_start_from->is_unpackFromSlice()) {
        err("function `{}` is recursive and leads to infinite serialization", f_start_from).collect(f_start_from);
      }
    }
  }
}

static void detect_requires_callxargs(const std::unordered_map<FunctionPtr, std::vector<FunctionPtr>>& call_graph,
                                      const std::unordered_set<FunctionPtr>& functions_with_try_catch) {
  std::unordered_set<FunctionPtr> requires_callxargs = functions_with_try_catch;
  bool changed;
  do {
    changed = false;
    for (const auto& [caller, callees] : call_graph) {
      if (!requires_callxargs.contains(caller)) {
        bool inlines_callxargs = std::any_of(callees.begin(), callees.end(), [&](FunctionPtr callee) {
          return callee->is_inlined_in_place() && requires_callxargs.contains(callee);
        });
        changed |= inlines_callxargs && requires_callxargs.insert(caller).second;
      }
    }
  } while (changed);

  for (FunctionPtr fun_ref : requires_callxargs) {
    fun_ref->mutate()->assign_requires_callxargs();
  }
}

// Check __expect_inline() calls, used in compiler tests as assertions.
class CheckExpectInlineAssertionsVisitor final : public ASTVisitorFunctionBody {

  void visit(V<ast_function_call> v) override {
    if (v->fun_maybe && v->fun_maybe->is_builtin() && v->fun_maybe->name == "__expect_inline") {
      tolk_assert(v->get_num_args() == 1 && v->get_arg(0)->get_expr()->kind == ast_bool_const);
      bool expected = v->get_arg(0)->get_expr()->as<ast_bool_const>()->bool_val;
      bool actual = cur_f->is_inlined_in_place();
      if (expected != actual) {
        err("__expect_inline failed").fire(v, cur_f);
      }
    }
    parent::visit(v);
  }

public:
  bool should_visit_function(FunctionPtr fun_ref) override {
    return fun_ref->is_code_function() && !fun_ref->is_generic_function();
  }
};

void pipeline_detect_inline_in_place() {
  // 1) we need call graph to detect recursive functions, BEFORE calculating inline
  CallGraphBuilderVisitor graph_visitor;
  visit_ast_of_all_functions(graph_visitor);
  detect_recursive_functions(graph_visitor.call_graph);

  // 2) not auto-detect inline and check `@inline` annotations
  DetectIfToInlineFunctionInPlaceVisitor visitor;
  visit_ast_of_all_functions(visitor);
  CheckExpectInlineAssertionsVisitor checker;
  visit_ast_of_all_functions(checker);

  // 3) assign callxargs by THE SAME call graph, strictly AFTER calculating inline
  detect_requires_callxargs(graph_visitor.call_graph, graph_visitor.functions_with_try_catch);
}

} // namespace tolk
