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
#include "compilation-errors.h"
#include "generics-helpers.h"
#include "compiler-state.h"
#include <charconv>
#include <unordered_map>

namespace tolk {

/*
 * Every TypeData has children_flags (just a mask of all children),
 * here we have an utility to calculate them at creation.
 */
class CalcChildrenFlags {
  int children_flags_mask = 0;

public:
  void feed_child(TypePtr inner) {
    children_flags_mask |= inner->flags;
  }

  void feed_child(const std::vector<TypePtr>& children) {
    for (TypePtr inner : children) {
      children_flags_mask |= inner->flags;
    }
  }

  int children_flags() const {
    return children_flags_mask;
  }
};

/*
 * This class stores a hashtable [TypePtr => type_id]
 * We need type_id to support union types, that are stored as tagged unions on a stack.
 * Every type actually contained inside a union, has type_id.
 * Some type_id are predefined (1 = int, etc.), but all user-defined types are assigned type_id.
 */
class TypeIdCalculation {
  static int last_type_id;
  static std::unordered_map<TypePtr, int> map_ptr_to_type_id;

public:
  static int assign_type_id(TypePtr self) {
    // type_id is calculated without aliases, based on "equal to";
    // for instance, `UserId` / `OwnerId` / `int` will have the same type_id without any runtime conversion
    auto it = std::find_if(map_ptr_to_type_id.begin(), map_ptr_to_type_id.end(), [self](std::pair<TypePtr, int> existing) {
      return existing.first->equal_to(self);
    });
    if (it != map_ptr_to_type_id.end()) {
      return it->second;
    }

    int type_id = ++last_type_id;
    map_ptr_to_type_id[self] = type_id;
    return type_id;
  }
};


int TypeIdCalculation::last_type_id = 128;       // below 128 reserved for built-in types
std::unordered_map<TypePtr, int> TypeIdCalculation::map_ptr_to_type_id;

TypePtr TypeDataInt::singleton;
TypePtr TypeDataBool::singleton;
TypePtr TypeDataCell::singleton;
TypePtr TypeDataSlice::singleton;
TypePtr TypeDataBuilder::singleton;
TypePtr TypeDataTuple::singleton;
TypePtr TypeDataContinuation::singleton;
TypePtr TypeDataAddress::singleton_internal;
TypePtr TypeDataAddress::singleton_any;
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
  TypeDataAddress::singleton_internal = new TypeDataAddress(0);
  TypeDataAddress::singleton_any = new TypeDataAddress(1);
  TypeDataNullLiteral::singleton = new TypeDataNullLiteral;
  TypeDataCoins::singleton = new TypeDataCoins;
  TypeDataUnknown::singleton = new TypeDataUnknown;
  TypeDataNever::singleton = new TypeDataNever;
  TypeDataVoid::singleton = new TypeDataVoid;
}


TypePtr TypeData::unwrap_alias_slow_path(TypePtr lhs) {
  TypePtr unwrapped = lhs;
  while (const TypeDataAlias* as_alias = unwrapped->try_as<TypeDataAlias>()) {
    unwrapped = as_alias->underlying_type;
  }
  return unwrapped;
}

// having `type UserId = int` and `type OwnerId = int` (when their underlying types are equal),
// make `UserId` and `OwnerId` NOT equal and NOT assignable (although they'll have the same type_id);
// it allows overloading methods for these types independently, e.g.
// > type BalanceList = dict
// > type AssetList = dict
// > fun BalanceList.validate(self)
// > fun AssetList.validate(self)
static bool are_two_equal_type_aliases_different(const TypeDataAlias* t1, const TypeDataAlias* t2) {
  if (t1->alias_ref == t2->alias_ref) {
    return false;
  }
  if (t1->alias_ref->is_instantiation_of_generic_alias() && t2->alias_ref->is_instantiation_of_generic_alias()) {
    return !t1->alias_ref->substitutedTs->equal_to(t2->alias_ref->substitutedTs);
  }
  // handle `type MInt2 = MInt1`, as well as `type BalanceList = dict`, then they are equal
  const TypeDataAlias* t_und1 = t1->underlying_type->try_as<TypeDataAlias>();
  const TypeDataAlias* t_und2 = t2->underlying_type->try_as<TypeDataAlias>();
  bool one_aliases_another = (t_und1 && t_und1->alias_ref == t2->alias_ref)
                          || (t_und2 && t1->alias_ref == t_und2->alias_ref);
  return !one_aliases_another;
}

// --------------------------------------------
//    create()
//
// all constructors of TypeData classes are private, only TypeData*::create() is allowed
// each non-trivial create() method calculates hash
// and creates an object only if it isn't found in a global hashtable
//

TypePtr TypeDataAlias::create(AliasDefPtr alias_ref) {
  TypePtr underlying_type = alias_ref->underlying_type;
  if (underlying_type == TypeDataNullLiteral::create() || underlying_type == TypeDataNever::create() || underlying_type == TypeDataVoid::create()) {
    return underlying_type;   // aliasing these types is strange, don't store an alias
  }

  CalcChildrenFlags reg;
  reg.feed_child(alias_ref->underlying_type);
  return new TypeDataAlias(reg.children_flags(), alias_ref, underlying_type);
}

TypePtr TypeDataFunCallable::create(std::vector<TypePtr>&& params_types, TypePtr return_type) {
  CalcChildrenFlags reg;
  reg.feed_child(params_types);
  reg.feed_child(return_type);
  return new TypeDataFunCallable(reg.children_flags(), std::move(params_types), return_type);
}

TypePtr TypeDataGenericT::create(std::string&& nameT) {
  return new TypeDataGenericT(std::move(nameT));
}

TypePtr TypeDataGenericTypeWithTs::create(StructPtr struct_ref, AliasDefPtr alias_ref, std::vector<TypePtr>&& type_arguments) {
  if (struct_ref) {
    tolk_assert(alias_ref == nullptr && struct_ref->is_generic_struct());
  } else {
    tolk_assert(struct_ref == nullptr && alias_ref->is_generic_alias());
  }

  CalcChildrenFlags reg;
  reg.feed_child(type_arguments);
  return new TypeDataGenericTypeWithTs(reg.children_flags(), struct_ref, alias_ref, std::move(type_arguments));
}

TypePtr TypeDataStruct::create(StructPtr struct_ref) {
  return new TypeDataStruct(struct_ref);
}

TypePtr TypeDataEnum::create(EnumDefPtr enum_ref) {
  return new TypeDataEnum(enum_ref);
}

TypePtr TypeDataTensor::create(std::vector<TypePtr>&& items) {
  CalcChildrenFlags reg;
  reg.feed_child(items);
  return new TypeDataTensor(reg.children_flags(), std::move(items));
}

TypePtr TypeDataBrackets::create(std::vector<TypePtr>&& items) {
  CalcChildrenFlags reg;
  reg.feed_child(items);
  return new TypeDataBrackets(reg.children_flags(), std::move(items));
}

TypePtr TypeDataIntN::create(int n_bits, bool is_unsigned, bool is_variadic) {
  return new TypeDataIntN(n_bits, is_unsigned, is_variadic);
}

TypePtr TypeDataBitsN::create(int n_width, bool is_bits) {
  return new TypeDataBitsN(n_width, is_bits);
}

TypePtr TypeDataUnion::create(std::vector<TypePtr>&& variants) {
  // flatten variants and remove duplicates
  // note, that `int | slice` and `int | int | slice` are different TypePtr, but actually the same variants;
  // note, that `UserId | OwnerId` (both are aliases to `int`) will emit `UserId` (OwnerId is a duplicate)
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

  if (flat_variants.size() == 1) {    // `int | int`
    return flat_variants[0];
  }

  CalcChildrenFlags reg;
  reg.feed_child(flat_variants);
  return new TypeDataUnion(reg.children_flags(), or_null, std::move(flat_variants));
}

TypePtr TypeDataMapKV::create(TypePtr TKey, TypePtr TValue) {
  CalcChildrenFlags reg;
  reg.feed_child(TKey);
  reg.feed_child(TValue);
  return new TypeDataMapKV(reg.children_flags(), TKey, TValue);
}


// --------------------------------------------
//    get_width_on_stack()
//
// calculate, how many stack slots the type occupies, e.g. `int`=1, `(int,int)`=2, `(int,int)?`=3
// it's calculated dynamically (not saved at TypeData*::create) to overcome problems with
// - recursive struct mentions (to create TypeDataStruct without knowing width of children)
// - uninitialized generics (that don't make any sense upon being instantiated)
//

int TypeDataAlias::get_width_on_stack() const {
  return underlying_type->get_width_on_stack();
}

int TypeDataGenericT::get_width_on_stack() const {
  tolk_assert(false);
}

int TypeDataGenericTypeWithTs::get_width_on_stack() const {
  tolk_assert(false);
}

int TypeDataStruct::get_width_on_stack() const {
  int width_on_stack = 0;
  for (StructFieldPtr field_ref : struct_ref->fields) {
    width_on_stack += field_ref->declared_type->get_width_on_stack();
  }
  return width_on_stack;
}

int TypeDataTensor::get_width_on_stack() const {
  int width_on_stack = 0;
  for (TypePtr item : items) {
    width_on_stack += item->get_width_on_stack();
  }
  return width_on_stack;
}

int TypeDataUnion::get_width_on_stack() const {
  if (or_null && or_null->can_hold_tvm_null_instead()) {
    return 1;
  }

  // `T1 | T2 | ...` occupy max(W[i]) + 1 slot for UTag (stores type_id or 0 for null)
  int max_child_width = 0;
  for (TypePtr i : variants) {
    if (i != TypeDataNullLiteral::create()) {   // `Empty | () | null` totally should be 1 (0 + 1 for UTag)
      max_child_width = std::max(max_child_width, i->get_width_on_stack());
    }
  }
  return max_child_width + 1;
}

int TypeDataNever::get_width_on_stack() const {
  return 0;
}

int TypeDataVoid::get_width_on_stack() const {
  return 0;
}


// --------------------------------------------
//    get_type_id()
//
// in order to support union types, every type that can be stored inside a union has a unique type_id
// some are predefined (1 = int, etc. in .h file), the others are here
//

int TypeDataAlias::get_type_id() const {
  tolk_assert(!alias_ref->is_generic_alias());
  return underlying_type->get_type_id();
}

int TypeDataAddress::get_type_id() const {
  if (is_internal()) {
    return type_id_address_int;
  }
  return type_id_address_any;
}

int TypeDataFunCallable::get_type_id() const {
  return TypeIdCalculation::assign_type_id(this);
}

int TypeDataGenericT::get_type_id() const {
  tolk_assert(false);    // generics must have been instantiated in advance
}

int TypeDataGenericTypeWithTs::get_type_id() const {
  tolk_assert(false);    // `Wrapper<T>` has to be resolved in advance
}

int TypeDataStruct::get_type_id() const {
  tolk_assert(!struct_ref->is_generic_struct());
  return TypeIdCalculation::assign_type_id(this);
}

int TypeDataEnum::get_type_id() const {
  return TypeIdCalculation::assign_type_id(this);
}

int TypeDataTensor::get_type_id() const {
  tolk_assert(!has_genericT_inside());
  return TypeIdCalculation::assign_type_id(this);
}

int TypeDataBrackets::get_type_id() const {
  tolk_assert(!has_genericT_inside());
  return TypeIdCalculation::assign_type_id(this);
}

int TypeDataIntN::get_type_id() const {
  switch (n_bits) {
    case 8:   return type_id_int8   + is_unsigned;    // for common intN, use predefined small numbers
    case 16:  return type_id_int16  + is_unsigned;
    case 32:  return type_id_int32  + is_unsigned;
    case 64:  return type_id_int64  + is_unsigned;
    case 128: return type_id_int128 + is_unsigned;
    case 256: return type_id_int256 + is_unsigned;
    default:  return TypeIdCalculation::assign_type_id(this);
  }
}

int TypeDataBitsN::get_type_id() const {
  return TypeIdCalculation::assign_type_id(this);
}

int TypeDataUnion::get_type_id() const {
  tolk_assert(false);    // a union can not be inside a union
}

int TypeDataMapKV::get_type_id() const {
  tolk_assert(!has_genericT_inside());
  return TypeIdCalculation::assign_type_id(this);
}

int TypeDataUnknown::get_type_id() const {
  tolk_assert(false);    // unknown can not be inside a union
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

std::string TypeDataAddress::as_human_readable() const {
  if (is_internal()) {
    return "address";
  }
  return "any_address";
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

std::string TypeDataGenericTypeWithTs::as_human_readable() const {
  std::string result = struct_ref ? struct_ref->name : alias_ref->name;
  result += '<';
  for (TypePtr argT : type_arguments) {
    if (result[result.size() - 1] != '<') {
      result += ", ";
    }
    result += argT->as_human_readable();
  }
  result += '>';
  return result;
}

std::string TypeDataStruct::as_human_readable() const {
  return struct_ref->name;
}

std::string TypeDataEnum::as_human_readable() const {
  return enum_ref->name;
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

std::string TypeDataBrackets::as_human_readable() const {
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

std::string TypeDataBitsN::as_human_readable() const {
  std::string s_bits = is_bits ? "bits" : "bytes";
  return s_bits + std::to_string(n_width);
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

std::string TypeDataMapKV::as_human_readable() const {
  return "map<" + TKey->as_human_readable() + ", " + TValue->as_human_readable() + ">";
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

TypePtr TypeDataGenericTypeWithTs::replace_children_custom(const ReplacerCallbackT& callback) const {
  std::vector<TypePtr> mapped;
  mapped.reserve(type_arguments.size());
  for (TypePtr argT : type_arguments) {
    mapped.push_back(argT->replace_children_custom(callback));
  }
  return callback(create(struct_ref, alias_ref, std::move(mapped)));
}

TypePtr TypeDataTensor::replace_children_custom(const ReplacerCallbackT& callback) const {
  std::vector<TypePtr> mapped;
  mapped.reserve(items.size());
  for (TypePtr item : items) {
    mapped.push_back(item->replace_children_custom(callback));
  }
  return callback(create(std::move(mapped)));
}

TypePtr TypeDataBrackets::replace_children_custom(const ReplacerCallbackT& callback) const {
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

TypePtr TypeDataMapKV::replace_children_custom(const ReplacerCallbackT& callback) const {
  return callback(create(TKey->replace_children_custom(callback), TValue->replace_children_custom(callback)));
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
  if (const TypeDataAlias* rhs_alias = rhs->try_as<TypeDataAlias>()) {
    // having `type UserId = int` and `type OwnerId = int`, make them NOT assignable without `as`
    // (although they both have the same type_id) 
    if (underlying_type->equal_to(rhs_alias->underlying_type)) {
      return !are_two_equal_type_aliases_different(this, rhs_alias);
    }
  }
  return underlying_type->can_rhs_be_assigned(rhs);
}

bool TypeDataInt::can_rhs_be_assigned(TypePtr rhs) const {
  if (rhs == singleton) {
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
  if (rhs == singleton) {
    return true;
  }
  if (const TypeDataAlias* rhs_alias = rhs->try_as<TypeDataAlias>()) {
    return can_rhs_be_assigned(rhs_alias->underlying_type);
  }
  return rhs == TypeDataNever::create();
}

bool TypeDataCell::can_rhs_be_assigned(TypePtr rhs) const {
  if (rhs == singleton) {
    return true;
  }
  if (const TypeDataStruct* rhs_struct = rhs->try_as<TypeDataStruct>()) {
    if (rhs_struct->struct_ref->is_instantiation_of_generic_struct() && rhs_struct->struct_ref->base_struct_ref->name == "Cell") {
      return true;      // Cell<Something> to cell, e.g. `contract.setData(obj.toCell())`
    }
  }
  if (const TypeDataAlias* rhs_alias = rhs->try_as<TypeDataAlias>()) {
    return can_rhs_be_assigned(rhs_alias->underlying_type);
  }
  return rhs == TypeDataNever::create();
}

bool TypeDataSlice::can_rhs_be_assigned(TypePtr rhs) const {
  if (rhs == singleton) {
    return true;
  }
  if (const TypeDataAlias* rhs_alias = rhs->try_as<TypeDataAlias>()) {
    return can_rhs_be_assigned(rhs_alias->underlying_type);
  }
  return rhs == TypeDataNever::create();   // note, that bitsN/address is NOT automatically cast to slice without `as` operator
}

bool TypeDataBuilder::can_rhs_be_assigned(TypePtr rhs) const {
  if (rhs == singleton) {
    return true;
  }
  if (const TypeDataAlias* rhs_alias = rhs->try_as<TypeDataAlias>()) {
    return can_rhs_be_assigned(rhs_alias->underlying_type);
  }
  return rhs == TypeDataNever::create();
}

bool TypeDataTuple::can_rhs_be_assigned(TypePtr rhs) const {
  if (rhs == singleton) {
    return true;
  }
  if (const TypeDataAlias* rhs_alias = rhs->try_as<TypeDataAlias>()) {
    return can_rhs_be_assigned(rhs_alias->underlying_type);
  }
  return rhs == TypeDataNever::create();
}

bool TypeDataContinuation::can_rhs_be_assigned(TypePtr rhs) const {
  if (rhs == singleton) {
    return true;
  }
  if (const TypeDataAlias* rhs_alias = rhs->try_as<TypeDataAlias>()) {
    return can_rhs_be_assigned(rhs_alias->underlying_type);
  }
  return rhs == TypeDataNever::create();
}

bool TypeDataAddress::can_rhs_be_assigned(TypePtr rhs) const {
  if (const TypeDataAddress* rhs_address = rhs->try_as<TypeDataAddress>()) {
    // note that not `address` to `any_address` also requires manual `as`
    return kind == rhs_address->kind;
  }
  if (const TypeDataAlias* rhs_alias = rhs->try_as<TypeDataAlias>()) {
    return can_rhs_be_assigned(rhs_alias->underlying_type);
  }
  return rhs == TypeDataNever::create();   // note, that slice is NOT automatically cast to address without `as` operator
}

bool TypeDataNullLiteral::can_rhs_be_assigned(TypePtr rhs) const {
  if (rhs == singleton) {
    return true;
  }
  if (const TypeDataAlias* rhs_alias = rhs->try_as<TypeDataAlias>()) {
    return can_rhs_be_assigned(rhs_alias->underlying_type);
  }
  return rhs == TypeDataNever::create();
}

bool TypeDataFunCallable::can_rhs_be_assigned(TypePtr rhs) const {
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
  return false;
}

bool TypeDataGenericTypeWithTs::can_rhs_be_assigned(TypePtr rhs) const {
  return false;
}

bool TypeDataStruct::can_rhs_be_assigned(TypePtr rhs) const {
  if (const TypeDataStruct* rhs_struct = rhs->try_as<TypeDataStruct>()) {   // C<C<int>> = C<CIntAlias>
    return struct_ref == rhs_struct->struct_ref || equal_to(rhs_struct);
  }
  if (const TypeDataAlias* rhs_alias = rhs->try_as<TypeDataAlias>()) {
    return can_rhs_be_assigned(rhs_alias->underlying_type);
  }
  return rhs == TypeDataNever::create();
}

bool TypeDataEnum::can_rhs_be_assigned(TypePtr rhs) const {
  if (const TypeDataEnum* rhs_enum = rhs->try_as<TypeDataEnum>()) {
    return enum_ref == rhs_enum->enum_ref;
  }
  if (const TypeDataAlias* rhs_alias = rhs->try_as<TypeDataAlias>()) {
    return can_rhs_be_assigned(rhs_alias->underlying_type);
  }
  return rhs == TypeDataNever::create();
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

bool TypeDataBrackets::can_rhs_be_assigned(TypePtr rhs) const {
  if (const auto* as_tuple = rhs->try_as<TypeDataBrackets>(); as_tuple && as_tuple->size() == size()) {
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
  if (rhs == TypeDataInt::create()) {
    return true;
  }
  if (const TypeDataIntN* rhs_intN = rhs->try_as<TypeDataIntN>()) {
    // `int8` is NOT assignable to `int32` without `as`
    return n_bits == rhs_intN->n_bits && is_unsigned == rhs_intN->is_unsigned && is_variadic == rhs_intN->is_variadic;
  }
  if (const TypeDataAlias* rhs_alias = rhs->try_as<TypeDataAlias>()) {
    return can_rhs_be_assigned(rhs_alias->underlying_type);
  }
  return rhs == TypeDataNever::create();
}

bool TypeDataBitsN::can_rhs_be_assigned(TypePtr rhs) const {
  if (const TypeDataBitsN* rhs_bitsN = rhs->try_as<TypeDataBitsN>()) {
    // `slice` is NOT assignable to bitsN without `as`
    // `bytes32` is NOT assignable to `bytes256` and even to `bits256` without `as`
    return n_width == rhs_bitsN->n_width && is_bits == rhs_bitsN->is_bits;
  }
  if (const TypeDataAlias* rhs_alias = rhs->try_as<TypeDataAlias>()) {
    return can_rhs_be_assigned(rhs_alias->underlying_type);
  }
  return rhs == TypeDataNever::create();
}

bool TypeDataCoins::can_rhs_be_assigned(TypePtr rhs) const {
  if (rhs == singleton) {
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

bool TypeDataMapKV::can_rhs_be_assigned(TypePtr rhs) const {
  if (const TypeDataMapKV* rhs_map = rhs->try_as<TypeDataMapKV>()) {
    return TKey->equal_to(rhs_map->TKey) && TValue->equal_to(rhs_map->TValue);
  }
  if (const TypeDataAlias* rhs_alias = rhs->try_as<TypeDataAlias>()) {
    return can_rhs_be_assigned(rhs_alias->underlying_type);
  }
  return rhs == TypeDataNever::create();
}

bool TypeDataUnknown::can_rhs_be_assigned(TypePtr rhs) const {
  return true;
}

bool TypeDataNever::can_rhs_be_assigned(TypePtr rhs) const {
  return rhs == singleton;
}

bool TypeDataVoid::can_rhs_be_assigned(TypePtr rhs) const {
  if (rhs == singleton) {
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
  if (cast_to->try_as<TypeDataEnum>()) {  // `int` as `Color` (all enums are integer)
    return true;
  }
  if (const TypeDataAlias* to_alias = cast_to->try_as<TypeDataAlias>()) {
    return can_be_casted_with_as_operator(to_alias->underlying_type);
  }
  return cast_to == singleton;
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
  return cast_to == singleton;
}

bool TypeDataCell::can_be_casted_with_as_operator(TypePtr cast_to) const {
  if (const TypeDataUnion* to_union = cast_to->try_as<TypeDataUnion>()) {
    return can_be_casted_to_union(this, to_union);
  }
  if (const TypeDataStruct* to_struct = cast_to->try_as<TypeDataStruct>()) {    // cell as Cell<T>
    return to_struct->struct_ref->is_instantiation_of_generic_struct() && to_struct->struct_ref->base_struct_ref->name == "Cell";
  }
  if (const TypeDataAlias* to_alias = cast_to->try_as<TypeDataAlias>()) {
    return can_be_casted_with_as_operator(to_alias->underlying_type);
  }
  return cast_to == singleton;
}

bool TypeDataSlice::can_be_casted_with_as_operator(TypePtr cast_to) const {
  if (cast_to->try_as<TypeDataBitsN>()) {  // `slice` to `bytes32` / `slice` to `bits8`
    return true;
  }
  if (cast_to->try_as<TypeDataAddress>()) {   // `slice` to `address`
    return true;
  }
  if (const TypeDataUnion* to_union = cast_to->try_as<TypeDataUnion>()) {
    return can_be_casted_to_union(this, to_union);
  }
  if (const TypeDataAlias* to_alias = cast_to->try_as<TypeDataAlias>()) {
    return can_be_casted_with_as_operator(to_alias->underlying_type);
  }
  return cast_to == singleton;
}

bool TypeDataBuilder::can_be_casted_with_as_operator(TypePtr cast_to) const {
  if (const TypeDataUnion* to_union = cast_to->try_as<TypeDataUnion>()) {
    return can_be_casted_to_union(this, to_union);
  }
  if (const TypeDataAlias* to_alias = cast_to->try_as<TypeDataAlias>()) {
    return can_be_casted_with_as_operator(to_alias->underlying_type);
  }
  return cast_to == singleton;
}

bool TypeDataTuple::can_be_casted_with_as_operator(TypePtr cast_to) const {
  if (const TypeDataUnion* to_union = cast_to->try_as<TypeDataUnion>()) {
    return can_be_casted_to_union(this, to_union);
  }
  if (const TypeDataAlias* to_alias = cast_to->try_as<TypeDataAlias>()) {
    return can_be_casted_with_as_operator(to_alias->underlying_type);
  }
  return cast_to == singleton;
}

bool TypeDataContinuation::can_be_casted_with_as_operator(TypePtr cast_to) const {
  if (const TypeDataUnion* to_union = cast_to->try_as<TypeDataUnion>()) {
    return can_be_casted_to_union(this, to_union);
  }
  if (const TypeDataAlias* to_alias = cast_to->try_as<TypeDataAlias>()) {
    return can_be_casted_with_as_operator(to_alias->underlying_type);
  }
  return cast_to == singleton;
}

bool TypeDataAddress::can_be_casted_with_as_operator(TypePtr cast_to) const {
  if (cast_to == TypeDataSlice::create() || cast_to->try_as<TypeDataBitsN>()) {
    return true;
  }
  if (cast_to->try_as<TypeDataAddress>()) {
    return true;    // `any_address` as `address` and any other casts are ok
  }
  if (const TypeDataUnion* to_union = cast_to->try_as<TypeDataUnion>()) {
    return can_be_casted_to_union(this, to_union);
  }
  if (const TypeDataAlias* to_alias = cast_to->try_as<TypeDataAlias>()) {
    return can_be_casted_with_as_operator(to_alias->underlying_type);
  }
  return false;
}

bool TypeDataNullLiteral::can_be_casted_with_as_operator(TypePtr cast_to) const {
  if (const TypeDataUnion* to_union = cast_to->try_as<TypeDataUnion>()) {   // `null` to `T?` / `null` to `... | null`
    return can_be_casted_to_union(this, to_union);
  }
  if (const TypeDataAlias* to_alias = cast_to->try_as<TypeDataAlias>()) {
    return can_be_casted_with_as_operator(to_alias->underlying_type);
  }
  return cast_to == singleton;
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

bool TypeDataGenericTypeWithTs::can_be_casted_with_as_operator(TypePtr cast_to) const {
  return true;
}

bool TypeDataStruct::can_be_casted_with_as_operator(TypePtr cast_to) const {
  if (const TypeDataUnion* to_union = cast_to->try_as<TypeDataUnion>()) {
    return can_be_casted_to_union(this, to_union);
  }
  if (cast_to == TypeDataCell::create()) {    // Cell<T> as cell
    return struct_ref->is_instantiation_of_generic_struct() && struct_ref->base_struct_ref->name == "Cell";
  }
  if (const TypeDataAlias* to_alias = cast_to->try_as<TypeDataAlias>()) {
    return can_be_casted_with_as_operator(to_alias->underlying_type);
  }
  if (const TypeDataStruct* to_struct = cast_to->try_as<TypeDataStruct>()) {   // C<C<int>> as C<CIntAlias>
    return equal_to(to_struct);
  }
  return false;
}

bool TypeDataEnum::can_be_casted_with_as_operator(TypePtr cast_to) const {
  if (cast_to == TypeDataInt::create() || cast_to == TypeDataCoins::create() || cast_to->try_as<TypeDataIntN>()) {
    return true;
  }
  if (cast_to->try_as<TypeDataEnum>()) {
    return true;    // all enums are integers, they can be `as` cast to each other
  }
  if (const TypeDataUnion* to_union = cast_to->try_as<TypeDataUnion>()) {
    return can_be_casted_to_union(this, to_union);
  }
  if (const TypeDataAlias* to_alias = cast_to->try_as<TypeDataAlias>()) {
    return can_be_casted_with_as_operator(to_alias->underlying_type);
  }
  return false;
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

bool TypeDataBrackets::can_be_casted_with_as_operator(TypePtr cast_to) const {
  if (cast_to->try_as<TypeDataTuple>()) {   // `[int, int]` as `tuple`
    return true;
  }
  if (const auto* to_tuple = cast_to->try_as<TypeDataBrackets>(); to_tuple && to_tuple->size() == size()) {
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

bool TypeDataBitsN::can_be_casted_with_as_operator(TypePtr cast_to) const {
  if (cast_to->try_as<TypeDataBitsN>()) {  // `bytes256` as `bytes512`, `bits1` as `bytes8`
    return true;
  }
  if (cast_to->try_as<TypeDataAddress>()) {   // `bytes267` as `address`
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
  return cast_to == singleton;
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

bool TypeDataMapKV::can_be_casted_with_as_operator(TypePtr cast_to) const {
  if (const TypeDataMapKV* to_map = cast_to->try_as<TypeDataMapKV>()) {
    return TKey->equal_to(to_map->TKey) && TValue->equal_to(to_map->TValue);
  }
  if (const TypeDataUnion* to_union = cast_to->try_as<TypeDataUnion>()) {
    return can_be_casted_to_union(this, to_union);
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

bool TypeDataNever::can_be_casted_with_as_operator(TypePtr cast_to) const {
  return true;
}

bool TypeDataVoid::can_be_casted_with_as_operator(TypePtr cast_to) const {
  return cast_to == singleton;
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

bool TypeDataStruct::can_hold_tvm_null_instead() const {
  if (get_width_on_stack() != 1) {    // example that can hold null: `{ field: int }`
    return false;                     // another example: `{ e: Empty, field: ((), int) }`
  }                                   // examples can NOT: `{ field1: int, field2: int }`, `{ field1: int? }`
  for (StructFieldPtr field : struct_ref->fields) {
    if (field->declared_type->get_width_on_stack() == 1 && !field->declared_type->can_hold_tvm_null_instead()) {
      return false;
    }
  }
  return true;
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

bool TypeDataMapKV::can_hold_tvm_null_instead() const {
  return false;   // map is an optional cell, so `map?` requires a nullable presence slot
}

bool TypeDataNever::can_hold_tvm_null_instead() const {
  return false;
}

bool TypeDataVoid::can_hold_tvm_null_instead() const {
  return false;
}


// --------------------------------------------
//    equal_to()
//
// comparing types for equality (when implementation differs from a default "compare pointers");
// two types are EQUAL is a much more strict property than "assignable";
// a union type can hold only non-equal types; for instance, having `type MyInt = int`, a union `int | MyInt` == `int`;
// searching for a compatible method for a receiver is also based on equal_to() as first priority
//

bool TypeDataAlias::equal_to(TypePtr rhs) const {
  if (const TypeDataAlias* rhs_alias = rhs->try_as<TypeDataAlias>()) {
    // given `type UserId = int` and `type OwnerId = int`, treat them as NOT equal (they are also not assignable);
    // (but nevertheless, they will have the same type_id, and `UserId | OwnerId` is not a valid union)
    if (underlying_type->equal_to(rhs_alias->underlying_type)) {
      return !are_two_equal_type_aliases_different(this, rhs_alias);
    }
  }
  return underlying_type->equal_to(rhs);
}

bool TypeDataFunCallable::equal_to(TypePtr rhs) const {
  if (const TypeDataFunCallable* rhs_callable = rhs->try_as<TypeDataFunCallable>(); rhs_callable && rhs_callable->params_size() == params_size()) {
    for (int i = 0; i < params_size(); ++i) {
      if (!params_types[i]->equal_to(rhs_callable->params_types[i])) {
        return false;
      }
    }
    return return_type->equal_to(rhs_callable->return_type);
  }
  if (const TypeDataAlias* rhs_alias = rhs->try_as<TypeDataAlias>()) {
    return equal_to(rhs_alias->underlying_type);
  }
  return false;
}

bool TypeDataGenericT::equal_to(TypePtr rhs) const {
  if (const TypeDataGenericT* rhs_T = rhs->try_as<TypeDataGenericT>()) {
    return nameT == rhs_T->nameT;
  }
  return false;
}

bool TypeDataGenericTypeWithTs::equal_to(TypePtr rhs) const {
  if (const TypeDataGenericTypeWithTs* rhs_Ts = rhs->try_as<TypeDataGenericTypeWithTs>(); rhs_Ts && size() == rhs_Ts->size()) {
    for (int i = 0; i < size(); ++i) {
      if (!type_arguments[i]->equal_to(rhs_Ts->type_arguments[i])) {
        return false;
      }
    }
    return alias_ref == rhs_Ts->alias_ref && struct_ref == rhs_Ts->struct_ref;
  }
  if (const TypeDataAlias* rhs_alias = rhs->try_as<TypeDataAlias>()) {
    return equal_to(rhs_alias->underlying_type);
  }
  return false;
}

bool TypeDataStruct::equal_to(TypePtr rhs) const {
  if (const TypeDataStruct* rhs_struct = rhs->try_as<TypeDataStruct>()) {
    if (struct_ref == rhs_struct->struct_ref) {
      return true;
    }
    if (struct_ref->is_instantiation_of_generic_struct() && rhs_struct->struct_ref->is_instantiation_of_generic_struct()) {
      return struct_ref->base_struct_ref == rhs_struct->struct_ref->base_struct_ref
          && struct_ref->substitutedTs->equal_to(rhs_struct->struct_ref->substitutedTs);
    }
  }
  if (const TypeDataAlias* rhs_alias = rhs->try_as<TypeDataAlias>()) {
    return equal_to(rhs_alias->underlying_type);
  }
  return false;
}

bool TypeDataEnum::equal_to(TypePtr rhs) const {
  if (const TypeDataEnum* rhs_enum = rhs->try_as<TypeDataEnum>()) {
    return enum_ref == rhs_enum->enum_ref;
  }
  if (const TypeDataAlias* rhs_alias = rhs->try_as<TypeDataAlias>()) {
    return equal_to(rhs_alias->underlying_type);
  }
  return false;
}

bool TypeDataTensor::equal_to(TypePtr rhs) const {
  if (const TypeDataTensor* rhs_tensor = rhs->try_as<TypeDataTensor>(); rhs_tensor && size() == rhs_tensor->size()) {
    for (int i = 0; i < size(); ++i) {
      if (!items[i]->equal_to(rhs_tensor->items[i])) {
        return false;
      }
    }
    return true;
  }
  if (const TypeDataAlias* rhs_alias = rhs->try_as<TypeDataAlias>()) {
    return equal_to(rhs_alias->underlying_type);
  }
  return false;
}

bool TypeDataBrackets::equal_to(TypePtr rhs) const {
  if (const TypeDataBrackets* rhs_brackets = rhs->try_as<TypeDataBrackets>(); rhs_brackets && size() == rhs_brackets->size()) {
    for (int i = 0; i < size(); ++i) {
      if (!items[i]->equal_to(rhs_brackets->items[i])) {
        return false;
      }
    }
    return true;
  }
  if (const TypeDataAlias* rhs_alias = rhs->try_as<TypeDataAlias>()) {
    return equal_to(rhs_alias->underlying_type);
  }
  return false;
}

bool TypeDataIntN::equal_to(TypePtr rhs) const {
  if (const TypeDataIntN* rhs_intN = rhs->try_as<TypeDataIntN>()) {
    return n_bits == rhs_intN->n_bits && is_unsigned == rhs_intN->is_unsigned && is_variadic == rhs_intN->is_variadic;
  }
  if (const TypeDataAlias* rhs_alias = rhs->try_as<TypeDataAlias>()) {
    return equal_to(rhs_alias->underlying_type);
  }
  return false;
}

bool TypeDataBitsN::equal_to(TypePtr rhs) const {
  if (const TypeDataBitsN* rhs_bitsN = rhs->try_as<TypeDataBitsN>()) {
    return n_width == rhs_bitsN->n_width && is_bits == rhs_bitsN->is_bits;
  }
  if (const TypeDataAlias* rhs_alias = rhs->try_as<TypeDataAlias>()) {
    return equal_to(rhs_alias->underlying_type);
  }
  return false;
}

bool TypeDataUnion::equal_to(TypePtr rhs) const {
  if (const TypeDataUnion* rhs_union = rhs->try_as<TypeDataUnion>()) {
    return variants.size() == rhs_union->variants.size() && has_all_variants_of(rhs_union);
  }
  if (const TypeDataAlias* rhs_alias = rhs->try_as<TypeDataAlias>()) {
    return equal_to(rhs_alias->underlying_type);
  }
  return false;
}

bool TypeDataMapKV::equal_to(TypePtr rhs) const {
  if (const TypeDataMapKV* rhs_map = rhs->try_as<TypeDataMapKV>()) {
    return TKey->equal_to(rhs_map->TKey) && TValue->equal_to(rhs_map->TValue);
  }
  if (const TypeDataAlias* rhs_alias = rhs->try_as<TypeDataAlias>()) {
    return equal_to(rhs_alias->underlying_type);
  }
  return false;
}


// union types creation is a bit tricky: nested unions are flattened, duplicates are removed
// so, a resolved union type has variants, each will be assigned a unique type_id (tagged unions)
void TypeDataUnion::append_union_type_variant(TypePtr variant, std::vector<TypePtr>& out_unique_variants) {
  // having `UserId | OwnerId` (both are aliases to `int`) merge them into just `UserId`, because underlying are equal
  TypePtr underlying_variant = variant->unwrap_alias();
  for (TypePtr existing : out_unique_variants) {
    if (existing->equal_to(underlying_variant)) {
      return;
    }
  }

  out_unique_variants.push_back(variant);
}

bool TypeDataUnion::has_variant_equal_to(TypePtr rhs_type) const {
  for (TypePtr self_variant : variants) {
    if (self_variant->equal_to(rhs_type)) {
      return true;
    }
  }
  return false;
}

bool TypeDataUnion::has_all_variants_of(const TypeDataUnion* rhs_type) const {
  for (TypePtr rhs_variant : rhs_type->variants) {
    if (!has_variant_equal_to(rhs_variant)) {
      return false;
    }
  }
  return true;
}

int TypeDataUnion::get_variant_idx(TypePtr lookup_variant) const {
  for (int i = 0; i < size(); ++i) {
    if (variants[i]->equal_to(lookup_variant)) {
      return i;
    }
  }
  return -1;
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
    if (variant->equal_to(rhs_type)) {
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

} // namespace tolk
