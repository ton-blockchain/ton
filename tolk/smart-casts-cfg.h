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
#pragma once

#include "fwd-declarations.h"
#include "type-system.h"
#include <cstdint>
#include <map>
#include <vector>

namespace tolk {

/*
 * TypeInferringUnifyStrategy unifies types from various branches to a common result (lca).
 * It's used to auto infer function return type based on return statements, like in TypeScript.
 * Example: `fun f() { ... return 1; ... return null; }` inferred as `int?`.
 *
 * Besides function returns, it's also used for ternary `return cond ? 1 : null` and `match` expression.
 * If types can't be unified (a function returns int and cell, for example), `unify()` returns false, handled outside.
 * BTW, don't confuse this way of inferring with Hindley-Milner, they have nothing in common.
 */
class TypeInferringUnifyStrategy {
  TypePtr unified_result = nullptr;
  bool different_types_became_union = false;

public:
  void unify_with(TypePtr next, TypePtr dest_hint = nullptr);

  TypePtr get_result() const { return unified_result; }
  bool is_union_of_different_types() const { return different_types_became_union; }
};

/*
 * SinkExpression is an expression that can be smart cast like `if (x != null)` (x is int inside)
 * or analyzed by data flow is some other way like `if (x > 0) ... else ...` (x <= 0 inside else).
 * In other words, it "absorbs" data flow facts.
 * Examples: `localVar`, `localTensor.1`, `localTuple.1.2.3`, `localObj.field`
 * These are NOT sink expressions: `globalVar`, `f()`, `f().1`
 * Note, that globals are NOT sink: don't encourage to use a global twice, it costs gas, better assign it to a local.
 */
struct SinkExpression {
  LocalVarPtr const var_ref;            // smart casts and data flow applies only to locals
  const uint64_t index_path;            // 0 for just `v`; for `v.N` it's (N+1), for `v.N.M` it's (N+1) + (M+1)<<8, etc.

  SinkExpression()
    : var_ref(nullptr), index_path(0) {}
  explicit SinkExpression(LocalVarPtr var_ref)
    : var_ref(var_ref), index_path(0) {}
  explicit SinkExpression(LocalVarPtr var_ref, uint64_t index_path)
    : var_ref(var_ref), index_path(index_path) {}

  SinkExpression(const SinkExpression&) = default;
  SinkExpression& operator=(const SinkExpression&) = delete;

  bool operator==(const SinkExpression& rhs) const { return var_ref == rhs.var_ref && index_path == rhs.index_path; }
  bool operator<(const SinkExpression& rhs) const { return var_ref == rhs.var_ref ? index_path < rhs.index_path : var_ref < rhs.var_ref; }
  explicit operator bool() const { return var_ref != nullptr; }

  std::string to_string() const;
};

// UnreachableKind is a reason of why control flow is unreachable or interrupted
// example: `return;` interrupts control flow
// example: `if (true) ... else ...` inside "else" flow is unreachable because it can't happen
enum class UnreachableKind {
  Unknown,     // no definite info or not unreachable
  CantHappen,
  ThrowStatement,
  ReturnStatement,
  CallNeverReturnFunction,
};

// SignState is "definitely positive", etc.
// example: inside `if (x > 0)`, x is Positive, in `else` it's NonPositive (if x is local, until reassigned)
enum class SignState {
  Unknown,     // no definite info
  Positive,
  Negative,
  Zero,
  NonNegative,
  NonPositive,
  Never        // can't happen, like "never" type
};

// BoolState is "definitely true" or "definitely false"
// example: inside `if (x)`, x is AlwaysTrue, in `else` it's AlwaysFalse
enum class BoolState {
  Unknown,     // no definite info
  AlwaysTrue,
  AlwaysFalse,
  Never        // can't happen, like "never" type
};

// FactsAboutExpr represents "everything known about SinkExpression at a given execution point"
// example: after `var x = getNullableInt()`, x is `int?`, sign/bool is Unknown
// example: after `x = 2;`, x is `int`, sign is Positive, bool is AlwaysTrue
// example: inside `if (x != null && x > 0)`, x is `int`, sign is Positive (in else, no definite knowledge)
// remember, that indices/fields are also expressions, `t.1 = 2` or `u.id = 2` also store such facts
// WARNING! Detecting data-flow facts about sign state and bool state is NOT IMPLEMENTED
// (e.g. `if (x > 0)` / `if (!t.1)` is NOT analysed, therefore not updated, always Unknown now)
// it's a potential improvement for the future, for example `if (x > 0) { ... if (x < 0)` to warn always false
// their purpose for now is to show, that data flow is not only about smart casts, but eventually for other facts also
struct FactsAboutExpr {
  TypePtr expr_type;        // originally declared type or smart cast (Unknown if no info)
  SignState sign_state;     // definitely positive, etc. (Unknown if no info)
  BoolState bool_state;     // definitely true/false (Unknown if no info)

  FactsAboutExpr()
    : expr_type(nullptr), sign_state(SignState::Unknown), bool_state(BoolState::Unknown) {}
  FactsAboutExpr(TypePtr smart_cast_type, SignState sign_state, BoolState bool_state)
    : expr_type(smart_cast_type), sign_state(sign_state), bool_state(bool_state) {}

  bool operator==(const FactsAboutExpr& rhs) const = default;
};

// FlowContext represents "everything known about control flow at a given execution point"
// while traversing AST, each statement node gets "in" FlowContext (prior knowledge)
// and returns "output" FlowContext (representing a state AFTER execution of a statement)
// on branching, like if/else, input context is cloned, two contexts for each branch calculated, and merged to a result
class FlowContext {
  // std::map, not std::unordered_map, because LLDB visualises it better, for debugging
  std::map<SinkExpression, FactsAboutExpr> known_facts;     // all local vars plus (optionally) indices/fields of tensors/tuples/objects
  bool unreachable = false;                                 // if execution can't reach this point (after `return`, for example)

  FlowContext(std::map<SinkExpression, FactsAboutExpr>&& known_facts, bool unreachable)
    : known_facts(std::move(known_facts)), unreachable(unreachable) {}

  void invalidate_all_subfields(LocalVarPtr var_ref, uint64_t parent_path, uint64_t parent_mask);

  friend std::ostream& operator<<(std::ostream& os, const FlowContext& flow);

public:
  FlowContext() = default;
  FlowContext(FlowContext&&) noexcept = default;
  FlowContext(const FlowContext&) = delete;
  FlowContext& operator=(FlowContext&&) = default;
  FlowContext& operator=(const FlowContext&) = delete;

  FlowContext clone() const {
    std::map<SinkExpression, FactsAboutExpr> copy = known_facts;
    return FlowContext(std::move(copy), unreachable);
  }

  bool is_unreachable() const { return unreachable; }

  TypePtr smart_cast_if_exists(SinkExpression s_expr) const {
    auto it = known_facts.find(s_expr);
    return it == known_facts.end() ? nullptr : it->second.expr_type;
  }

  void register_known_type(SinkExpression s_expr, TypePtr assigned_type);
  void mark_unreachable(UnreachableKind reason);

  static FlowContext merge_flow(FlowContext&& c1, FlowContext&& c2);
};

struct ExprFlow {
  FlowContext out_flow;

  // only calculated inside `if`, left of `&&`, etc. — there this expression is immediate condition, empty otherwise
  FlowContext true_flow;
  FlowContext false_flow;

  ExprFlow(FlowContext&& out_flow, FlowContext&& true_flow, FlowContext&& false_flow)
    : out_flow(std::move(out_flow))
    , true_flow(std::move(true_flow))
    , false_flow(std::move(false_flow)) {}
  ExprFlow(FlowContext&& out_flow, const bool clone_flow_for_condition)
    : out_flow(std::move(out_flow)) {
    if (clone_flow_for_condition) {
      true_flow = this->out_flow.clone();
      false_flow = this->out_flow.clone();
    }
  }

  ExprFlow(ExprFlow&&) noexcept = default;
  ExprFlow(const ExprFlow&) = delete;
  ExprFlow& operator=(ExprFlow&&) = delete;
  ExprFlow& operator=(const ExprFlow&) = delete;

  int get_always_true_false_state() const {
    if (true_flow.is_unreachable() != false_flow.is_unreachable()) {
      return false_flow.is_unreachable() ? 1 : 2;   // 1 is "always true"
    }
    return 0;
  }
};

std::ostream& operator<<(std::ostream& os, const FactsAboutExpr& facts);
std::ostream& operator<<(std::ostream& os, const FlowContext& flow);
TypePtr calculate_type_subtract_rhs_type(TypePtr type, TypePtr subtract_type);
SinkExpression extract_sink_expression_from_vertex(AnyExprV v);
TypePtr calc_declared_type_before_smart_cast(AnyExprV v);
TypePtr calc_smart_cast_type_on_assignment(TypePtr lhs_declared_type, TypePtr rhs_inferred_type);

} // namespace tolk
