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
#include "type-system.h"
#include "lexer.h"
#include "platform-utils.h"
#include "compiler-state.h"
#include <charconv>
#include <unordered_map>

namespace tolk {

/*
 * This class stores a big hashtable [hash => TypeData]
 * Every non-trivial TypeData*::create() method at first looks here, and allocates an object only if not found.
 * That's why all allocated TypeData objects are unique, storing unique type_id.
 */
class TypeDataTypeIdCalculation {
  uint64_t cur_hash;
  int children_flags_mask = 0;

  static std::unordered_map<uint64_t, TypePtr> all_unique_occurred_types;

public:
  explicit TypeDataTypeIdCalculation(uint64_t initial_arbitrary_unique_number)
    : cur_hash(initial_arbitrary_unique_number) {}

  void feed_hash(uint64_t val) {
    cur_hash = cur_hash * 56235515617499ULL + val;
  }

  void feed_string(const std::string& s) {
    feed_hash(std::hash<std::string>{}(s));
  }

  void feed_child(TypePtr inner) {
    feed_hash(inner->type_id);
    children_flags_mask |= inner->flags;
  }

  uint64_t type_id() const {
    return cur_hash;
  }

  int children_flags() const {
    return children_flags_mask;
  }

  GNU_ATTRIBUTE_FLATTEN
  TypePtr get_existing() const {
    auto it = all_unique_occurred_types.find(cur_hash);
    return it != all_unique_occurred_types.end() ? it->second : nullptr;
  }

  GNU_ATTRIBUTE_NOINLINE
  TypePtr register_unique(TypePtr newly_created) const {
#ifdef TOLK_DEBUG
    assert(newly_created->type_id == cur_hash);
#endif
    all_unique_occurred_types[cur_hash] = newly_created;
    return newly_created;
  }
};

std::unordered_map<uint64_t, TypePtr> TypeDataTypeIdCalculation::all_unique_occurred_types;
TypePtr TypeDataInt::singleton;
TypePtr TypeDataBool::singleton;
TypePtr TypeDataCell::singleton;
TypePtr TypeDataSlice::singleton;
TypePtr TypeDataBuilder::singleton;
TypePtr TypeDataTuple::singleton;
TypePtr TypeDataContinuation::singleton;
TypePtr TypeDataNullLiteral::singleton;
TypePtr TypeDataCoins::singleton;
TypePtr TypeDataUnknown::singleton;
TypePtr TypeDataNever::singleton;
TypePtr TypeDataVoid::singleton;

void type_system_init() {
  TypeDataInt::singleton = new TypeDataInt;
  TypeDataBool::singleton = new TypeDataBool;
  TypeDataCell::singleton = new TypeDataCell;
  TypeDataSlice::singleton = new TypeDataSlice;
  TypeDataBuilder::singleton = new TypeDataBuilder;
  TypeDataTuple::singleton = new TypeDataTuple;
  TypeDataContinuation::singleton = new TypeDataContinuation;
  TypeDataNullLiteral::singleton = new TypeDataNullLiteral;
  TypeDataCoins::singleton = new TypeDataCoins;
  TypeDataUnknown::singleton = new TypeDataUnknown;
  TypeDataNever::singleton = new TypeDataNever;
  TypeDataVoid::singleton = new TypeDataVoid;
}


// --------------------------------------------
//    create()
//
// all constructors of TypeData classes are private, only TypeData*::create() is allowed
// each non-trivial create() method calculates hash (type_id)
// and creates an object only if it isn't found in a global hashtable
//

TypePtr TypeDataNullable::create(TypePtr inner) {
  TypeDataTypeIdCalculation hash(1774084920039440885ULL);
  hash.feed_child(inner);

  if (TypePtr existing = hash.get_existing()) {
    return existing;
  }
  // most types (int?, slice?, etc.), when nullable, still occupy 1 stack slot (holding TVM NULL at runtime)
  // but for example for `(int, int)` we need an extra stack slot "null flag"
  int width_on_stack = inner->can_hold_tvm_null_instead() ? 1 : inner->get_width_on_stack() + 1;
  return hash.register_unique(new TypeDataNullable(hash.type_id(), hash.children_flags(), width_on_stack, inner));
}

TypePtr TypeDataFunCallable::create(std::vector<TypePtr>&& params_types, TypePtr return_type) {
  TypeDataTypeIdCalculation hash(3184039965511020991ULL);
  for (TypePtr param : params_types) {
    hash.feed_child(param);
    hash.feed_hash(767721);
  }
  hash.feed_child(return_type);
  hash.feed_hash(767722);

  if (TypePtr existing = hash.get_existing()) {
    return existing;
  }
  return hash.register_unique(new TypeDataFunCallable(hash.type_id(), hash.children_flags(), std::move(params_types), return_type));
}

TypePtr TypeDataGenericT::create(std::string&& nameT) {
  TypeDataTypeIdCalculation hash(9145033724911680012ULL);
  hash.feed_string(nameT);

  if (TypePtr existing = hash.get_existing()) {
    return existing;
  }
  return hash.register_unique(new TypeDataGenericT(hash.type_id(), std::move(nameT)));
}

TypePtr TypeDataTensor::create(std::vector<TypePtr>&& items) {
  TypeDataTypeIdCalculation hash(3159238551239480381ULL);
  for (TypePtr item : items) {
    hash.feed_child(item);
    hash.feed_hash(819613);
  }

  if (TypePtr existing = hash.get_existing()) {
    return existing;
  }
  int width_on_stack = 0;
  for (TypePtr item : items) {
    width_on_stack += item->get_width_on_stack();
  }
  return hash.register_unique(new TypeDataTensor(hash.type_id(), hash.children_flags(), width_on_stack, std::move(items)));
}

TypePtr TypeDataTypedTuple::create(std::vector<TypePtr>&& items) {
  TypeDataTypeIdCalculation hash(9189266157349499320ULL);
  for (TypePtr item : items) {
    hash.feed_child(item);
    hash.feed_hash(735911);
  }

  if (TypePtr existing = hash.get_existing()) {
    return existing;
  }
  return hash.register_unique(new TypeDataTypedTuple(hash.type_id(), hash.children_flags(), std::move(items)));
}

TypePtr TypeDataIntN::create(bool is_unsigned, bool is_variadic, int n_bits) {
  TypeDataTypeIdCalculation hash(1678330938771108027ULL);
  hash.feed_hash(is_unsigned);
  hash.feed_hash(is_variadic);
  hash.feed_hash(n_bits);

  if (TypePtr existing = hash.get_existing()) {
    return existing;
  }
  return hash.register_unique(new TypeDataIntN(hash.type_id(), is_unsigned, is_variadic, n_bits));
}

TypePtr TypeDataBytesN::create(bool is_bits, int n_width) {
  TypeDataTypeIdCalculation hash(7810988137199333041ULL);
  hash.feed_hash(is_bits);
  hash.feed_hash(n_width);

  if (TypePtr existing = hash.get_existing()) {
    return existing;
  }
  return hash.register_unique(new TypeDataBytesN(hash.type_id(), is_bits, n_width));
}

TypePtr TypeDataUnresolved::create(std::string&& text, SrcLocation loc) {
  TypeDataTypeIdCalculation hash(3680147223540048162ULL);
  hash.feed_string(text);
  // hash.feed_hash(*reinterpret_cast<uint64_t*>(&loc));

  if (TypePtr existing = hash.get_existing()) {
    return existing;
  }
  return hash.register_unique(new TypeDataUnresolved(hash.type_id(), std::move(text), loc));
}


// --------------------------------------------
//    as_human_readable()
//
// is used only for error messages and debugging, therefore no optimizations for simplicity
// only non-trivial implementations are here; trivial are defined in .h file
//

std::string TypeDataNullable::as_human_readable() const {
  std::string nested = inner->as_human_readable();
  bool embrace = inner->try_as<TypeDataFunCallable>();
  return embrace ? "(" + nested + ")?" : nested + "?";
}

std::string TypeDataFunCallable::as_human_readable() const {
  std::string result = "(";
  for (TypePtr param : params_types) {
    if (result.size() > 1) {
      result += ", ";
    }
    result += param->as_human_readable();
  }
  result += ") -> ";
  result += return_type->as_human_readable();
  return result;
}

std::string TypeDataTensor::as_human_readable() const {
  std::string result = "(";
  for (TypePtr item : items) {
    if (result.size() > 1) {
      result += ", ";
    }
    result += item->as_human_readable();
  }
  result += ')';
  return result;
}

std::string TypeDataTypedTuple::as_human_readable() const {
  std::string result = "[";
  for (TypePtr item : items) {
    if (result.size() > 1) {
      result += ", ";
    }
    result += item->as_human_readable();
  }
  result += ']';
  return result;
}

std::string TypeDataIntN::as_human_readable() const {
  std::string s_int = is_variadic
    ? is_unsigned ? "varuint" : "varint"
    : is_unsigned ? "uint" : "int";
  return s_int + std::to_string(n_bits);
}

std::string TypeDataBytesN::as_human_readable() const {
  std::string s_bytes = is_bits ? "bits" : "bytes";
  return s_bytes + std::to_string(n_width);
}


// --------------------------------------------
//    traverse()
//
// invokes a callback for TypeData itself and all its children
// only non-trivial implementations are here; by default (no children), `callback(this)` is executed
//

void TypeDataNullable::traverse(const TraverserCallbackT& callback) const {
  callback(this);
  inner->traverse(callback);
}

void TypeDataFunCallable::traverse(const TraverserCallbackT& callback) const {
  callback(this);
  for (TypePtr param : params_types) {
    param->traverse(callback);
  }
  return_type->traverse(callback);
}

void TypeDataTensor::traverse(const TraverserCallbackT& callback) const {
  callback(this);
  for (TypePtr item : items) {
    item->traverse(callback);
  }
}

void TypeDataTypedTuple::traverse(const TraverserCallbackT& callback) const {
  callback(this);
  for (TypePtr item : items) {
    item->traverse(callback);
  }
}


// --------------------------------------------
//    replace_children_custom()
//
// returns new TypeData with children replaced by a custom callback
// used to replace generic T on generics expansion — to convert `f<T>` to `f<int>`
// only non-trivial implementations are here; by default (no children), `return callback(this)` is executed
//

TypePtr TypeDataNullable::replace_children_custom(const ReplacerCallbackT& callback) const {
  return callback(create(inner->replace_children_custom(callback)));
}

TypePtr TypeDataFunCallable::replace_children_custom(const ReplacerCallbackT& callback) const {
  std::vector<TypePtr> mapped;
  mapped.reserve(params_types.size());
  for (TypePtr param : params_types) {
    mapped.push_back(param->replace_children_custom(callback));
  }
  return callback(create(std::move(mapped), return_type->replace_children_custom(callback)));
}

TypePtr TypeDataTensor::replace_children_custom(const ReplacerCallbackT& callback) const {
  std::vector<TypePtr> mapped;
  mapped.reserve(items.size());
  for (TypePtr item : items) {
    mapped.push_back(item->replace_children_custom(callback));
  }
  return callback(create(std::move(mapped)));
}

TypePtr TypeDataTypedTuple::replace_children_custom(const ReplacerCallbackT& callback) const {
  std::vector<TypePtr> mapped;
  mapped.reserve(items.size());
  for (TypePtr item : items) {
    mapped.push_back(item->replace_children_custom(callback));
  }
  return callback(create(std::move(mapped)));
}


// --------------------------------------------
//    can_rhs_be_assigned()
//
// on `var lhs: <lhs_type> = rhs`, having inferred rhs_type, check that it can be assigned without any casts
// the same goes for passing arguments, returning values, etc. — where the "receiver" (lhs) checks "applier" (rhs)
//

bool TypeDataInt::can_rhs_be_assigned(TypePtr rhs) const {
  if (rhs == this) {
    return true;
  }
  if (rhs->try_as<TypeDataIntN>()) {
    return true;
  }
  if (rhs == TypeDataCoins::create()) {
    return true;
  }
  return rhs == TypeDataNever::create();
}

bool TypeDataBool::can_rhs_be_assigned(TypePtr rhs) const {
  if (rhs == this) {
    return true;
  }
  return rhs == TypeDataNever::create();
}

bool TypeDataCell::can_rhs_be_assigned(TypePtr rhs) const {
  if (rhs == this) {
    return true;
  }
  return rhs == TypeDataNever::create();
}

bool TypeDataSlice::can_rhs_be_assigned(TypePtr rhs) const {
  if (rhs == this) {
    return true;
  }
  return rhs == TypeDataNever::create();   // note, that bytesN is NOT automatically cast to slice without `as` operator
}

bool TypeDataBuilder::can_rhs_be_assigned(TypePtr rhs) const {
  if (rhs == this) {
    return true;
  }
  return rhs == TypeDataNever::create();
}

bool TypeDataTuple::can_rhs_be_assigned(TypePtr rhs) const {
  if (rhs == this) {
    return true;
  }
  return rhs == TypeDataNever::create();
}

bool TypeDataContinuation::can_rhs_be_assigned(TypePtr rhs) const {
  if (rhs == this) {
    return true;
  }
  return rhs == TypeDataNever::create();
}

bool TypeDataNullLiteral::can_rhs_be_assigned(TypePtr rhs) const {
  if (rhs == this) {
    return true;
  }
  return rhs == TypeDataNever::create();
}

bool TypeDataNullable::can_rhs_be_assigned(TypePtr rhs) const {
  if (rhs == this) {
    return true;
  }
  if (rhs == TypeDataNullLiteral::create()) {
    return true;
  }
  if (const TypeDataNullable* rhs_nullable = rhs->try_as<TypeDataNullable>()) {
    return inner->can_rhs_be_assigned(rhs_nullable->inner);
  }
  if (inner->can_rhs_be_assigned(rhs)) {
    return true;
  }
  return rhs == TypeDataNever::create();
}

bool TypeDataFunCallable::can_rhs_be_assigned(TypePtr rhs) const {
  if (rhs == this) {
    return true;
  }
  return rhs == TypeDataNever::create();
}

bool TypeDataGenericT::can_rhs_be_assigned(TypePtr rhs) const {
  assert(false);
  return false;
}

bool TypeDataTensor::can_rhs_be_assigned(TypePtr rhs) const {
  if (const auto* as_tensor = rhs->try_as<TypeDataTensor>(); as_tensor && as_tensor->size() == size()) {
    for (int i = 0; i < size(); ++i) {
      if (!items[i]->can_rhs_be_assigned(as_tensor->items[i])) {
        return false;
      }
    }
    return true;
  }
  return rhs == TypeDataNever::create();
}

bool TypeDataTypedTuple::can_rhs_be_assigned(TypePtr rhs) const {
  if (const auto* as_tuple = rhs->try_as<TypeDataTypedTuple>(); as_tuple && as_tuple->size() == size()) {
    for (int i = 0; i < size(); ++i) {
      if (!items[i]->can_rhs_be_assigned(as_tuple->items[i])) {
        return false;
      }
    }
    return true;
  }
  return rhs == TypeDataNever::create();
}

bool TypeDataIntN::can_rhs_be_assigned(TypePtr rhs) const {
  if (rhs == this) {
    return true;
  }
  if (rhs == TypeDataInt::create()) {
    return true;
  }
  return rhs == TypeDataNever::create();   // `int8` is NOT assignable to `int32` without `as`
}

bool TypeDataBytesN::can_rhs_be_assigned(TypePtr rhs) const {
  // `slice` is NOT assignable to bytesN without `as`
  // `bytes32` is NOT assignable to `bytes256` and even to `bits256` without `as`
  if (rhs == this) {
    return true;
  }
  return rhs == TypeDataNever::create();
}

bool TypeDataCoins::can_rhs_be_assigned(TypePtr rhs) const {
  if (rhs == this) {
    return true;
  }
  if (rhs == TypeDataInt::create()) {
    return true;
  }
  return rhs == TypeDataNever::create();
}

bool TypeDataUnknown::can_rhs_be_assigned(TypePtr rhs) const {
  return true;
}

bool TypeDataUnresolved::can_rhs_be_assigned(TypePtr rhs) const {
  assert(false);
  return false;
}

bool TypeDataNever::can_rhs_be_assigned(TypePtr rhs) const {
  return true;
}

bool TypeDataVoid::can_rhs_be_assigned(TypePtr rhs) const {
  if (rhs == this) {
    return true;
  }
  return rhs == TypeDataNever::create();
}


// --------------------------------------------
//    can_be_casted_with_as_operator()
//
// on `expr as <cast_to>`, check whether casting is applicable
// note, that it's not auto-casts `var lhs: <lhs_type> = rhs`, it's an expression `rhs as <cast_to>`
//

bool TypeDataInt::can_be_casted_with_as_operator(TypePtr cast_to) const {
  if (const auto* to_nullable = cast_to->try_as<TypeDataNullable>()) {  // `int` as `int?`
    return can_be_casted_with_as_operator(to_nullable->inner);
  }
  if (cast_to->try_as<TypeDataIntN>()) {    // `int` as `int8` / `int` as `uint2`
    return true;
  }
  if (cast_to == TypeDataCoins::create()) {   // `int` as `coins`
    return true;
  }
  return cast_to == this;
}

bool TypeDataBool::can_be_casted_with_as_operator(TypePtr cast_to) const {
  if (cast_to == TypeDataInt::create()) {
    return true;
  }
  if (const auto* to_nullable = cast_to->try_as<TypeDataNullable>()) {
    return can_be_casted_with_as_operator(to_nullable->inner);
  }
  if (const auto* to_intN = cast_to->try_as<TypeDataIntN>()) {
    return !to_intN->is_unsigned;   // `bool` as `int8` ok, `bool` as `uintN` not (true is -1)
  }
  return cast_to == this;
}

bool TypeDataCell::can_be_casted_with_as_operator(TypePtr cast_to) const {
  if (const auto* to_nullable = cast_to->try_as<TypeDataNullable>()) {
    return can_be_casted_with_as_operator(to_nullable->inner);
  }
  return cast_to == this;
}

bool TypeDataSlice::can_be_casted_with_as_operator(TypePtr cast_to) const {
  if (cast_to->try_as<TypeDataBytesN>()) {  // `slice` to `bytes32` / `slice` to `bits8`
    return true;
  }
  if (const auto* to_nullable = cast_to->try_as<TypeDataNullable>()) {
    return can_be_casted_with_as_operator(to_nullable->inner);
  }
  return cast_to == this;
}

bool TypeDataBuilder::can_be_casted_with_as_operator(TypePtr cast_to) const {
  if (const auto* to_nullable = cast_to->try_as<TypeDataNullable>()) {
    return can_be_casted_with_as_operator(to_nullable->inner);
  }
  return cast_to == this;
}

bool TypeDataTuple::can_be_casted_with_as_operator(TypePtr cast_to) const {
  if (const auto* to_nullable = cast_to->try_as<TypeDataNullable>()) {
    return can_be_casted_with_as_operator(to_nullable->inner);
  }
  return cast_to == this;
}

bool TypeDataContinuation::can_be_casted_with_as_operator(TypePtr cast_to) const {
  if (const auto* to_nullable = cast_to->try_as<TypeDataNullable>()) {
    return can_be_casted_with_as_operator(to_nullable->inner);
  }
  return cast_to == this;
}

bool TypeDataNullLiteral::can_be_casted_with_as_operator(TypePtr cast_to) const {
  return cast_to == this || cast_to->try_as<TypeDataNullable>();
}

bool TypeDataNullable::can_be_casted_with_as_operator(TypePtr cast_to) const {
  if (const auto* to_nullable = cast_to->try_as<TypeDataNullable>()) {
    return inner->can_be_casted_with_as_operator(to_nullable->inner);
  }
  return false;
}

bool TypeDataFunCallable::can_be_casted_with_as_operator(TypePtr cast_to) const {
  if (const auto* to_nullable = cast_to->try_as<TypeDataNullable>()) {
    return can_be_casted_with_as_operator(to_nullable->inner);
  }
  return this == cast_to;
}

bool TypeDataGenericT::can_be_casted_with_as_operator(TypePtr cast_to) const {
  return true;
}

bool TypeDataTensor::can_be_casted_with_as_operator(TypePtr cast_to) const {
  if (const auto* to_tensor = cast_to->try_as<TypeDataTensor>(); to_tensor && to_tensor->size() == size()) {
    for (int i = 0; i < size(); ++i) {
      if (!items[i]->can_be_casted_with_as_operator(to_tensor->items[i])) {
        return false;
      }
    }
    return true;
  }
  if (const auto* to_nullable = cast_to->try_as<TypeDataNullable>()) {
    return can_be_casted_with_as_operator(to_nullable->inner);
  }
  return false;
}

bool TypeDataTypedTuple::can_be_casted_with_as_operator(TypePtr cast_to) const {
  if (const auto* to_tuple = cast_to->try_as<TypeDataTypedTuple>(); to_tuple && to_tuple->size() == size()) {
    for (int i = 0; i < size(); ++i) {
      if (!items[i]->can_be_casted_with_as_operator(to_tuple->items[i])) {
        return false;
      }
    }
    return true;
  }
  if (const auto* to_nullable = cast_to->try_as<TypeDataNullable>()) {
    return can_be_casted_with_as_operator(to_nullable->inner);
  }
  return false;
}

bool TypeDataIntN::can_be_casted_with_as_operator(TypePtr cast_to) const {
  if (cast_to->try_as<TypeDataIntN>()) {    // `int8` as `int32`, `int256` as `uint5`, anything
    return true;
  }
  if (const auto* to_nullable = cast_to->try_as<TypeDataNullable>()) {  // `int8` as `int32?`
    return can_be_casted_with_as_operator(to_nullable->inner);
  }
  return cast_to == TypeDataInt::create() || cast_to == TypeDataCoins::create();
}

bool TypeDataBytesN::can_be_casted_with_as_operator(TypePtr cast_to) const {
  if (cast_to->try_as<TypeDataBytesN>()) {  // `bytes256` as `bytes512`, `bits1` as `bytes8`
    return true;
  }
  if (const auto* to_nullable = cast_to->try_as<TypeDataNullable>()) {  // `bytes8` as `slice?`
    return can_be_casted_with_as_operator(to_nullable->inner);
  }
  return cast_to == TypeDataSlice::create();
}

bool TypeDataCoins::can_be_casted_with_as_operator(TypePtr cast_to) const {
  if (cast_to->try_as<TypeDataIntN>()) {    // `coins` as `int8`
    return true;
  }
  if (const auto* to_nullable = cast_to->try_as<TypeDataNullable>()) {  // `coins` as `coins?` / `coins` as `int?`
    return can_be_casted_with_as_operator(to_nullable->inner);
  }
  if (cast_to == TypeDataInt::create()) {
    return true;
  }
  return cast_to == this;
}

bool TypeDataUnknown::can_be_casted_with_as_operator(TypePtr cast_to) const {
  // 'unknown' can be cast to any TVM value
  return cast_to->get_width_on_stack() == 1;
}

bool TypeDataUnresolved::can_be_casted_with_as_operator(TypePtr cast_to) const {
  return false;
}

bool TypeDataNever::can_be_casted_with_as_operator(TypePtr cast_to) const {
  return true;
}

bool TypeDataVoid::can_be_casted_with_as_operator(TypePtr cast_to) const {
  return cast_to == this;
}


// --------------------------------------------
//    can_hold_tvm_null_instead()
//
// assigning `null` to a primitive variable like `int?` / `cell?` can store TVM NULL inside the same slot
// (that's why the default implementation is just "return true", and most of types occupy 1 slot)
// but for complex variables, like `(int, int)?`, "null presence" is kept in a separate slot (UTag for union types)
// though still, tricky situations like `(int, ())?` can still "embed" TVM NULL in parallel with original value
//

bool TypeDataNullable::can_hold_tvm_null_instead() const {
  if (get_width_on_stack() != 1) {    // `(int, int)?` / `()?` can not hold null instead
    return false;                     // only `int?` / `cell?` / `StructWith1IntField?` can
  }                                   // and some tricky situations like `(int, ())?`, but not `(int?, ())?`
  return !inner->can_hold_tvm_null_instead();
}

bool TypeDataTensor::can_hold_tvm_null_instead() const {
  if (get_width_on_stack() != 1) {    // `(int, int)` / `()` can not hold null instead, since null is 1 slot
    return false;                     // only `((), int)` and similar can:
  }                                   // one item is width 1 (and not nullable), others are 0
  for (TypePtr item : items) {
    if (item->get_width_on_stack() == 1 && !item->can_hold_tvm_null_instead()) {
      return false;
    }
  }
  return true;
}

bool TypeDataNever::can_hold_tvm_null_instead() const {
  return false;
}

bool TypeDataVoid::can_hold_tvm_null_instead() const {
  return false;
}


// --------------------------------------------
//    parsing type from tokens
//
// here we implement parsing types (mostly after colon) to TypeData
// example: `var v: int` is TypeDataInt
// example: `var v: (builder?, [cell])` is TypeDataTensor(TypeDataNullable(TypeDataBuilder), TypeDataTypedTuple(TypeDataCell))
// example: `fun f(): ()` is TypeDataTensor() (an empty one)
//
// note, that unrecognized type names (MyEnum, MyStruct, T) are parsed as TypeDataUnresolved,
// and later, when all files are parsed and all symbols registered, such identifiers are resolved
// example: `fun f<T>(v: T)` at first v is TypeDataUnresolved("T"), later becomes TypeDataGenericT
// see finalize_type_data()
//
// note, that `self` does not name a type, it can appear only as a return value of a function (parsed specially)
// when `self` appears as a type, it's parsed as TypeDataUnresolved, and later an error is emitted
//

static TypePtr parse_type_expression(Lexer& lex);

std::vector<TypePtr> parse_nested_type_list(Lexer& lex, TokenType tok_op, const char* s_op, TokenType tok_cl, const char* s_cl) {
  lex.expect(tok_op, s_op);
  std::vector<TypePtr> sub_types;
  while (true) {
    if (lex.tok() == tok_cl) {  // empty lists allowed
      lex.next();
      break;
    }

    sub_types.emplace_back(parse_type_expression(lex));
    if (lex.tok() == tok_comma) {
      lex.next();
    } else if (lex.tok() != tok_cl) {
      lex.unexpected(s_cl);
    }
  }
  return sub_types;
}

std::vector<TypePtr> parse_nested_type_list_in_parenthesis(Lexer& lex) {
  return parse_nested_type_list(lex, tok_oppar, "`(`", tok_clpar, "`)` or `,`");
}

static TypePtr parse_intN(std::string_view strN, bool is_unsigned) {
  int n;
  auto result = std::from_chars(strN.data() + 3 + static_cast<int>(is_unsigned), strN.data() + strN.size(), n);
  bool parsed = result.ec == std::errc() && result.ptr == strN.data() + strN.size();
  if (!parsed || n <= 0 || n > 256 + static_cast<int>(is_unsigned)) {
    return nullptr;   // `int1000`, maybe it's user-defined alias, let it be unresolved
  }
  return TypeDataIntN::create(is_unsigned, false, n);
}

static TypePtr parse_bytesN(std::string_view strN, bool is_bits) {
  int n;
  auto result = std::from_chars(strN.data() + 5  - static_cast<int>(is_bits), strN.data() + strN.size(), n);
  bool parsed = result.ec == std::errc() && result.ptr == strN.data() + strN.size();
  if (!parsed || n <= 0 || n > 1024) {
    return nullptr;   // `bytes9999`, maybe it's user-defined alias, let it be unresolved
  }
  return TypeDataBytesN::create(is_bits, n);
}

static TypePtr parse_simple_type(Lexer& lex) {
  switch (lex.tok()) {
    case tok_self:
    case tok_identifier: {
      SrcLocation loc = lex.cur_location();
      std::string_view str = lex.cur_str();
      lex.next();
      switch (str.size()) {
        case 3:
          if (str == "int") return TypeDataInt::create();
          break;
        case 4:
          if (str == "cell") return TypeDataCell::create();
          if (str == "void") return TypeDataVoid::create();
          if (str == "bool") return TypeDataBool::create();
          break;
        case 5:
          if (str == "slice") return TypeDataSlice::create();
          if (str == "tuple") return TypeDataTuple::create();
          if (str == "coins") return TypeDataCoins::create();
          if (str == "never") return TypeDataNever::create();
          break;
        case 7:
          if (str == "builder") return TypeDataBuilder::create();
          break;
        case 8:
          if (str == "varint16") return TypeDataIntN::create(false, true, 16);
          if (str == "varint32") return TypeDataIntN::create(false, true, 32);
          break;
        case 12:
          if (str == "continuation") return TypeDataContinuation::create();
          break;
        default:
          break;
      }
      if (str.starts_with("int")) {
        if (TypePtr intN = parse_intN(str, false)) {
          return intN;
        }
      }
      if (str.size() > 4 && str.starts_with("uint")) {
        if (TypePtr uintN = parse_intN(str, true)) {
          return uintN;
        }
      }
      if (str.size() > 4 && str.starts_with("bits")) {
        if (TypePtr bitsN = parse_bytesN(str, true)) {
          return bitsN;
        }
      }
      if (str.size() > 5 && str.starts_with("bytes")) {
        if (TypePtr bytesN = parse_bytesN(str, false)) {
          return bytesN;
        }
      }
      return TypeDataUnresolved::create(std::string(str), loc);
    }
    case tok_null:
      lex.next();
      return TypeDataNullLiteral::create();
    case tok_oppar: {
      std::vector<TypePtr> items = parse_nested_type_list_in_parenthesis(lex);
      if (items.size() == 1) {
        return items.front();
      }
      return TypeDataTensor::create(std::move(items));
    }
    case tok_opbracket: {
      std::vector<TypePtr> items = parse_nested_type_list(lex, tok_opbracket, "`[`", tok_clbracket, "`]` or `,`");
      return TypeDataTypedTuple::create(std::move(items));
    }
    default:
      lex.unexpected("<type>");
  }
}

static TypePtr parse_type_nullable(Lexer& lex) {
  TypePtr result = parse_simple_type(lex);

  if (lex.tok() == tok_question) {
    lex.next();
    result = TypeDataNullable::create(result);
  }

  return result;
}

static TypePtr parse_type_expression(Lexer& lex) {
  TypePtr result = parse_type_nullable(lex);

  if (lex.tok() == tok_arrow) {   // `int -> int`, `(cell, slice) -> void`
    lex.next();
    TypePtr return_type = parse_type_expression(lex);
    std::vector<TypePtr> params_types = {result};
    if (const auto* as_tensor = result->try_as<TypeDataTensor>()) {
      params_types = as_tensor->items;
    }
    return TypeDataFunCallable::create(std::move(params_types), return_type);
  }

  if (lex.tok() != tok_bitwise_or) {
    return result;
  }

  lex.error("union types are not supported yet");
}

TypePtr parse_type_from_tokens(Lexer& lex) {
  return parse_type_expression(lex);
}

// for internal usage only
TypePtr parse_type_from_string(std::string_view text) {
  Lexer lex(text);
  return parse_type_expression(lex);
}

std::ostream& operator<<(std::ostream& os, TypePtr type_data) {
  return os << (type_data ? type_data->as_human_readable() : "(nullptr-type)");
}

} // namespace tolk
