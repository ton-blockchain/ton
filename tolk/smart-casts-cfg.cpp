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
#include "smart-casts-cfg.h"
#include "ast.h"
#include "tolk.h"

/*
 *   This file represents internals of AST-level control flow and data flow analysis.
 *   Data flow is mostly used for smart casts and is calculated AT THE TIME of type inferring.
 * Not before, not after, but simultaneously with type inferring, because any local variable can be smart cast,
 * which affects other expressions/variables types, generics instantiation, return auto-infer, etc.
 *   Though it's a part of type inferring, it's extracted as a separate file to keep inferring a bit clearer.
 *
 *   Control flow is represented NOT as a "graph with edges". Instead, it's a "structured DFS" for the AST:
 * 1) at every point of inferring, we have "current flow facts" (FlowContext)
 * 2) when we see an `if (...)`, we create two derived contexts (by cloning current)
 * 3) after `if`, finalize them at the end and unify
 * 4) if we detect unreachable code, we mark that path's context as "unreachable"
 *   In other words, we get the effect of a CFG but in a more direct approach. That's enough for AST-level data-flow.
 *
 *   FlowContext contains "data-flow facts that are definitely known": variables types (original or refined),
 * sign state (definitely positive, definitely zero, etc.), boolean state (definitely true, definitely false).
 * Each local variable is contained there, and possibly sub-fields of tensors/objects if definitely known:
 *      // current facts: x is int?, t is (int, int)
 *      if (x != null && t.0 > 0)
 *         // current facts: x is int, t is (int, int), t.0 is positive
 *      else
 *         // current facts: x is null, t is (int, int), t.0 is not positive
 *   When branches rejoin, facts are merged back (int+null = int? and so on, here they would be equal to before if).
 *   Another example:
 *      // current facts: x is int?
 *      if (x == null) {
 *          // current facts: x is null
 *          x = 1;
 *          // current facts: x is int
 *      }   // else branch is empty, its facts are: x is int
 *      // current facts (after rejoin): x is int
 *
 *   Every expression analysis result (performed along with type inferring) returns ExprFlow:
 * 1) out_flow: facts after evaluating the whole expression, no matter how it evaluates (true or false)
 * 2) true_flow: the environment if expression is definitely true
 * 3) false_flow: the environment if expression is definitely false
 *
 *   Note, that globals are NOT analyzed (smart casts work for locals only). The explanation is simple:
 * don't encourage to use a global twice, it costs gas, better assign it to a local.
 * See SinkExpression.
 *
 *   An important highlight about internal structure of tensors / tuples / objects and `t.1` sink expressions.
 *   When a tensor/object is assigned, its fields are NOT tracked individually.
 *   For better understanding, I'll give some examples in TypeScript (having the same behavior):
 *      interface User { id: number | string, ... }
 *      var u: User = { id: 123, ... }
 *      u.id    // it's number|string, not number
 *      u = { id: 'asdf', ... }
 *      u.id    // it's number|string, not string
 *      if (typeof u.id === 'string') {
 *          // here `u.id` is string (smart cast)
 *      }
 *      u.id = 123;
 *      u.id    // now it's number (smart cast) (until `u.id` or `u` are reassigned)
 *      // but `u` still has type `{ id: number | string, ... }`, not `{ id: number, ... }`; only `u.id` is refined
 *   The same example, but with nullable tensor in Tolk:
 *      var t: (int?, ...) = (123, ...)
 *      t.0     // it's int?, not int
 *      t = (null, ...)
 *      t.0     // it's int?, not null
 *      if (t.0 == null) {
 *          // here `t.0` is null (smart cast)
 *      }
 *      t.0 = 123;
 *      t.0     // now it's int (smart cast) (until `t.0` or `t` are reassigned)
 *      // but `t` still has type `(int?, ...)`, not `(int, ...)`; only `t.0` is refined
 *
 *   In the future, not only smart casts, but other data-flow analysis can be implemented.
 * 1) detect signs: `if (x > 0) { ... if (x < 0)` to warn always false
 * 2) detect always true/false: `if (x) { return; } ... if (!x)` to warn always true
 *   These potential improvements are SignState and BoolState. Now they are NOT IMPLEMENTED, though declared.
 * Their purpose is to show, that data flow is not only about smart casts, but eventually for other facts also.
 * (though it's not obvious whether they should be analyzed at AST level or at IR level, like constants now)
 */

namespace tolk {

std::string SinkExpression::to_string() const {
  std::string result = var_ref->name;
  uint64_t cur_path = index_path;
  while (cur_path != 0) {
    result += ".";
    result += std::to_string((cur_path & 0xFF) - 1);
    cur_path >>= 8;
  }
  return result;
}

static std::string to_string(SignState s) {
  static const char* txt[6 + 1] = {"sign=unknown", ">0", "<0", "=0", ">=0", "<=0", "sign=never"};
  return txt[static_cast<int>(s)];
}

static std::string to_string(BoolState s) {
  static const char* txt[4 + 1] = {"unknown", "always_true", "always_false", "bool=never"};
  return txt[static_cast<int>(s)];
}

// from `expr!` get `expr`
static AnyExprV unwrap_not_null_operator(AnyExprV expr) {
  while (auto v_not_null = expr->try_as<ast_not_null_operator>()) {
    expr = v_not_null->get_expr();
  }
  return expr;
}

// "type lca" for a and b is T, so that both are assignable to T
// it's used
// 1) for auto-infer return type of the function if not specified
//    example: `fun f(x: int?) { ... return 1; ... return x; }`; lca(`int`,`int?`) = `int?`
// 2) for auto-infer type of ternary and `match` expressions
//    example: `cond ? beginCell() : null`; lca(`builder`,`null`) = `builder?`
// 3) when two data flows rejoin
//    example: `if (tensorVar != null) ... else ...` rejoin `(int,int)` and `null` into `(int,int)?`
// when lca can't be calculated (example: `(int,int)` and `(int,int,int)`), nullptr is returned
static TypePtr calculate_type_lca(TypePtr a, TypePtr b) {
  if (a == b) {
    return a;
  }
  if (a == TypeDataNever::create()) {
    return b;
  }
  if (b == TypeDataNever::create()) {
    return a;
  }

  if (a->can_rhs_be_assigned(b)) {
    return a;
  }
  if (b->can_rhs_be_assigned(a)) {
    return b;
  }

  if (a == TypeDataUnknown::create() || b == TypeDataUnknown::create()) {
    return TypeDataUnknown::create();
  }

  if (a == TypeDataNullLiteral::create()) {
    return TypeDataNullable::create(b);
  }
  if (b == TypeDataNullLiteral::create()) {
    return TypeDataNullable::create(a);
  }

  const auto* tensor1 = a->try_as<TypeDataTensor>();
  const auto* tensor2 = b->try_as<TypeDataTensor>();
  if (tensor1 && tensor2 && tensor1->size() == tensor2->size()) {
    std::vector<TypePtr> types_lca;
    types_lca.reserve(tensor1->size());
    for (int i = 0; i < tensor1->size(); ++i) {
      TypePtr next = calculate_type_lca(tensor1->items[i], tensor2->items[i]);
      if (next == nullptr) {
        return nullptr;
      }
      types_lca.push_back(next);
    }
    return TypeDataTensor::create(std::move(types_lca));
  }

  const auto* tuple1 = a->try_as<TypeDataTypedTuple>();
  const auto* tuple2 = b->try_as<TypeDataTypedTuple>();
  if (tuple1 && tuple2 && tuple1->size() == tuple2->size()) {
    std::vector<TypePtr> types_lca;
    types_lca.reserve(tuple1->size());
    for (int i = 0; i < tuple1->size(); ++i) {
      TypePtr next = calculate_type_lca(tuple1->items[i], tuple2->items[i]);
      if (next == nullptr) {
        return nullptr;
      }
      types_lca.push_back(next);
    }
    return TypeDataTypedTuple::create(std::move(types_lca));
  }

  if (a->try_as<TypeDataIntN>() && b->try_as<TypeDataIntN>()) {   // cond ? int32 : int64
    return TypeDataInt::create();
  }

  return nullptr;
}

// merge (unify) of two sign states: what sign do we definitely have
// it's used on data flow rejoin
// example: `if (x > 0) ... else ...`; lca(Positive, NonPositive) = Unknown
SignState calculate_sign_lca(SignState a, SignState b) {
  using s = SignState;
  // a transformation lookup table, using the following rules:
  // 1) if one is Unknown, the result is Unknown ("no definite constraints")
  // 2) if one is Never (can't happen), the result is the other
  //    example: x is known > 0 already, given code `if (x > 0) {} else {}` merges Positive (always true) and Never
  // 3) handle all other combinations carefully
  static constexpr SignState transformations[7][7] = {
    //               b=        Unknown |   Positive    |    Negative   |      Zero     |  NonNegative  |  NonPositive  |    Never     |
    /* a=Unknown     */ {s::Unknown, s::Unknown,     s::Unknown,     s::Unknown,     s::Unknown,     s::Unknown,     s::Unknown    },
    /* a=Positive    */ {s::Unknown, s::Positive,    s::Unknown,     s::NonNegative, s::NonNegative, s::Unknown,     s::Positive   },
    /* a=Negative    */ {s::Unknown, s::Unknown,     s::Negative,    s::NonPositive, s::Unknown,     s::NonPositive, s::Negative   },
    /* a=Zero        */ {s::Unknown, s::NonNegative, s::NonPositive, s::Zero,        s::NonNegative, s::NonPositive, s::Zero       },
    /* a=NonNegative */ {s::Unknown, s::NonNegative, s::Unknown,     s::NonNegative, s::NonNegative, s::Unknown,     s::NonNegative},
    /* a=NonPositive */ {s::Unknown, s::Unknown,     s::NonPositive, s::NonPositive, s::Unknown,     s::NonPositive, s::NonPositive},
    /* a=Never       */ {s::Unknown, s::Positive,    s::Negative,    s::Zero,        s::NonNegative, s::NonPositive, s::Never      }
  };

  return transformations[static_cast<int>(a)][static_cast<int>(b)];
}

// merge (unify) two bool state: what state do we definitely have
// it's used on data flow rejoin
// example: `if (x) ... else ...`; lca(AlwaysTrue, AlwaysFalse) = Unknown
BoolState calculate_bool_lca(BoolState a, BoolState b) {
  using s = BoolState;
  static constexpr BoolState transformations[4][4] = {
    //               b=        Unknown |  AlwaysTrue   |  AlwaysFalse  |    Never     |
    /* a=Unknown     */ {s::Unknown, s::Unknown,     s::Unknown,     s::Unknown    },
    /* a=AlwaysTrue  */ {s::Unknown, s::AlwaysTrue,  s::Unknown,     s::AlwaysTrue },
    /* a=AlwaysFalse */ {s::Unknown, s::Unknown,     s::AlwaysFalse, s::AlwaysFalse},
    /* a=Never       */ {s::Unknown, s::AlwaysTrue,  s::AlwaysFalse, s::Never      }
  };

  return transformations[static_cast<int>(a)][static_cast<int>(b)];
}

// see comments above TypeInferringUnifyStrategy
// this function calculates lca or currently stored result and next
bool TypeInferringUnifyStrategy::unify_with(TypePtr next) {
  if (unified_result == nullptr) {
    unified_result = next;
    return true;
  }
  if (unified_result == next) {
    return true;
  }

  TypePtr combined = calculate_type_lca(unified_result, next);
  if (!combined) {
    return false;
  }

  unified_result = combined;
  return true;
}

bool TypeInferringUnifyStrategy::unify_with_implicit_return_void() {
  if (unified_result == nullptr) {
    unified_result = TypeDataVoid::create();
    return true;
  }

  return unified_result == TypeDataVoid::create();
}

// invalidate knowledge about sub-fields of a variable or its field
// example: `tensorVar = 2`, invalidate facts about `tensorVar`, `tensorVar.0`, `tensorVar.1.2`, and all others
// example: `user.id = rhs`, invalidate facts about `user.id` (sign, etc.) and `user.id.*` if exist
void FlowContext::invalidate_all_subfields(LocalVarPtr var_ref, uint64_t parent_path, uint64_t parent_mask) {
  for (auto it = known_facts.begin(); it != known_facts.end();) {
    bool is_self_or_field = it->first.var_ref == var_ref && (it->first.index_path & parent_mask) == parent_path;
    if (is_self_or_field) {
      it = known_facts.erase(it);
    } else {
      ++it;
    }
  }
}

// update current type of `local_var` / `tensorVar.0` / `obj.field`
// example: `local_var = rhs`
// example: `f(mutate obj.field)`
// example: `if (t.0 != null)`, in true_flow `t.0` assigned to "not-null of current", in false_flow to null
void FlowContext::register_known_type(SinkExpression s_expr, TypePtr assigned_type) {
  // having index_path = (some bytes filled in the end),
  // calc index_mask: replace every filled byte with 0xFF
  // example: `t.0.1`, index_path = (1<<8) + 2, index_mask = 0xFFFF
  uint64_t index_path = s_expr.index_path;
  uint64_t index_mask = 0;
  while (index_path > 0) {
    index_mask = index_mask << 8 | 0xFF;
    index_path >>= 8;
  }
  invalidate_all_subfields(s_expr.var_ref, s_expr.index_path, index_mask);

  // if just `int` assigned, we have no considerations about its sign
  // so, even if something existed by the key s_expr, drop all knowledge
  known_facts[s_expr] = FactsAboutExpr(assigned_type, SignState::Unknown, BoolState::Unknown);
}

// mark control flow unreachable / interrupted
void FlowContext::mark_unreachable(UnreachableKind reason) {
  unreachable = true;
  // currently we don't save why control flow became unreachable (it's not obvious how, there may be consequent reasons),
  // but it helps debugging and reading outer code
  static_cast<void>(reason);
}


// "merge" two data-flow contexts occurs on control flow rejoins (if/else branches merging, for example)
// it's generating a new context that describes "knowledge that definitely outcomes from these two"
// example: in one branch x is `int`, in x is `null`, result is `int?` unless any of them is unreachable
FlowContext FlowContext::merge_flow(FlowContext&& c1, FlowContext&& c2) {
  if (!c1.unreachable && c2.unreachable) {
    return merge_flow(std::move(c2), std::move(c1));
  }

  std::map<SinkExpression, FactsAboutExpr> unified;

  if (c1.unreachable && !c2.unreachable) {
    // `if (...) return; else ...;` — copy facts about common variables only from else (c2)
    for (const auto& [s_expr, i2] : c2.known_facts) {
      auto it1 = c1.known_facts.find(s_expr);
      bool need_add = it1 != c1.known_facts.end() || s_expr.index_path != 0;
      if (need_add) {
        unified.emplace(s_expr, i2);
      }
    }

  } else {
    // either both reachable, or both not — merge types and restrictions of common variables and fields
    for (const auto& [s_expr, i1] : c1.known_facts) {
      if (auto it2 = c2.known_facts.find(s_expr); it2 != c2.known_facts.end()) {
        const FactsAboutExpr& i2 = it2->second;
        unified.emplace(s_expr, i1 == i2 ? i1 : FactsAboutExpr(
          calculate_type_lca(i1.expr_type, i2.expr_type),
          calculate_sign_lca(i1.sign_state, i2.sign_state),
          calculate_bool_lca(i1.bool_state, i2.bool_state)
        ));
      }
    }
  }

  return FlowContext(std::move(unified), c1.unreachable && c2.unreachable);
}

// return `T`, so that `T?` = type
// what for: `if (x != null)`, to smart cast x inside if
TypePtr calculate_type_subtract_null(TypePtr type) {
  if (const auto* as_nullable = type->try_as<TypeDataNullable>()) {
    return as_nullable->inner;
  }
  // union types will be handled here
  return TypeDataNever::create();
}

// given any expression vertex, extract SinkExpression is possible
// example: `x.0` is { var_ref: x, index_path: 1 }
// example: `x.1` is { var_ref: x, index_path: 2 }
// example: `x!.1` is the same
// example: `x.1.2` is { var_ref: x, index_path: 2<<8 + 3 }
// example: `x!.1!.2` is the same
// not SinkExpressions: `globalVar` / `f()` / `obj.method().1`
SinkExpression extract_sink_expression_from_vertex(AnyExprV v) {
  if (auto as_ref = v->try_as<ast_reference>()) {
    if (LocalVarPtr var_ref = as_ref->sym->try_as<LocalVarPtr>()) {
      return SinkExpression(var_ref);
    }
  }

  if (auto as_dot = v->try_as<ast_dot_access>(); as_dot && as_dot->is_target_indexed_access()) {
    V<ast_dot_access> cur_dot = as_dot;
    uint64_t index_path = 0;
    while (cur_dot->is_target_indexed_access()) {
      int index_at = std::get<int>(cur_dot->target);
      index_path = (index_path << 8) + index_at + 1;
      if (auto parent_dot = unwrap_not_null_operator(cur_dot->get_obj())->try_as<ast_dot_access>()) {
        cur_dot = parent_dot;
      } else {
        break;
      }
    }
    if (auto as_ref = unwrap_not_null_operator(cur_dot->get_obj())->try_as<ast_reference>()) {
      if (LocalVarPtr var_ref = as_ref->sym->try_as<LocalVarPtr>()) {
        return SinkExpression(var_ref, index_path);
      }
    }
  }

  if (auto as_par = v->try_as<ast_parenthesized_expression>()) {
    return extract_sink_expression_from_vertex(as_par->get_expr());
  }

  if (auto as_assign = v->try_as<ast_assign>()) {
    return extract_sink_expression_from_vertex(as_assign->get_lhs());
  }

  return {};
}

// given `lhs = rhs`, calculate "original" type of `lhs`
// example: `var x: int? = ...; if (x != null) { x (here) = null; }`
// "(here)" x is `int` (smart cast), but originally declared as `int?`
// example: `if (x is (int,int)?) { x!.0 = rhs }`, here `x!.0` is `int`
TypePtr calc_declared_type_before_smart_cast(AnyExprV v) {
  if (auto as_ref = v->try_as<ast_reference>()) {
    if (LocalVarPtr var_ref = as_ref->sym->try_as<LocalVarPtr>()) {
      return var_ref->declared_type;
    }
  }

  if (auto as_dot = v->try_as<ast_dot_access>(); as_dot && as_dot->is_target_indexed_access()) {
    TypePtr obj_type = as_dot->get_obj()->inferred_type;    // v already inferred; hence, index_at is correct
    int index_at = std::get<int>(as_dot->target);
    if (const auto* t_tensor = obj_type->try_as<TypeDataTensor>()) {
      return t_tensor->items[index_at];
    }
    if (const auto* t_tuple = obj_type->try_as<TypeDataTypedTuple>()) {
      return t_tuple->items[index_at];
    }
  }

  return v->inferred_type;
}

// given `lhs = rhs` (and `var x = rhs`), calculate probable smart cast for lhs
// it's NOT directly type of rhs! see comment at the top of the file about internal structure of tensors/tuples.
// obvious example: `var x: int? = 5`, it's `int` (most cases are like this)
// obvious example: `var x: (int,int)? = null`, it's `null` (`x == null` is always true, `x` can be passed to any `T?`)
// not obvious example: `var x: (int?, int?)? = (3,null)`, result is `(int?,int?)`, whereas type of rhs is `(int,null)`
TypePtr calc_smart_cast_type_on_assignment(TypePtr lhs_declared_type, TypePtr rhs_inferred_type) {
  // assign `T` to `T?` (or at least "assignable-to-T" to "T?")
  // smart cast to `T`
  if (const auto* lhs_nullable = lhs_declared_type->try_as<TypeDataNullable>()) {
    if (lhs_nullable->inner->can_rhs_be_assigned(rhs_inferred_type)) {
      return lhs_nullable->inner;
    }
  }

  // assign `null` to `T?`
  // smart cast to `null`
  if (lhs_declared_type->try_as<TypeDataNullable>() && rhs_inferred_type == TypeDataNullLiteral::create()) {
    return TypeDataNullLiteral::create();
  }

  // no smart cast, type is the same as declared
  // example: `var x: (int?,slice?) = (1, null)`, it's `(int?,slice?)`, not `(int,null)`
  return lhs_declared_type;
}


std::ostream& operator<<(std::ostream& os, const FlowContext& flow) {
  os << "(" << flow.known_facts.size() << " facts) " << (flow.unreachable ? "(unreachable) " : "");
  for (const auto& [s_expr, facts] : flow.known_facts) {
    os << ", " << s_expr.to_string() << ": " << facts;
  }
  return os;
}

std::ostream& operator<<(std::ostream& os, const FactsAboutExpr& facts) {
  os << facts.expr_type;
  if (facts.sign_state != SignState::Unknown) {
    os << " " << to_string(facts.sign_state);
  }
  if (facts.bool_state != BoolState::Unknown) {
    os << " " << to_string(facts.bool_state);
  }
  return os;
}

} // namespace tolk
