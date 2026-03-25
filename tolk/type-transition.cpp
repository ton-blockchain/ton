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
#include "generics-helpers.h"
#include "tolk.h"
#include "type-system.h"

/*
 *   "Transition to the destination (runtime) type" is the following process.
 *   Imagine `fun analyze(t: (int,int)?)` and a call `analyze((1,2))`.
 * `(1,2)` (inferred_type) is 2 stack slots, but `t` (target_type, or dest_type) is 3 (one for null-flag).
 *   So, this null flag should be implicitly added (non-zero, since a variable is not null).
 *   Another example: `var t: (int, int)? = null`.
 * `null` (inferred_type) is 1 stack slots, but target_type is 3, we should transform it to `(null, null, 0)`.
 *   Another example: `var t1 = (1, null); var t2: (int, (int,int)?) = t1;`.
 * Then t1's rvect is 2 vars (1 and null), but t1's `null` should be converted to 3 stack slots (resulting in 4 total).
 *   The same mechanism works for union types, but there is a union tag (UTag) slot instead of null flag.
 * `var i: int|slice = 5;`. This "5" is represented as "5 1" (5 for value, 1 is type_id of `int`).
 *
 *   In code, transitioning is triggered by smart casts, `as` operator, etc.
 *   Moreover, there are some transitions that are not supported by `as`, but still exist.
 * For example, narrowing `T1 | T2 | T3` to `T1`, which is required for the `match` statement.
 */

namespace tolk {

std::vector<var_idx_t> transition_rvect_to_runtime_type(std::vector<var_idx_t>&& rvect, CodeBlob& code, TypePtr from_type, TypePtr dest_type, AnyV origin);

// having `a = b` where `a` is `array<int?>`, b is `array<int>` (from=`int`, dest=`int?`),
// check that this assignment can be done atomically, without unpacking a tuple and moving every item
// example when can: `array<int> to array<int?>`, `[Point, Point] to [unknown, unknown]`
// example when not: `array<Point> to array<Point?>`, because of transformation `[x y]` to `[x y type-id]`
static bool can_safely_move_inside_tuple(TypePtr from, TypePtr dest) {
  if (from->equal_to(dest) || dest == TypeDataUnknown::create()) {
    return true;
  }
  if (from->get_width_on_stack() != 1 || dest->get_width_on_stack() != 1) {
    return false;
  }

  CodeBlob tmp(nullptr);
  std::vector same = transition_rvect_to_runtime_type({-1}, tmp, from, dest, nullptr);
  return same[0] == -1 && tmp.var_cnt == 0;
}

std::vector<var_idx_t> transition_rvect_to_runtime_type(std::vector<var_idx_t>&& rvect, CodeBlob& code, TypePtr from_type, TypePtr dest_type, AnyV origin) {
#ifdef TOLK_DEBUG
  tolk_assert(static_cast<int>(rvect.size()) == from_type->get_width_on_stack());
#endif

  // aliases are erased at the TVM level
  from_type = from_type->unwrap_alias();
  dest_type = dest_type->unwrap_alias();

  if (dest_type == from_type) {
    return rvect;
  }
  if (dest_type->equal_to(from_type)) {
    return rvect;
  }

  int from_slots = from_type->get_width_on_stack();   // = rvect.size()
  int dest_slots = dest_type->get_width_on_stack();

  // transform `int` to `int8` / `coins` / `Color` / etc. (enums also)
  // have: [INT]
  // need: [INT]
  bool from_is_int = from_type == TypeDataInt::create() || from_type == TypeDataCoins::create()
                  || from_type->try_as<TypeDataIntN>()  || from_type->try_as<TypeDataEnum>();
  bool dest_is_int = dest_type == TypeDataInt::create() || dest_type == TypeDataCoins::create()
                  || dest_type->try_as<TypeDataIntN>()  || dest_type->try_as<TypeDataEnum>();
  if (from_is_int && dest_is_int) {
    return rvect;
  }

  // transform `bool` to an integer
  // have: [INT] — -1 or 0
  // need: [INT]
  if (from_type == TypeDataBool::create() && dest_is_int) {
    return rvect;
  }
  
  // transform `slice` to `address` / `bits64` / etc.
  // have: [SLICE]
  // need: [SLICE]
  bool from_is_slice = from_type == TypeDataSlice::create()
                    || from_type->try_as<TypeDataAddress>() || from_type->try_as<TypeDataBitsN>();
  bool dest_is_slice = dest_type == TypeDataSlice::create()
                    || dest_type->try_as<TypeDataAddress>() || dest_type->try_as<TypeDataBitsN>();
  if (from_is_slice && dest_is_slice) {
    return rvect;
  }

  // handle `never`
  // it occurs in unreachable branches, for example `if (intVal == null) { return intVal; }`
  // we can't do anything reasonable here, but (hopefully) execution will never reach this point, and stack won't be polluted
  if (dest_type == TypeDataNever::create() || from_type == TypeDataNever::create()) {
    return rvect;
  }

  // transform anything to `unknown`
  // type `unknown` can not occur inside a union (`5 as int|unknown` is forbidden), so do this before handling unions
  // have: [value] or [slot ... slot]
  // need: [value] or [tuple]
  if (dest_type == TypeDataUnknown::create()) {
    if (from_slots == 1) {
      return rvect;
    }
    std::vector ir_to_tuple = code.create_tmp_var(TypeDataUnknown::create(), origin, "(to-unknown)");
    code.add_to_tuple(origin, ir_to_tuple, std::move(rvect));
    return ir_to_tuple;
  }

  // transform `unknown` to anything
  // have: [value] or [tuple] (compile-time known that a tuple is of size N!=1)
  // need: [value] or [slot ... slot]
  if (from_type == TypeDataUnknown::create()) {
    if (dest_slots == 1) {
      return rvect;
    }
    std::vector ir_from_tuple = code.create_tmp_var(dest_type, origin, "(from-unknown)");
    code.add_un_tuple(origin, ir_from_tuple, std::move(rvect));
    return ir_from_tuple;
  }

  // handle unions, there are many combinations, since tagged unions have special stack layout and optimizations
  const TypeDataUnion* from_union = from_type->try_as<TypeDataUnion>();
  const TypeDataUnion* dest_union = dest_type->try_as<TypeDataUnion>();

  // transform `null` to a primitive 1-slot nullable `T?`
  // - `null` to `int?`
  // - `null` to `Color?`
  // have: [NULL]
  // need: [NULL] — a representation of a null value `T?` since it's a primitive nullable 
  if (from_type == TypeDataNullLiteral::create() && dest_union && dest_union->is_primitive_nullable()) {
    return rvect;
  }
  
  // transform `null` to a wide nullable union `T?`
  // - `null` to `(int, int)?`
  // - `null` to `int | slice | null`
  // have: [NULL]
  // need: [NULL, NULL, ..., 0] (UTag=0 is the type_id of a null literal)
  if (from_type == TypeDataNullLiteral::create() && dest_union && dest_slots > 1) {
    tolk_assert(dest_union->has_null());
    FunctionPtr null_sym = lookup_function("__null");
    rvect.reserve(dest_slots);      // keep rvect[0], it's already null
    for (int i = 1; i < dest_slots - 1; ++i) {
      std::vector ith_null = code.create_tmp_var(TypeDataNullLiteral::create(), origin, "(null-literal)");
      code.add_call(origin, ith_null, {}, null_sym);
      rvect.push_back(ith_null[0]);
    }
    rvect.push_back(code.create_int(origin, 0, "(UTag)"));
    return rvect;
  }

  // transform `null` to a nullable empty type
  // - `null` to `()?`
  // - `null` to `Empty1 | Empty2 | null`
  // have: [NULL]
  // need: [0] (UTag for null)  
  if (from_type == TypeDataNullLiteral::create() && dest_union) {
    tolk_assert(dest_union->has_null() && dest_slots == 1);
    return {code.create_int(origin, 0, "(UTag)")};
  }

  // transform a primitive 1-slot nullable `T?` to `null`
  // - `int?` to `null`
  // - `Color?` to `null`
  // have: [NULL] — a primitive `T?` holding a null value 
  // need: [NULL]
  if (dest_type == TypeDataNullLiteral::create() && from_union && from_union->is_primitive_nullable()) {
    return rvect;
  }
  
  // transform a wide nullable union to `null`
  // - `(int, int)?` to `null`
  // - `int | slice | null` to `null`
  // have: [NULL, NULL, ..., 0]
  // need: [NULL]
  if (dest_type == TypeDataNullLiteral::create() && from_union && from_slots > 1) {
    tolk_assert(from_union->has_null());
    return {rvect[rvect.size() - 2]};
  }

  // transform a nullable empty type to `null`
  // - `()?` to `null`
  // - `Empty1 | Empty2 | null` to `null`
  // have: [0] (UTag for null)
  // need: [NULL]
  if (dest_type == TypeDataNullLiteral::create() && from_union) {
    tolk_assert(from_union->has_null() && from_slots == 1);
    FunctionPtr null_sym = lookup_function("__null");
    std::vector new_rvect = code.create_tmp_var(TypeDataNullLiteral::create(), origin, "(null-literal)");
    code.add_call(origin, new_rvect, {}, null_sym);
    return new_rvect;
  }

  // transform a wide nullable union to a primitive 1-slot `T?`
  // - `int | slice | null` to `slice?`
  // - `A | int | null` to `int?`
  // have: [NULL, NULL, ..., value, UTag] or [NULL, NULL, ..., NULL, 0]
  // need: [value] or [NULL]
  if (from_union && dest_union && dest_union->is_primitive_nullable()) {
    // nothing except "T1 | T2 | ... null" can be cast to 1-slot nullable `T1?`
    tolk_assert(from_slots > 1 && from_union->has_null() && from_union->has_variant_equal_to(dest_union->or_null));
    // conversion between unions does not require transition of rvect: dest already has `T`,
    // so the slot before UTag, if not null, already has a required shape
    return {rvect[rvect.size() - 2]};
  }

  // transform a primitive 1-slot `T?` to a wide nullable union
  // - `int?` to `int | slice | null`
  // - `slice?` to `(int, int) | slice | builder | null`
  // have: [value] or [NULL]
  // need: [NULL, NULL, ..., value, UTag] or [NULL, NULL, ..., NULL, 0]
  if (from_union && from_union->is_primitive_nullable() && dest_union) {
    tolk_assert(dest_union->has_null() && dest_union->has_variant_equal_to(from_union->or_null) && dest_slots > 1);
    // when value is null, we need to achieve "... NULL 0"         (value is already null, so "... value 0")
    // when value is not null, we need to get "... value UTag"
    // we place either 0 or UTag via the IF at runtime; luckily, this case is very uncommon in practice
    FunctionPtr null_sym = lookup_function("__null");
    std::vector<var_idx_t> new_rvect(dest_slots);
    for (int i = 0; i < dest_slots - 2; ++i) {    // N-1 nulls
      std::vector ith_null = code.create_tmp_var(TypeDataNullLiteral::create(), origin, "(null-literal)");
      code.add_call(origin, ith_null, {}, null_sym);
      new_rvect[i] = ith_null[0];
    }
    new_rvect[dest_slots - 2] = rvect[0];   // value
    new_rvect[dest_slots - 1] = code.create_tmp_var(TypeDataInt::create(), origin, "(UTag)")[0];

    std::vector ir_eq_null = code.create_tmp_var(TypeDataBool::create(), origin, "(value-is-null)");
    code.add_call(origin, ir_eq_null, rvect, lookup_function("__isNull"));
    Op& if_op = code.add_if_else(origin, ir_eq_null);
    code.push_set_cur(if_op.block0);
    code.add_int_const(origin, {new_rvect[dest_slots - 1]}, td::make_refint(0));
    code.close_pop_cur(origin);
    code.push_set_cur(if_op.block1);
    code.add_int_const(origin, {new_rvect[dest_slots - 1]}, td::make_refint(from_union->or_null->get_type_id()));
    code.close_pop_cur(origin);
    return new_rvect;
  }

  // transform a union type to a wider one
  // - `int | slice` to `int | slice | builder`
  // - `int | slice` to `int | (int, int) | slice | null`
  // have: [USlot1, ..., UTag]
  // need: [NULL, ..., USlot1, ..., UTag] — prepend by several nulls
  if (dest_union && from_union && dest_union->size() >= from_union->size()) {
    tolk_assert(dest_slots >= from_slots && dest_union->has_all_variants_of(from_union));
    std::vector<var_idx_t> prepend_nulls;
    prepend_nulls.reserve(dest_slots - from_slots);
    for (int i = 0; i < dest_slots - from_slots; ++i) {
      FunctionPtr null_sym = lookup_function("__null");
      std::vector ith_null = code.create_tmp_var(TypeDataNullLiteral::create(), origin, "(UVar.null)");
      prepend_nulls.push_back(ith_null[0]);
      code.add_call(origin, std::move(ith_null), {}, null_sym);
    }
    rvect.insert(rvect.begin(), prepend_nulls.begin(), prepend_nulls.end());
    return rvect;
  }

  // transform a union type to a narrower one
  // - `int | slice | builder` to `int | slice`
  // - `int | (int, int) | slice | null` to `int | slice`
  // have: [USlot1, ..., USlotN, ..., UTag]
  // need: [USlotN, ..., UTag] — cut off some slots from the left
  if (dest_union && from_union) {
    tolk_assert(dest_slots <= from_slots && from_union->has_all_variants_of(dest_union));
    rvect = std::vector(rvect.begin() + (from_slots - dest_slots), rvect.end());
    return rvect;
  }

  // transform a single type to a primitive nullable `T?`
  // - `int` to `int?`
  // - `[int, null]` to `[int8, [int, slice]?]?`
  // have: [value]
  // need: [value*] — a non-null slot, containing a transitioned value
  if (dest_union && !from_union && dest_union->is_primitive_nullable()) {
    TypePtr dest_subtype = dest_union->calculate_exact_variant_to_fit_rhs(from_type);
    tolk_assert(dest_subtype && from_slots == 1 && dest_slots == 1);
    if (from_type != dest_subtype) {    // quick false for `int` and other singleton pointers
      rvect = transition_rvect_to_runtime_type(std::move(rvect), code, from_type, dest_subtype, origin);
      tolk_assert(rvect.size() == 1);
    }
    return rvect;
  }
  
  // transform a single type to a wide union
  // - `Point` to `int | Point | null`
  // - `(int, int)` to `(int, int, cell) | builder | (int, int)`
  // - `(int, null)` to `(int, int | slice | null) | ...`: mind transition
  // have: [value, ..., value]
  // need: [NULL, ..., value*, ... value*, UTag] — transition rvect to an acceptor type, prepend by nulls, append UTag
  if (dest_union && !from_union) {
    TypePtr dest_subtype = dest_union->calculate_exact_variant_to_fit_rhs(from_type);
    tolk_assert(dest_subtype && dest_slots > dest_subtype->get_width_on_stack());
    rvect = transition_rvect_to_runtime_type(std::move(rvect), code, from_type, dest_subtype, origin);
    std::vector<var_idx_t> prepend_nulls;
    prepend_nulls.reserve(dest_slots - dest_subtype->get_width_on_stack() - 1);
    for (int i = 0; i < dest_slots - dest_subtype->get_width_on_stack() - 1; ++i) {
      FunctionPtr null_sym = lookup_function("__null");
      std::vector ith_null = code.create_tmp_var(TypeDataNullLiteral::create(), origin, "(UVar.null)");
      prepend_nulls.push_back(ith_null[0]);
      code.add_call(origin, std::move(ith_null), {}, null_sym);
    }
    rvect.insert(rvect.begin(), prepend_nulls.begin(), prepend_nulls.end());

    std::vector ir_last_utag = code.create_tmp_var(TypeDataInt::create(), origin, "(UTag)");
    code.add_int_const(origin, ir_last_utag, td::make_refint(dest_subtype->get_type_id()));
    rvect.push_back(ir_last_utag[0]);
    return rvect;
  }

  // transform a primitive nullable `T?` to a single type
  // - `int?` to `int`
  // - `[int8, [int, slice]?]?` to `[int, null]`
  // have: [value]
  // need: [value*]
  if (from_union && !dest_union && from_union->is_primitive_nullable()) {
    TypePtr from_subtype = from_union->calculate_exact_variant_to_fit_rhs(dest_type);
    tolk_assert(from_subtype && dest_slots == 1 && from_slots == 1);
    if (from_subtype != dest_type) {    // quick false for `int` and other singleton pointers
      rvect = transition_rvect_to_runtime_type(std::move(rvect), code, from_subtype, dest_type, origin);
      tolk_assert(rvect.size() == 1);
    }
    return rvect;
  }
  
  // transform a wide union to a single type
  // - `int | Point | null` to `Point`
  // - `(int, int, cell) | builder | (int, int)` to `(int, int)`
  // - `(int, int | slice | null) | ...` to `(int, null)`: mind transition
  // need: [NULL, ..., value, ... value, UTag]
  // have: [value*, ..., value*] — cut off some nulls, drop UTag, and transition the rect
  if (from_union && !dest_union) {
    TypePtr from_subtype = from_union->calculate_exact_variant_to_fit_rhs(dest_type);
    tolk_assert(from_subtype && from_slots > from_subtype->get_width_on_stack());
    rvect = std::vector(rvect.begin() + from_slots - from_subtype->get_width_on_stack() - 1, rvect.end() - 1);
    rvect = transition_rvect_to_runtime_type(std::move(rvect), code, from_subtype, dest_type, origin);
    return rvect;
  }

  // transform a tensor to a tensor
  // - `(1, null)` to `(int, slice?)`
  // - `(1, null)` to `(int, (int,int)?)`
  // every element of lhs should be transitioned to rhs
  const TypeDataTensor* from_tensor = from_type->try_as<TypeDataTensor>();
  const TypeDataTensor* dest_tensor = dest_type->try_as<TypeDataTensor>();
  if (from_tensor && dest_tensor) {
    tolk_assert(dest_tensor->size() == from_tensor->size());
    std::vector<var_idx_t> dest_rvect;
    dest_rvect.reserve(dest_slots);
    int stack_offset = 0;
    for (int i = 0; i < from_tensor->size(); ++i) {
      int ith_w = from_tensor->items[i]->get_width_on_stack();
      std::vector rvect_i(rvect.begin() + stack_offset, rvect.begin() + stack_offset + ith_w);
      std::vector result_i = transition_rvect_to_runtime_type(std::move(rvect_i), code, from_tensor->items[i], dest_tensor->items[i], origin);
      dest_rvect.insert(dest_rvect.end(), result_i.begin(), result_i.end());
      stack_offset += ith_w;
    }
    return dest_rvect;
  }

  // handle arrays and shapes (TVM tuples at runtime)
  const TypeDataArray* from_array = from_type->try_as<TypeDataArray>();
  const TypeDataArray* dest_array = dest_type->try_as<TypeDataArray>();
  const TypeDataShapedTuple* from_shaped = from_type->try_as<TypeDataShapedTuple>();
  const TypeDataShapedTuple* dest_shaped = dest_type->try_as<TypeDataShapedTuple>();
  const TypeDataMapKV* dest_mapKV = dest_type->try_as<TypeDataMapKV>();

  // transform an array to another array
  // - `array<int>` to `array<int?>`
  // - `array<int?>` to `array<unknown>`
  // - `array<Point>` to `array<Point?>`
  if (from_array && dest_array) {
    // if no inner transformations required, just re-use a ready TVM tuple
    if (can_safely_move_inside_tuple(from_array->innerT, dest_array->innerT)) {
      return rvect;
    }

    // otherwise, transform every element, e.g. `array<Point>` to `array<Point?>`: each from [x y] to [x y type-id]
    // it's done via generating a loop; pseudocode: `i = 0; repeat (a1.size()) { a2.push(a1.get(i) as T2); i++ }`
    var_idx_t ir_one = code.create_int(origin, 1, "(one)");
    std::vector ir_loop_i = {code.create_int(origin, 0, "(loop-i)")};
    std::vector ir_result_arr = code.create_tmp_var(TypeDataArray::create(TypeDataUnknown::create()), origin, "(target-array)");
    code.add_to_tuple(origin, ir_result_arr, {});
    std::vector ir_orig_size = code.create_tmp_var(TypeDataInt::create(), origin, "(orig-tuple-size)");
    code.add_call(origin, ir_orig_size, rvect, lookup_function("array<T>.size"));
    Op& repeat_op = code.add_repeat_loop(origin, ir_orig_size);
    code.push_set_cur(repeat_op.block0);
    std::vector ir_ith_elem = code.create_tmp_var(from_array->innerT, origin, "(ith-orig-elem)");
    code.add_call(origin, ir_ith_elem, {rvect[0], ir_loop_i[0]}, lookup_function("array<T>.get"));
    ir_ith_elem = transition_rvect_to_runtime_type(std::move(ir_ith_elem), code, from_array->innerT, dest_array->innerT, origin);
    std::vector<var_idx_t> ir_args_push;
    ir_args_push.reserve(1 + ir_ith_elem.size());
    ir_args_push.push_back(ir_result_arr[0]);
    ir_args_push.insert(ir_args_push.end(), ir_ith_elem.begin(), ir_ith_elem.end());
    code.add_call(origin, ir_result_arr, std::move(ir_args_push), lookup_function("array<T>.push"));
    code.add_call(origin, ir_loop_i, {ir_loop_i[0], ir_one}, lookup_function("_+_"));
    code.close_pop_cur(origin);
    return ir_result_arr;
  }

  // transform a shape to an array (they both are TVM tuples)
  // - `[int, int]` to `array<int>`
  // - `[int, slice, Point]` to `array<unknown>`
  if (from_shaped && dest_array) {
    int shape_size = from_shaped->size();
    bool safely_move = true;
    for (int i = 0; i < shape_size; ++i) {
      safely_move &= can_safely_move_inside_tuple(from_shaped->items[i], dest_array->innerT);
    }
    if (safely_move) {
      return rvect;
    }

    std::vector ir_un_tuple = code.create_tmp_var(TypeDataTensor::create(std::vector(shape_size, TypeDataUnknown::create())), origin, "(unpack-shape)");
    code.add_un_tuple(origin, ir_un_tuple, std::move(rvect));
    std::vector ir_result_arr = code.create_tmp_var(TypeDataArray::create(TypeDataUnknown::create()), origin, "(target-array)");
    code.add_to_tuple(origin, ir_result_arr, {});
    for (int i = 0; i < shape_size; ++i) {
      std::vector ir_ith_elem = { ir_un_tuple[i] };
      ir_ith_elem = transition_rvect_to_runtime_type(std::move(ir_ith_elem), code, TypeDataUnknown::create(), from_shaped->items[i], origin);
      ir_ith_elem = transition_rvect_to_runtime_type(std::move(ir_ith_elem), code, from_shaped->items[i], dest_array->innerT, origin);
      std::vector<var_idx_t> ir_args_push = std::move(ir_ith_elem);
      ir_args_push.insert(ir_args_push.begin(), ir_result_arr[0]);
      code.add_call(origin, ir_result_arr, std::move(ir_args_push), lookup_function("array<T>.push"));
    }
    return ir_result_arr;
  }

  // transform a shape to a shape
  // - `[int, int]` to `[int?, int?]`
  // - `[Point, null]` to `[Point?, Point?]`
  // - `[Point?, Point?]` to `[Point, null]` (smart cast)
  if (from_shaped && dest_shaped) {
    tolk_assert(dest_shaped->size() == from_shaped->size());
    int shape_size = dest_shaped->size();
    bool safely_move = true;
    for (int i = 0; i < shape_size; ++i) {
      safely_move &= can_safely_move_inside_tuple(from_shaped->items[i], dest_shaped->items[i]);
    }
    if (safely_move) {
      return rvect;
    }

    std::vector ir_un_tuple = code.create_tmp_var(TypeDataTensor::create(std::vector(shape_size, TypeDataUnknown::create())), origin, "(unpack-shape)");
    code.add_un_tuple(origin, ir_un_tuple, std::move(rvect));
    std::vector ir_result_arr = code.create_tmp_var(TypeDataArray::create(TypeDataUnknown::create()), origin, "(target-shape)");
    code.add_to_tuple(origin, ir_result_arr, {});
    for (int i = 0; i < shape_size; ++i) {
      std::vector ir_ith_elem = { ir_un_tuple[i] };
      ir_ith_elem = transition_rvect_to_runtime_type(std::move(ir_ith_elem), code, TypeDataUnknown::create(), from_shaped->items[i], origin);
      ir_ith_elem = transition_rvect_to_runtime_type(std::move(ir_ith_elem), code, from_shaped->items[i], dest_shaped->items[i], origin);
      std::vector<var_idx_t> ir_args_push = std::move(ir_ith_elem);
      ir_args_push.insert(ir_args_push.begin(), ir_result_arr[0]);
      code.add_call(origin, ir_result_arr, std::move(ir_args_push), lookup_function("array<T>.push"));
    }
    return ir_result_arr;
  }

  // transform a shape to a map
  // - `[]` to `map<int32, bool>` and any other empty map
  // - `[ [1, true] ]` to `map<int32, bool>` (non-empty map) not supported yet, checked earlier
  if (from_shaped && dest_mapKV) {
    tolk_assert(from_shaped->size() == 0 && rvect.size() == 1);
    std::vector ir_result_map = code.create_tmp_var(TypeDataNullLiteral::create(), origin, "(map)");
    code.add_call(origin, ir_result_map, {}, lookup_function("createEmptyMap"));
    return ir_result_map;
  }

  // handle structs and typed cells (which are also structs in stdlib)
  const TypeDataStruct* from_struct = from_type->try_as<TypeDataStruct>();
  const TypeDataStruct* dest_struct = dest_type->try_as<TypeDataStruct>();

  // transform `[]` to `lisp_list<T>` / `[1, 2, 3]` to `lisp_list<int>`
  if (from_shaped && dest_struct && dest_struct->struct_ref->is_instantiation_of_LispListT()) {
    TypePtr list_T = dest_struct->struct_ref->substitutedTs->typeT_at(0);
    std::vector ir_un_tuple = code.create_tmp_var(TypeDataTensor::create(std::vector(from_shaped->size(), TypeDataUnknown::create())), origin, "(unpack-shape)");
    code.add_un_tuple(origin, ir_un_tuple, std::move(rvect));
    std::vector ir_result_list = code.create_tmp_var(TypeDataNullLiteral::create(), origin, "(lisp-list)");
    code.add_call(origin, ir_result_list, {}, lookup_function("__null"));
    for (int i = from_shaped->size() - 1; i >= 0; --i) {
      std::vector ir_ith_elem = { ir_un_tuple[i] };
      ir_ith_elem = transition_rvect_to_runtime_type(std::move(ir_ith_elem), code, TypeDataUnknown::create(), from_shaped->items[i], origin);
      ir_ith_elem = transition_rvect_to_runtime_type(std::move(ir_ith_elem), code, from_shaped->items[i], list_T, origin);
      ir_ith_elem = transition_rvect_to_runtime_type(std::move(ir_ith_elem), code, list_T, TypeDataUnknown::create(), origin);
      std::vector<var_idx_t> ir_args_tuple = std::move(ir_ith_elem);
      ir_args_tuple.push_back(ir_result_list[0]);
      code.add_to_tuple(origin, ir_result_list, std::move(ir_args_tuple));
    }
    return ir_result_list;
  }

  // transform a callable to a callable
  // their types aren't exactly equal, but they match (containing aliases, for example)
  if (from_type->try_as<TypeDataFunCallable>() && dest_type->try_as<TypeDataFunCallable>()) {
    tolk_assert(dest_slots == 1 && from_slots == 1);
    return rvect;
  }

  // transform struct A to struct B
  // different structs are typically not assignable, but Wrapper<WrapperAlias<int>> is ok to Wrapper<Wrapper<int>>
  if (from_struct && dest_struct) {
    tolk_assert(dest_struct->can_rhs_be_assigned(from_struct) && from_slots == dest_slots);
    return rvect;
  }

  // transform `Cell<Something>` to `cell`
  if (from_struct && dest_type == TypeDataCell::create()) {
    tolk_assert(from_slots == 1 && from_struct->struct_ref->is_instantiation_of_CellT());
    return rvect;
  }
  // and vice versa, `cell as Cell<Something>`
  if (from_type == TypeDataCell::create() && dest_struct) {
    tolk_assert(dest_slots == 1 && dest_struct->struct_ref->is_instantiation_of_CellT());
    return rvect;
  }

  throw Fatal("unhandled transition_rvect_to_runtime_type() combination");
}

} // namespace tolk
