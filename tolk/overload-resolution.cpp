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
#include "overload-resolution.h"
#include "type-system.h"
#include "compiler-state.h"

/*
 *   Find an exact method having a receiver type.
 *
 *   Given: int.copy, T.copy, Container<T>.copy
 * > 5.copy();                       // 1
 * > (5 as int8).copy();             // 2 with T=int8
 * > containerOfInt.copy();          // 3 with T=int
 * > nullableContainerOfInt.copy();  // 2 with T=Container<int>?
 *
 */

namespace tolk {

// each next shape kind is more specific than another;
// e.g., between `T.copy` and `int.copy` we choose the second;
enum class ShapeKind {
  GenericT,     // T
  Union,        // U|V, T?
  Primitive,    // int, slice, address, ...
  Tensor,       // (A,B,...)
  Instantiated, // Map<K,V>, Container<T>, Struct<X>, ...
};

// for every receiver, we calculate "score": how deep and specific it is;
// e.g., between `Container<T>` and `T` we choose the first;
// e.g., between `map<int8, V>` and `map<K, map<K, K>>` we choose the second;
struct ShapeScore {
  ShapeKind kind;
  int depth;

  bool is_shape_better_than(const ShapeScore& rhs) const {
    if (kind != rhs.kind) {
      return kind > rhs.kind;
    }
    return depth > rhs.depth;
  }

  bool operator==(const ShapeScore& rhs) const = default;
};

// calculate score for a receiver;
// note: it's an original receiver, with generics, not an instantiated one
static ShapeScore calculate_shape_score(TypePtr t) {
  if (t->try_as<TypeDataGenericT>()) {
    return {ShapeKind::GenericT, 1};
  }

  if (const auto* t_union = t->try_as<TypeDataUnion>()) {
    int d = 0;
    for (TypePtr variant : t_union->variants) {
      d = std::max(d, calculate_shape_score(variant).depth);
    }
    return {ShapeKind::Union, 1 + d};
  }

  if (const auto* t_tensor = t->try_as<TypeDataTensor>()) {
    int d = 0;
    for (TypePtr item : t_tensor->items) {
      d = std::max(d, calculate_shape_score(item).depth);
    }
    return {ShapeKind::Tensor, 1 + d};
  }

  if (const auto* t_brackets = t->try_as<TypeDataBrackets>()) {
    int d = 0;
    for (TypePtr item : t_brackets->items) {
      d = std::max(d, calculate_shape_score(item).depth);
    }
    return {ShapeKind::Tensor, 1 + d};
  }

  if (const auto* t_instTs = t->try_as<TypeDataGenericTypeWithTs>()) {
    int d = 0;
    for (TypePtr typeT : t_instTs->type_arguments) {
      d = std::max(d, calculate_shape_score(typeT).depth);
    }
    return {ShapeKind::Instantiated, 1 + d};
  }

  if (const auto* t_map = t->try_as<TypeDataMapKV>()) {
    int d = std::max(calculate_shape_score(t_map->TKey).depth, calculate_shape_score(t_map->TValue).depth);
    return {ShapeKind::Instantiated, 1 + d};
  }

  if (const auto* t_alias = t->try_as<TypeDataAlias>()) {
    return calculate_shape_score(t_alias->underlying_type);
  }

  return {ShapeKind::Primitive, 1};
}

// tries to find Ts in `pattern` to reach `actual`;
// example: pattern=`map<K, slice>`, actual=`map<int, slice>` => T=int
// example: pattern=`Container<T>`, actual=`Container<Container<U>>` => T=Container<U> 
static bool can_substitute_Ts_to_reach_actual(TypePtr pattern, TypePtr actual, const GenericsDeclaration* genericTs) {
  GenericSubstitutionsDeducing deducingTs(genericTs);
  TypePtr replaced = deducingTs.auto_deduce_from_argument(pattern, actual);
  return replaced->equal_to(actual);
}

// checks whether a generic typeA is more specific than typeB;
// example: `map<int,V>` dominates `map<K,V>`;
// example: `map<K, map<K,K>>` dominates `map<K, map<K,V>>` dominates `map<K1, map<K2,V>>`; 
// example: `map<int,V>` and `map<K,slice>` are not comparable;
static bool is_more_specific_generic(TypePtr typeA, TypePtr typeB, const GenericsDeclaration* genericTsA, const GenericsDeclaration* genericTsB) {
  // exists θ: θ(B)=A && not exists φ: φ(A)=B
  return can_substitute_Ts_to_reach_actual(typeB, typeA, genericTsB)
     && !can_substitute_Ts_to_reach_actual(typeA, typeB, genericTsA);
}

// the main "overload resolution" entrypoint: given `obj.method()`, find best applicable methods;
// if there are many (no one is better than others), a caller side will emit "ambiguous call"
std::vector<MethodCallCandidate> resolve_methods_for_call(TypePtr provided_receiver, std::string_view called_name) {
  // find all methods theoretically applicable; we'll filter them by priority;
  // for instance, if there is `T.method`, it will be instantiated with T=provided_receiver
  std::vector<MethodCallCandidate> viable;
  for (FunctionPtr method_ref : G.all_methods) {
    if (method_ref->method_name == called_name) {
      TypePtr receiver = method_ref->receiver_type;
      if (receiver->has_genericT_inside()) {
        try {   // check whether exist some T to make it a valid call (probably with type coercion)
          GenericSubstitutionsDeducing deducingTs(method_ref);
          TypePtr replaced = deducingTs.auto_deduce_from_argument(receiver, provided_receiver);
          if (replaced->can_rhs_be_assigned(provided_receiver)) {
            viable.emplace_back(receiver, replaced, method_ref, deducingTs.flush());
          }
        } catch (...) {}
      } else if (receiver->can_rhs_be_assigned(provided_receiver)) {
        viable.emplace_back(receiver, receiver, method_ref, GenericsSubstitutions(method_ref->genericTs));
      }
    }
  }
  // if nothing found, return nothing;
  // if the only found, it's the one
  if (viable.size() <= 1) {
    return viable;
  }
  // okay, we have multiple viable methods, and need to locate the better

  // 1) exact match candidates with equal_to()
  //    (for instance, an alias equals to its underlying type, as well as `T1|T2` equals to `T2|T1`)
  std::vector<MethodCallCandidate> exact;
  for (const MethodCallCandidate& candidate : viable) {
    if (candidate.instantiated_receiver->equal_to(provided_receiver)) {
      exact.push_back(candidate);
    }
  }
  if (exact.size() == 1) {
    return exact;
  }
  if (!exact.empty()) {
    viable = std::move(exact);
  }

  // 2) if there are both generic and non-generic functions, filter out generic
  size_t n_generics = 0;
  for (const MethodCallCandidate& candidate : viable) {
    n_generics += candidate.is_generic();
  }
  if (n_generics < viable.size()) {
    std::vector<MethodCallCandidate> non_generic;
    for (const MethodCallCandidate& candidate : viable) {
      if (!candidate.is_generic()) {
        non_generic.push_back(candidate);
      }
    }
    // all the code below is dedicated to choosing between generic Ts, so return if non-generic
    return non_generic;
  }

  // 3) better shape in terms of structural depth
  //    (prefer `Container<T>` over `T` and `map<K1, map<K2,V2>>` over `map<K,V>`)
  ShapeScore best_shape = {ShapeKind::GenericT, -999};
  for (const MethodCallCandidate& candidate : viable) {
    ShapeScore s = calculate_shape_score(candidate.original_receiver);
    if (s.is_shape_better_than(best_shape)) {
      best_shape = s;
    }
  }
  
  std::vector<MethodCallCandidate> best_by_shape;
  for (const MethodCallCandidate& candidate : viable) {
    if (calculate_shape_score(candidate.original_receiver) == best_shape) {
      best_by_shape.push_back(candidate);
    }
  }
  if (best_by_shape.size() == 1) {
    return best_by_shape;
  }
  if (!best_by_shape.empty()) {
    viable = std::move(best_by_shape);
  }

  // 4) find the overload that dominates all others
  //    (prefer `Container<int>` over `Container<T>` and `map<K, slice>` over `map<K, V>`)
  const MethodCallCandidate* dominator = nullptr;
  for (const MethodCallCandidate& candidate : viable) {
    bool dominates_all = true;
    for (const MethodCallCandidate& other : viable) {
      if (candidate.method_ref != other.method_ref) {
        dominates_all &= is_more_specific_generic(candidate.original_receiver, other.original_receiver, candidate.method_ref->genericTs, other.method_ref->genericTs);
      }
    }
    if (dominates_all) {
      tolk_assert(!dominator);
      dominator = &candidate;
    }
  }

  if (dominator) {
    return {*dominator};
  }
  return viable;
}

} // namespace tolk
