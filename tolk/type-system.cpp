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
TypePtr TypeDataUnknown::singleton;
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
  TypeDataUnknown::singleton = new TypeDataUnknown;
  TypeDataVoid::singleton = new TypeDataVoid;
}


// --------------------------------------------
//    create()
//
// all constructors of TypeData classes are private, only TypeData*::create() is allowed
// each non-trivial create() method calculates hash (type_id)
// and creates an object only if it isn't found in a global hashtable
//

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
  return hash.register_unique(new TypeDataTensor(hash.type_id(), hash.children_flags(), std::move(items)));
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


// --------------------------------------------
//    traverse()
//
// invokes a callback for TypeData itself and all its children
// only non-trivial implementations are here; by default (no children), `callback(this)` is executed
//

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
//    calc_width_on_stack()
//
// returns the number of stack slots occupied by a variable of this type
// only non-trivial implementations are here; by default (most types) occupy 1 stack slot
//

int TypeDataGenericT::calc_width_on_stack() const {
  // this function is invoked only in functions with generics already instantiated
  assert(false);
  return -999999;
}

int TypeDataTensor::calc_width_on_stack() const {
  int sum = 0;
  for (TypePtr item : items) {
    sum += item->calc_width_on_stack();
  }
  return sum;
}

int TypeDataUnresolved::calc_width_on_stack() const {
  // since early pipeline stages, no unresolved types left
  assert(false);
  return -999999;
}

int TypeDataVoid::calc_width_on_stack() const {
  return 0;
}


// --------------------------------------------
//    can_rhs_be_assigned()
//
// on `var lhs: <lhs_type> = rhs`, having inferred rhs_type, check that it can be assigned without any casts
// the same goes for passing arguments, returning values, etc. — where the "receiver" (lhs) checks "applier" (rhs)
// for now, `null` can be assigned to any TVM primitive, be later we'll have T? types and null safety
//

bool TypeDataInt::can_rhs_be_assigned(TypePtr rhs) const {
  if (rhs == this) {
    return true;
  }
  if (rhs == TypeDataNullLiteral::create()) {
    return true;
  }
  return false;
}

bool TypeDataBool::can_rhs_be_assigned(TypePtr rhs) const {
  if (rhs == this) {
    return true;
  }
  if (rhs == TypeDataNullLiteral::create()) {
    return true;
  }
  return false;
}

bool TypeDataCell::can_rhs_be_assigned(TypePtr rhs) const {
  if (rhs == this) {
    return true;
  }
  if (rhs == TypeDataNullLiteral::create()) {
    return true;
  }
  return false;
}

bool TypeDataSlice::can_rhs_be_assigned(TypePtr rhs) const {
  if (rhs == this) {
    return true;
  }
  if (rhs == TypeDataNullLiteral::create()) {
    return true;
  }
  return false;
}

bool TypeDataBuilder::can_rhs_be_assigned(TypePtr rhs) const {
  if (rhs == this) {
    return true;
  }
  if (rhs == TypeDataNullLiteral::create()) {
    return true;
  }
  return false;
}

bool TypeDataTuple::can_rhs_be_assigned(TypePtr rhs) const {
  if (rhs == this) {
    return true;
  }
  if (rhs == TypeDataNullLiteral::create()) {
    return true;
  }
  return false;
}

bool TypeDataContinuation::can_rhs_be_assigned(TypePtr rhs) const {
  if (rhs == this) {
    return true;
  }
  if (rhs == TypeDataNullLiteral::create()) {
    return true;
  }
  return false;
}

bool TypeDataNullLiteral::can_rhs_be_assigned(TypePtr rhs) const {
  return rhs == this;
}

bool TypeDataFunCallable::can_rhs_be_assigned(TypePtr rhs) const {
  return rhs == this;
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
  // note, that tensors can not accept null
  return false;
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
  if (rhs == TypeDataNullLiteral::create()) {
    return true;
  }
  return false;
}

bool TypeDataUnknown::can_rhs_be_assigned(TypePtr rhs) const {
  return true;
}

bool TypeDataUnresolved::can_rhs_be_assigned(TypePtr rhs) const {
  assert(false);
  return false;
}

bool TypeDataVoid::can_rhs_be_assigned(TypePtr rhs) const {
  return rhs == this;
}


// --------------------------------------------
//    can_be_casted_with_as_operator()
//
// on `expr as <cast_to>`, check whether casting is applicable
// note, that it's not auto-casts `var lhs: <lhs_type> = rhs`, it's an expression `rhs as <cast_to>`
//

bool TypeDataInt::can_be_casted_with_as_operator(TypePtr cast_to) const {
  return cast_to == this;
}

bool TypeDataBool::can_be_casted_with_as_operator(TypePtr cast_to) const {
  return cast_to == this || cast_to == TypeDataInt::create();
}

bool TypeDataCell::can_be_casted_with_as_operator(TypePtr cast_to) const {
  return cast_to == this;
}

bool TypeDataSlice::can_be_casted_with_as_operator(TypePtr cast_to) const {
  return cast_to == this;
}

bool TypeDataBuilder::can_be_casted_with_as_operator(TypePtr cast_to) const {
  return cast_to == this;
}

bool TypeDataTuple::can_be_casted_with_as_operator(TypePtr cast_to) const {
  return cast_to == this;
}

bool TypeDataContinuation::can_be_casted_with_as_operator(TypePtr cast_to) const {
  return cast_to == this;
}

bool TypeDataNullLiteral::can_be_casted_with_as_operator(TypePtr cast_to) const {
  return cast_to == this
      || cast_to == TypeDataInt::create() || cast_to == TypeDataBool::create() || cast_to == TypeDataCell::create() || cast_to == TypeDataSlice::create()
      || cast_to == TypeDataBuilder::create() || cast_to == TypeDataContinuation::create() || cast_to == TypeDataTuple::create()
      || cast_to->try_as<TypeDataTypedTuple>();
}

bool TypeDataFunCallable::can_be_casted_with_as_operator(TypePtr cast_to) const {
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
  return false;
}

bool TypeDataUnknown::can_be_casted_with_as_operator(TypePtr cast_to) const {
  // 'unknown' can be cast to any type
  // (though it's not valid for exception arguments when casting them to non-1 stack width,
  //  but to ensure it, we need a special type "unknown TVM primitive", which is overwhelming I think)
  return true;
}

bool TypeDataUnresolved::can_be_casted_with_as_operator(TypePtr cast_to) const {
  return false;
}

bool TypeDataVoid::can_be_casted_with_as_operator(TypePtr cast_to) const {
  return cast_to == this;
}


// --------------------------------------------
//    extract_components()
//
// used in code generation (transforming Ops to other Ops)
// to be removed in the future
//

void TypeDataGenericT::extract_components(std::vector<TypePtr>& comp_types) const {
  assert(false);
}

void TypeDataTensor::extract_components(std::vector<TypePtr>& comp_types) const {
  for (TypePtr item : items) {
    item->extract_components(comp_types);
  }
}

void TypeDataUnresolved::extract_components(std::vector<TypePtr>& comp_types) const {
  assert(false);
}

void TypeDataVoid::extract_components(std::vector<TypePtr>& comp_types) const {
}


// --------------------------------------------
//    parsing type from tokens
//
// here we implement parsing types (mostly after colon) to TypeData
// example: `var v: int` is TypeDataInt
// example: `var v: (builder, [cell])` is TypeDataTensor(TypeDataBuilder, TypeDataTypedTuple(TypeDataCell))
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

static TypePtr parse_simple_type(Lexer& lex) {
  switch (lex.tok()) {
    case tok_int:
      lex.next();
      return TypeDataInt::create();
    case tok_bool:
      lex.next();
      return TypeDataBool::create();
    case tok_cell:
      lex.next();
      return TypeDataCell::create();
    case tok_builder:
      lex.next();
      return TypeDataBuilder::create();
    case tok_slice:
      lex.next();
      return TypeDataSlice::create();
    case tok_tuple:
      lex.next();
      return TypeDataTuple::create();
    case tok_continuation:
      lex.next();
      return TypeDataContinuation::create();
    case tok_null:
      lex.next();
      return TypeDataNullLiteral::create();
    case tok_void:
      lex.next();
      return TypeDataVoid::create();
    case tok_self:
    case tok_identifier: {
      SrcLocation loc = lex.cur_location();
      std::string text = static_cast<std::string>(lex.cur_str());
      lex.next();
      return TypeDataUnresolved::create(std::move(text), loc);
    }
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
    case tok_fun: {
      lex.next();
      std::vector<TypePtr> params_types = parse_nested_type_list_in_parenthesis(lex);
      lex.expect(tok_arrow, "`->`");
    }
    default:
      lex.unexpected("<type>");
  }
}

static TypePtr parse_type_nullable(Lexer& lex) {
  TypePtr result = parse_simple_type(lex);

  if (lex.tok() == tok_question) {
    lex.error("nullable types are not supported yet");
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

std::ostream& operator<<(std::ostream& os, TypePtr type_data) {
  return os << (type_data ? type_data->as_human_readable() : "(nullptr-type)");
}

} // namespace tolk
