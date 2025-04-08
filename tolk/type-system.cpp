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
 * This class stores a big hashtable [hash => TypePtr]
 * Every non-trivial TypeData*::create() method at first looks here, and allocates an object only if not found.
 * That's why all allocated TypeData objects are unique, and can be compared as pointers.
 * But compare pointers carefully due to type aliases.
 */
class TypeDataHasherForUnique {
  uint64_t cur_hash;
  int children_flags_mask = 0;

  static std::unordered_map<uint64_t, TypePtr> all_unique_occurred_types;

public:
  explicit TypeDataHasherForUnique(uint64_t initial_arbitrary_unique_number)
    : cur_hash(initial_arbitrary_unique_number) {}

  void feed_hash(uint64_t val) {
    cur_hash = cur_hash * 56235515617499ULL + val;
  }

  void feed_string(const std::string& s) {
    feed_hash(std::hash<std::string>{}(s));
  }

  void feed_child(TypePtr inner) {
    feed_hash(reinterpret_cast<uint64_t>(inner));
    children_flags_mask |= inner->flags;
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
    assert(all_unique_occurred_types.find(cur_hash) == all_unique_occurred_types.end());
#endif
    all_unique_occurred_types[cur_hash] = newly_created;
    return newly_created;
  }
};

/*
 * This class stores a hashtable [TypePtr => type_id]
 * We need type_id to support union types, that are stored as tagged unions on a stack.
 * Every type that can be contained inside a union, has type_id.
 * Some type_id are predefined (1 = int, etc.), but all user-defined types are assigned type_id.
 */
class TypeIdCalculation {
  static int last_type_id;
  static std::unordered_map<TypePtr, int> map_ptr_to_type_id;

public:
  static int assign_type_id(TypePtr self) {
    if (self->has_type_alias_inside()) {        // type_id is calculated without aliases
      self = unwrap_type_alias_deeply(self);    // `(int,int)` equals `(IntAlias,IntAlias)`.
    }
    if (auto it = map_ptr_to_type_id.find(self); it != map_ptr_to_type_id.end()) {
      return it->second;
    }

    int type_id = ++last_type_id;
    map_ptr_to_type_id[self] = type_id;
    return type_id;
  }

  static TypePtr unwrap_type_alias_deeply(TypePtr type) {
    return type->replace_children_custom([](TypePtr child) {
      if (const TypeDataAlias* as_alias = child->try_as<TypeDataAlias>()) {
        return as_alias->underlying_type->unwrap_alias();
      }
      return child;
    });
  }
};


int TypeIdCalculation::last_type_id = 128;       // below 128 reserved for built-in types
std::unordered_map<uint64_t, TypePtr> TypeDataHasherForUnique::all_unique_occurred_types;
std::unordered_map<TypePtr, int> TypeIdCalculation::map_ptr_to_type_id;

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


bool TypeData::equal_to_slow_path(TypePtr lhs, TypePtr rhs) {
  if (lhs->has_type_alias_inside()) {
    lhs = TypeIdCalculation::unwrap_type_alias_deeply(lhs);
  }
  if (rhs->has_type_alias_inside()) {
    rhs = TypeIdCalculation::unwrap_type_alias_deeply(rhs);
  }
  if (lhs == rhs) {
    return true;
  }

  if (const TypeDataUnion* lhs_union = lhs->try_as<TypeDataUnion>()) {
    if (const TypeDataUnion* rhs_union = rhs->try_as<TypeDataUnion>()) {
      return lhs_union->variants.size() == rhs_union->variants.size() && lhs_union->has_all_variants_of(rhs_union);
    }
  }
  return false;
}

TypePtr TypeData::unwrap_alias_slow_path(TypePtr lhs) {
  TypePtr unwrapped = lhs;
  while (const TypeDataAlias* as_alias = unwrapped->try_as<TypeDataAlias>()) {
    unwrapped = as_alias->underlying_type;
  }
  return unwrapped;
}


// --------------------------------------------
//    create()
//
// all constructors of TypeData classes are private, only TypeData*::create() is allowed
// each non-trivial create() method calculates hash
// and creates an object only if it isn't found in a global hashtable
//

TypePtr TypeDataAlias::create(AliasDefPtr alias_ref) {
  TypeDataHasherForUnique hash(5694590762732189561ULL);
  hash.feed_string(alias_ref->name);
  hash.feed_child(alias_ref->underlying_type);

  if (TypePtr existing = hash.get_existing()) {
    return existing;
  }

  TypePtr underlying_type = alias_ref->underlying_type;
  if (underlying_type == TypeDataNullLiteral::create() || underlying_type == TypeDataNever::create() || underlying_type == TypeDataVoid::create()) {
    return underlying_type;   // aliasing these types is strange, don't store an alias
  }

  return hash.register_unique(new TypeDataAlias(hash.children_flags(), alias_ref, underlying_type));
}

TypePtr TypeDataFunCallable::create(std::vector<TypePtr>&& params_types, TypePtr return_type) {
  TypeDataHasherForUnique hash(3184039965511020991ULL);
  for (TypePtr param : params_types) {
    hash.feed_child(param);
    hash.feed_hash(767721);
  }
  hash.feed_child(return_type);
  hash.feed_hash(767722);

  if (TypePtr existing = hash.get_existing()) {
    return existing;
  }
  return hash.register_unique(new TypeDataFunCallable(hash.children_flags(), std::move(params_types), return_type));
}

TypePtr TypeDataGenericT::create(std::string&& nameT) {
  TypeDataHasherForUnique hash(9145033724911680012ULL);
  hash.feed_string(nameT);

  if (TypePtr existing = hash.get_existing()) {
    return existing;
  }
  return hash.register_unique(new TypeDataGenericT(std::move(nameT)));
}

TypePtr TypeDataTensor::create(std::vector<TypePtr>&& items) {
  TypeDataHasherForUnique hash(3159238551239480381ULL);
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
  return hash.register_unique(new TypeDataTensor(hash.children_flags(), width_on_stack, std::move(items)));
}

TypePtr TypeDataTypedTuple::create(std::vector<TypePtr>&& items) {
  TypeDataHasherForUnique hash(9189266157349499320ULL);
  for (TypePtr item : items) {
    hash.feed_child(item);
    hash.feed_hash(735911);
  }

  if (TypePtr existing = hash.get_existing()) {
    return existing;
  }
  return hash.register_unique(new TypeDataTypedTuple(hash.children_flags(), std::move(items)));
}

TypePtr TypeDataIntN::create(bool is_unsigned, bool is_variadic, int n_bits) {
  TypeDataHasherForUnique hash(1678330938771108027ULL);
  hash.feed_hash(is_unsigned);
  hash.feed_hash(is_variadic);
  hash.feed_hash(n_bits);

  if (TypePtr existing = hash.get_existing()) {
    return existing;
  }
  return hash.register_unique(new TypeDataIntN(is_unsigned, is_variadic, n_bits));
}

TypePtr TypeDataBytesN::create(bool is_bits, int n_width) {
  TypeDataHasherForUnique hash(7810988137199333041ULL);
  hash.feed_hash(is_bits);
  hash.feed_hash(n_width);

  if (TypePtr existing = hash.get_existing()) {
    return existing;
  }
  return hash.register_unique(new TypeDataBytesN(is_bits, n_width));
}

TypePtr TypeDataUnion::create(std::vector<TypePtr>&& variants) {
  TypeDataHasherForUnique hash(8719233194368471403ULL);
  for (TypePtr variant : variants) {
    hash.feed_child(variant);
    hash.feed_hash(817663);
  }

  if (TypePtr existing = hash.get_existing()) {
    return existing;
  }

  // at the moment of parsing, union type can contain unresolved symbols
  // in this case, don't try to flatten: we have no info
  // after symbols resolving, a new union type (with resolved variants) will be created
  bool not_ready_yet = false;
  for (TypePtr variant : variants) {
    not_ready_yet |= variant->has_unresolved_inside() || variant->has_genericT_inside();
  }
  if (not_ready_yet) {
    TypePtr or_null = nullptr;
    if (variants.size() == 2) {
      if (variants[0] == TypeDataNullLiteral::create() || variants[1] == TypeDataNullLiteral::create()) {
        or_null = variants[variants[0] == TypeDataNullLiteral::create()];
      }
    }
    return hash.register_unique(new TypeDataUnion(hash.children_flags(), -999999, or_null, std::move(variants)));
  }

  // flatten variants and remove duplicates
  // note, that `int | slice` and `int | int | slice` are different TypePtr, but actually the same variants
  std::vector<TypePtr> flat_variants;
  flat_variants.reserve(variants.size());
  for (TypePtr variant : variants) {
    if (const TypeDataUnion* nested_union = variant->unwrap_alias()->try_as<TypeDataUnion>()) {
      for (TypePtr nested_variant : nested_union->variants) {
        append_union_type_variant(nested_variant, flat_variants);
      }
    } else {
      append_union_type_variant(variant, flat_variants);
    }
  }
  // detect, whether it's `T?` or `T1 | T2 | ...`
  TypePtr or_null = nullptr;
  if (flat_variants.size() == 2) {
    if (flat_variants[0] == TypeDataNullLiteral::create() || flat_variants[1] == TypeDataNullLiteral::create()) {
      or_null = flat_variants[flat_variants[0] == TypeDataNullLiteral::create()];
    }
  }

  int width_on_stack;
  if (or_null && or_null->can_hold_tvm_null_instead()) {
    width_on_stack = 1;
  } else {
    // `T1 | T2 | ...` occupy max(W[i]) + 1 slot for UTag (stores type_id or 0 for null)
    int max_child_width = 0;
    for (TypePtr i : flat_variants) {
      if (i != TypeDataNullLiteral::create()) {   // `Empty | () | null` totally should be 1 (0 + 1 for UTag)
        max_child_width = std::max(max_child_width, i->get_width_on_stack());
      }
    }
    width_on_stack = max_child_width + 1;
  }

  if (flat_variants.size() == 1) {    // `int | int`
    return flat_variants[0];
  }
  return hash.register_unique(new TypeDataUnion(hash.children_flags(), width_on_stack, or_null, std::move(flat_variants)));
}

TypePtr TypeDataUnion::create_nullable(TypePtr nullable) {
  // calculate exactly the same hash as for `T | null` to create std::vector only if type seen the first time
  TypeDataHasherForUnique hash(8719233194368471403ULL);
  hash.feed_child(nullable);
  hash.feed_hash(817663);
  hash.feed_child(TypeDataNullLiteral::create());
  hash.feed_hash(817663);

  if (TypePtr existing = hash.get_existing()) {
    return existing;
  }
  return create({nullable, TypeDataNullLiteral::create()});
}

TypePtr TypeDataUnresolved::create(std::string&& text, SrcLocation loc) {
  TypeDataHasherForUnique hash(3680147223540048162ULL);
  hash.feed_string(text);
  // hash.feed_hash(*reinterpret_cast<uint64_t*>(&loc));

  if (TypePtr existing = hash.get_existing()) {
    return existing;
  }
  return hash.register_unique(new TypeDataUnresolved(std::move(text), loc));
}


// --------------------------------------------
//    get_type_id()
//
// in order to support union types, every type that can be stored inside a union has a unique type_id
// some are predefined (1 = int, etc. in .h file), the others are here
//

int TypeDataAlias::get_type_id() const {
  return underlying_type->get_type_id();
}

int TypeDataFunCallable::get_type_id() const {
  return TypeIdCalculation::assign_type_id(this);
}

int TypeDataGenericT::get_type_id() const {
  return TypeIdCalculation::assign_type_id(this);
}

int TypeDataTensor::get_type_id() const {
  assert(!has_genericT_inside());
  return TypeIdCalculation::assign_type_id(this);
}

int TypeDataTypedTuple::get_type_id() const {
  assert(!has_genericT_inside());
  return TypeIdCalculation::assign_type_id(this);
}

int TypeDataIntN::get_type_id() const {
  switch (n_bits) {
    case 8:   return 42 + is_unsigned;    // for common intN, use predefined small numbers
    case 16:  return 44 + is_unsigned;
    case 32:  return 46 + is_unsigned;
    case 64:  return 48 + is_unsigned;
    case 128: return 50 + is_unsigned;
    case 256: return 52 + is_unsigned;
    default:  return TypeIdCalculation::assign_type_id(this);
  }
}

int TypeDataBytesN::get_type_id() const {
  return TypeIdCalculation::assign_type_id(this);
}

int TypeDataUnion::get_type_id() const {
  assert(false);    // a union can not be inside a union
  throw Fatal("unexpected get_type_id() call");
}

int TypeDataUnknown::get_type_id() const {
  assert(false);    // unknown can not be inside a union
  throw Fatal("unexpected get_type_id() call");
}

int TypeDataUnresolved::get_type_id() const {
  assert(false);    // unresolved can be inside a union at parsing, but is resolved is advance
  throw Fatal("unexpected get_type_id() call");
}


// --------------------------------------------
//    as_human_readable()
//
// is used only for error messages and debugging, therefore no optimizations for simplicity
// only non-trivial implementations are here; trivial are defined in .h file
//

std::string TypeDataAlias::as_human_readable() const {
  return alias_ref->name;
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

std::string TypeDataUnion::as_human_readable() const {
  // stringify `T?`, not `T | null`
  if (or_null) {
    bool embrace = or_null->try_as<TypeDataFunCallable>();
    return embrace ? "(" + or_null->as_human_readable() + ")?" : or_null->as_human_readable() + "?";
  }

  std::string result;
  for (TypePtr variant : variants) {
    if (!result.empty()) {
      result += " | ";
    }
    bool embrace = variant->try_as<TypeDataFunCallable>();
    if (embrace) {
      result += "(";
    }
    result += variant->as_human_readable();
    if (embrace) {
      result += ")";
    }
  }
  return result;
}


// --------------------------------------------
//    traverse()
//
// invokes a callback for TypeData itself and all its children
// only non-trivial implementations are here; by default (no children), `callback(this)` is executed
//

void TypeDataAlias::traverse(const TraverserCallbackT& callback) const {
  callback(this);
  underlying_type->traverse(callback);
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

void TypeDataUnion::traverse(const TraverserCallbackT& callback) const {
  callback(this);
  for (TypePtr variant : variants) {
    variant->traverse(callback);
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

TypePtr TypeDataUnion::replace_children_custom(const ReplacerCallbackT& callback) const {
  std::vector<TypePtr> mapped;
  mapped.reserve(variants.size());
  for (TypePtr variant : variants) {
    mapped.push_back(variant->replace_children_custom(callback));
  }
  return callback(create(std::move(mapped)));
}


// --------------------------------------------
//    can_rhs_be_assigned()
//
// on `var lhs: <lhs_type> = rhs`, having inferred rhs_type, check that it can be assigned without any casts
// the same goes for passing arguments, returning values, etc. — where the "receiver" (lhs) checks "applier" (rhs)
// note, that `int8 | int16` is not assignable to `int` (even though both are assignable),
// because the only way to work with union types is to use `match`/`is` operators
//

bool TypeDataAlias::can_rhs_be_assigned(TypePtr rhs) const {
  if (rhs == this) {
    return true;
  }
  return underlying_type->can_rhs_be_assigned(rhs);
}

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
  if (const TypeDataAlias* rhs_alias = rhs->try_as<TypeDataAlias>()) {
    return can_rhs_be_assigned(rhs_alias->underlying_type);
  }
  return rhs == TypeDataNever::create();
}

bool TypeDataBool::can_rhs_be_assigned(TypePtr rhs) const {
  if (rhs == this) {
    return true;
  }
  if (const TypeDataAlias* rhs_alias = rhs->try_as<TypeDataAlias>()) {
    return can_rhs_be_assigned(rhs_alias->underlying_type);
  }
  return rhs == TypeDataNever::create();
}

bool TypeDataCell::can_rhs_be_assigned(TypePtr rhs) const {
  if (rhs == this) {
    return true;
  }
  if (const TypeDataAlias* rhs_alias = rhs->try_as<TypeDataAlias>()) {
    return can_rhs_be_assigned(rhs_alias->underlying_type);
  }
  return rhs == TypeDataNever::create();
}

bool TypeDataSlice::can_rhs_be_assigned(TypePtr rhs) const {
  if (rhs == this) {
    return true;
  }
  if (const TypeDataAlias* rhs_alias = rhs->try_as<TypeDataAlias>()) {
    return can_rhs_be_assigned(rhs_alias->underlying_type);
  }
  return rhs == TypeDataNever::create();   // note, that bytesN is NOT automatically cast to slice without `as` operator
}

bool TypeDataBuilder::can_rhs_be_assigned(TypePtr rhs) const {
  if (rhs == this) {
    return true;
  }
  if (const TypeDataAlias* rhs_alias = rhs->try_as<TypeDataAlias>()) {
    return can_rhs_be_assigned(rhs_alias->underlying_type);
  }
  return rhs == TypeDataNever::create();
}

bool TypeDataTuple::can_rhs_be_assigned(TypePtr rhs) const {
  if (rhs == this) {
    return true;
  }
  if (const TypeDataAlias* rhs_alias = rhs->try_as<TypeDataAlias>()) {
    return can_rhs_be_assigned(rhs_alias->underlying_type);
  }
  return rhs == TypeDataNever::create();
}

bool TypeDataContinuation::can_rhs_be_assigned(TypePtr rhs) const {
  if (rhs == this) {
    return true;
  }
  if (const TypeDataAlias* rhs_alias = rhs->try_as<TypeDataAlias>()) {
    return can_rhs_be_assigned(rhs_alias->underlying_type);
  }
  return rhs == TypeDataNever::create();
}

bool TypeDataNullLiteral::can_rhs_be_assigned(TypePtr rhs) const {
  if (rhs == this) {
    return true;
  }
  if (const TypeDataAlias* rhs_alias = rhs->try_as<TypeDataAlias>()) {
    return can_rhs_be_assigned(rhs_alias->underlying_type);
  }
  return rhs == TypeDataNever::create();
}

bool TypeDataFunCallable::can_rhs_be_assigned(TypePtr rhs) const {
  if (rhs == this) {
    return true;
  }
  if (const TypeDataFunCallable* rhs_callable = rhs->try_as<TypeDataFunCallable>()) {
    if (rhs_callable->params_size() != params_size()) {
      return false;
    }
    for (int i = 0; i < params_size(); ++i) {
      if (!rhs_callable->params_types[i]->can_rhs_be_assigned(params_types[i])) {
        return false;
      }
      if (!params_types[i]->can_rhs_be_assigned(rhs_callable->params_types[i])) {
        return false;
      }
    }
    return return_type->can_rhs_be_assigned(rhs_callable->return_type) &&
           rhs_callable->return_type->can_rhs_be_assigned(return_type);
  }
  if (const TypeDataAlias* rhs_alias = rhs->try_as<TypeDataAlias>()) {
    return can_rhs_be_assigned(rhs_alias->underlying_type);
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
  if (const TypeDataAlias* rhs_alias = rhs->try_as<TypeDataAlias>()) {
    return can_rhs_be_assigned(rhs_alias->underlying_type);
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
  if (const TypeDataAlias* rhs_alias = rhs->try_as<TypeDataAlias>()) {
    return can_rhs_be_assigned(rhs_alias->underlying_type);
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
  if (const TypeDataAlias* rhs_alias = rhs->try_as<TypeDataAlias>()) {
    return can_rhs_be_assigned(rhs_alias->underlying_type);
  }
  return rhs == TypeDataNever::create();   // `int8` is NOT assignable to `int32` without `as`
}

bool TypeDataBytesN::can_rhs_be_assigned(TypePtr rhs) const {
  // `slice` is NOT assignable to bytesN without `as`
  // `bytes32` is NOT assignable to `bytes256` and even to `bits256` without `as`
  if (rhs == this) {
    return true;
  }
  if (const TypeDataAlias* rhs_alias = rhs->try_as<TypeDataAlias>()) {
    return can_rhs_be_assigned(rhs_alias->underlying_type);
  }
  return false;
}

bool TypeDataCoins::can_rhs_be_assigned(TypePtr rhs) const {
  if (rhs == this) {
    return true;
  }
  if (rhs == TypeDataInt::create()) {
    return true;
  }
  if (const TypeDataAlias* rhs_alias = rhs->try_as<TypeDataAlias>()) {
    return can_rhs_be_assigned(rhs_alias->underlying_type);
  }
  return rhs == TypeDataNever::create();
}

bool TypeDataUnion::can_rhs_be_assigned(TypePtr rhs) const {
  if (rhs == this) {
    return true;
  }
  if (calculate_exact_variant_to_fit_rhs(rhs)) {    // `int` to `int | slice`, `int?` to `int8?`, `(int, null)` to `(int, T?) | slice`
    return true;
  }
  if (const TypeDataUnion* rhs_union = rhs->try_as<TypeDataUnion>()) {
    return has_all_variants_of(rhs_union);
  }
  if (const TypeDataAlias* rhs_alias = rhs->try_as<TypeDataAlias>()) {
    return can_rhs_be_assigned(rhs_alias->underlying_type);
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

// common helper for union types:
// - `int as int?` is ok
// - `int8 as int16?` is ok (primitive 1-slot nullable don't store UTag, rules are less strict)
// - `int as int | int16` is ok (exact match one of types)
// - `int as slice | null` is NOT ok (no rhs subtype fits)
// - `int as int8 | int16` is NOT ok (ambiguity)
static bool can_be_casted_to_union(TypePtr self, const TypeDataUnion* rhs_union) {
  if (rhs_union->is_primitive_nullable()) {     // casting to primitive 1-slot nullable
    return self == TypeDataNullLiteral::create() || self->can_be_casted_with_as_operator(rhs_union->or_null);
  }

  return rhs_union->calculate_exact_variant_to_fit_rhs(self) != nullptr;
}

bool TypeDataAlias::can_be_casted_with_as_operator(TypePtr cast_to) const {
  return underlying_type->can_be_casted_with_as_operator(cast_to);
}

bool TypeDataInt::can_be_casted_with_as_operator(TypePtr cast_to) const {
  if (const TypeDataUnion* to_union = cast_to->try_as<TypeDataUnion>()) {   // `int` as `int?` / `int` as `int | slice`
    return can_be_casted_to_union(this, to_union);
  }
  if (cast_to->try_as<TypeDataIntN>()) {    // `int` as `int8` / `int` as `uint2`
    return true;
  }
  if (cast_to == TypeDataCoins::create()) {   // `int` as `coins`
    return true;
  }
  if (const TypeDataAlias* to_alias = cast_to->try_as<TypeDataAlias>()) {
    return can_be_casted_with_as_operator(to_alias->underlying_type);
  }
  return cast_to == this;
}

bool TypeDataBool::can_be_casted_with_as_operator(TypePtr cast_to) const {
  if (cast_to == TypeDataInt::create()) {
    return true;
  }
  if (const TypeDataUnion* to_union = cast_to->try_as<TypeDataUnion>()) {
    return can_be_casted_to_union(this, to_union);
  }
  if (const auto* to_intN = cast_to->try_as<TypeDataIntN>()) {
    return !to_intN->is_unsigned;   // `bool` as `int8` ok, `bool` as `uintN` not (true is -1)
  }
  if (const TypeDataAlias* to_alias = cast_to->try_as<TypeDataAlias>()) {
    return can_be_casted_with_as_operator(to_alias->underlying_type);
  }
  return cast_to == this;
}

bool TypeDataCell::can_be_casted_with_as_operator(TypePtr cast_to) const {
  if (const TypeDataUnion* to_union = cast_to->try_as<TypeDataUnion>()) {
    return can_be_casted_to_union(this, to_union);
  }
  if (const TypeDataAlias* to_alias = cast_to->try_as<TypeDataAlias>()) {
    return can_be_casted_with_as_operator(to_alias->underlying_type);
  }
  return cast_to == this;
}

bool TypeDataSlice::can_be_casted_with_as_operator(TypePtr cast_to) const {
  if (cast_to->try_as<TypeDataBytesN>()) {  // `slice` to `bytes32` / `slice` to `bits8`
    return true;
  }
  if (const TypeDataUnion* to_union = cast_to->try_as<TypeDataUnion>()) {
    return can_be_casted_to_union(this, to_union);
  }
  if (const TypeDataAlias* to_alias = cast_to->try_as<TypeDataAlias>()) {
    return can_be_casted_with_as_operator(to_alias->underlying_type);
  }
  return cast_to == this;
}

bool TypeDataBuilder::can_be_casted_with_as_operator(TypePtr cast_to) const {
  if (const TypeDataUnion* to_union = cast_to->try_as<TypeDataUnion>()) {
    return can_be_casted_to_union(this, to_union);
  }
  if (const TypeDataAlias* to_alias = cast_to->try_as<TypeDataAlias>()) {
    return can_be_casted_with_as_operator(to_alias->underlying_type);
  }
  return cast_to == this;
}

bool TypeDataTuple::can_be_casted_with_as_operator(TypePtr cast_to) const {
  if (const TypeDataUnion* to_union = cast_to->try_as<TypeDataUnion>()) {
    return can_be_casted_to_union(this, to_union);
  }
  if (const TypeDataAlias* to_alias = cast_to->try_as<TypeDataAlias>()) {
    return can_be_casted_with_as_operator(to_alias->underlying_type);
  }
  return cast_to == this;
}

bool TypeDataContinuation::can_be_casted_with_as_operator(TypePtr cast_to) const {
  if (const TypeDataUnion* to_union = cast_to->try_as<TypeDataUnion>()) {
    return can_be_casted_to_union(this, to_union);
  }
  if (const TypeDataAlias* to_alias = cast_to->try_as<TypeDataAlias>()) {
    return can_be_casted_with_as_operator(to_alias->underlying_type);
  }
  return cast_to == this;
}

bool TypeDataNullLiteral::can_be_casted_with_as_operator(TypePtr cast_to) const {
  if (const TypeDataUnion* to_union = cast_to->try_as<TypeDataUnion>()) {   // `null` to `T?` / `null` to `... | null`
    return can_be_casted_to_union(this, to_union);
  }
  if (const TypeDataAlias* to_alias = cast_to->try_as<TypeDataAlias>()) {
    return can_be_casted_with_as_operator(to_alias->underlying_type);
  }
  return cast_to == this;
}

bool TypeDataFunCallable::can_be_casted_with_as_operator(TypePtr cast_to) const {
  if (const TypeDataUnion* to_union = cast_to->try_as<TypeDataUnion>()) {
    return can_be_casted_to_union(this, to_union);
  }
  if (const TypeDataAlias* to_alias = cast_to->try_as<TypeDataAlias>()) {
    return can_be_casted_with_as_operator(to_alias->underlying_type);
  }
  if (const TypeDataFunCallable* to_callable = cast_to->try_as<TypeDataFunCallable>()) {
    if (to_callable->params_size() != params_size()) {
      return false;
    }
    for (int i = 0; i < params_size(); ++i) {
      if (!params_types[i]->can_be_casted_with_as_operator(to_callable->params_types[i])) {
        return false;
      }
      if (!to_callable->params_types[i]->can_be_casted_with_as_operator(params_types[i])) {
        return false;
      }
    }
    return return_type->can_be_casted_with_as_operator(to_callable->return_type) &&
           to_callable->return_type->can_be_casted_with_as_operator(return_type);
  }
  return false;
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
  if (const TypeDataUnion* to_union = cast_to->try_as<TypeDataUnion>()) {
    return can_be_casted_to_union(this, to_union);
  }
  if (const TypeDataAlias* to_alias = cast_to->try_as<TypeDataAlias>()) {
    return can_be_casted_with_as_operator(to_alias->underlying_type);
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
  if (const TypeDataUnion* to_union = cast_to->try_as<TypeDataUnion>()) {
    return can_be_casted_to_union(this, to_union);
  }
  if (const TypeDataAlias* to_alias = cast_to->try_as<TypeDataAlias>()) {
    return can_be_casted_with_as_operator(to_alias->underlying_type);
  }
  return false;
}

bool TypeDataIntN::can_be_casted_with_as_operator(TypePtr cast_to) const {
  if (cast_to->try_as<TypeDataIntN>()) {    // `int8` as `int32`, `int256` as `uint5`, anything
    return true;
  }
  if (const TypeDataUnion* to_union = cast_to->try_as<TypeDataUnion>()) { // `int8` as `int32?`
    return can_be_casted_to_union(this, to_union);
  }
  if (const TypeDataAlias* to_alias = cast_to->try_as<TypeDataAlias>()) {
    return can_be_casted_with_as_operator(to_alias->underlying_type);
  }
  return cast_to == TypeDataInt::create() || cast_to == TypeDataCoins::create();
}

bool TypeDataBytesN::can_be_casted_with_as_operator(TypePtr cast_to) const {
  if (cast_to->try_as<TypeDataBytesN>()) {  // `bytes256` as `bytes512`, `bits1` as `bytes8`
    return true;
  }
  if (const TypeDataUnion* to_union = cast_to->try_as<TypeDataUnion>()) {   // `bytes8` as `slice?`
    return can_be_casted_to_union(this, to_union);
  }
  if (const TypeDataAlias* to_alias = cast_to->try_as<TypeDataAlias>()) {
    return can_be_casted_with_as_operator(to_alias->underlying_type);
  }
  return cast_to == TypeDataSlice::create();
}

bool TypeDataCoins::can_be_casted_with_as_operator(TypePtr cast_to) const {
  if (cast_to->try_as<TypeDataIntN>()) {    // `coins` as `int8`
    return true;
  }
  if (const TypeDataUnion* to_union = cast_to->try_as<TypeDataUnion>()) { // `coins` as `coins?` / `coins` as `int?`
    return can_be_casted_to_union(this, to_union);
  }
  if (const TypeDataAlias* to_alias = cast_to->try_as<TypeDataAlias>()) {
    return can_be_casted_with_as_operator(to_alias->underlying_type);
  }
  if (cast_to == TypeDataInt::create()) {
    return true;
  }
  return cast_to == this;
}

bool TypeDataUnion::can_be_casted_with_as_operator(TypePtr cast_to) const {
  if (const TypeDataUnion* to_union = cast_to->try_as<TypeDataUnion>()) {   // `int8 | int16` as `int16 | int8 | slice`
    if (to_union->is_primitive_nullable()) {
      return or_null && or_null->can_be_casted_with_as_operator(to_union->or_null);
    }
    return to_union->has_all_variants_of(this);
  }
  if (const TypeDataAlias* to_alias = cast_to->try_as<TypeDataAlias>()) {
    return can_be_casted_with_as_operator(to_alias->underlying_type);
  }
  return false;
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

bool TypeDataAlias::can_hold_tvm_null_instead() const {
  return underlying_type->can_hold_tvm_null_instead();
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

bool TypeDataUnion::can_hold_tvm_null_instead() const {
  if (get_width_on_stack() != 1) {    // `(int, int)?` / `()?` can not hold null instead
    return false;                     // only `int?` / `cell?` / `StructWith1IntField?` can
  }                                   // and some tricky situations like `(int, ())?`, but not `(int?, ())?`
  return or_null && !or_null->can_hold_tvm_null_instead();
}

bool TypeDataNever::can_hold_tvm_null_instead() const {
  return false;
}

bool TypeDataVoid::can_hold_tvm_null_instead() const {
  return false;
}


// union types creation is a bit tricky: nested unions are flattened, duplicates are removed
// so, a resolved union type has variants, each with unique type_id
// (type_id is calculated with aliases erasure)
void TypeDataUnion::append_union_type_variant(TypePtr variant, std::vector<TypePtr>& out_unique_variants) {
  for (TypePtr existing : out_unique_variants) {
    if (existing->get_type_id() == variant->get_type_id()) {
      return;
    }
  }

  out_unique_variants.push_back(variant);
}

bool TypeDataUnion::has_variant_with_type_id(int type_id) const {
  for (TypePtr self_variant : variants) {
    if (self_variant->get_type_id() == type_id) {
      return true;
    }
  }
  return false;
}

bool TypeDataUnion::has_all_variants_of(const TypeDataUnion* rhs_type) const {
  for (TypePtr rhs_variant : rhs_type->variants) {
    if (!has_variant_with_type_id(rhs_variant->get_type_id())) {
      return false;
    }
  }
  return true;
}

// given this = `T1 | T2 | ...` and rhs_type, find the only (not ambiguous) T_i that can accept it
TypePtr TypeDataUnion::calculate_exact_variant_to_fit_rhs(TypePtr rhs_type) const {
  // primitive 1-slot nullable don't store type_id, they can be assigned less strict, like `int?` to `int16?`
  if (const TypeDataUnion* rhs_union = rhs_type->unwrap_alias()->try_as<TypeDataUnion>()) {
    if (is_primitive_nullable() && rhs_union->is_primitive_nullable() && or_null->can_rhs_be_assigned(rhs_union->or_null)) {
      return this;
    }
    return nullptr;
  }
  // `int` to `int | int8` is okay: exact type matching
  for (TypePtr variant : variants) {
    if (variant->get_type_id() == rhs_type->get_type_id()) {
      return variant;
    }
  }

  // find the only T_i; it would also be used for transition at IR generation, like `(int,null)` to `(int, User?) | int`
  TypePtr first_covering = nullptr;
  for (TypePtr variant : variants) {
    if (variant->can_rhs_be_assigned(rhs_type)) {
      if (first_covering) {
        return nullptr;
      }
      first_covering = variant;
    }
  }
  return first_covering;
}


// --------------------------------------------
//    parsing type from tokens
//
// here we implement parsing types (mostly after colon) to TypeData
// example: `var v: int` is TypeDataInt
// example: `var v: (builder?, [cell])` is TypeDataTensor(TypeDataUnion(TypeDataBuilder,TypeDataNullLiteral), TypeDataTypedTuple(TypeDataCell))
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
    result = TypeDataUnion::create_nullable(result);
  }

  return result;
}

static TypePtr parse_type_expression(Lexer& lex) {
  TypePtr result = parse_type_nullable(lex);

  if (lex.tok() == tok_bitwise_or) {  // `int | slice`, `Pair2 | (Pair3 | null)`
    std::vector<TypePtr> items;
    items.emplace_back(result);
    while (lex.tok() == tok_bitwise_or) {
      lex.next();
      items.emplace_back(parse_type_nullable(lex));
    }
    result = TypeDataUnion::create(std::move(items));
  }

  if (lex.tok() == tok_arrow) {   // `int -> int`, `(cell, slice) -> void`, `int -> int -> int`, `int | cell -> void`
    lex.next();
    TypePtr return_type = parse_type_expression(lex);
    std::vector<TypePtr> params_types = {result};
    if (const auto* as_tensor = result->try_as<TypeDataTensor>()) {
      params_types = as_tensor->items;
    }
    result = TypeDataFunCallable::create(std::move(params_types), return_type);
  }

  return result;
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
