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
#include <functional>
#include <string>
#include <vector>

namespace tolk {

/*
 *   TypeData is both a user-given and an inferred type representation.
 *   `int`, `cell`, `T`, `(int, [tuple])` are instances of TypeData.
 *   Every unique TypeData is created only once, so for example TypeDataTensor::create(int, int)
 * returns one and the same pointer always.
 *
 *   Type inferring creates TypeData for inferred expressions. All AST expression nodes have inferred_type.
 * For example, `1 + 2`, both operands are TypeDataInt, its result is also TypeDataInt.
 *   Type checking also uses TypeData. For example, `var i: slice = 1 + 2`, at first rhs (TypeDataInt) is inferred,
 * then lhs (TypeDataSlice from declaration) is checked whether rhs can be assigned.
 *   See can_rhs_be_assigned().
 *
 *   At the moment of parsing, types after colon `var v: (int, T)` are parsed to AST (AnyTypeV),
 * and all symbols have been registered, AST representation resolved to TypeData, see pipe-resolve-types.cpp.
 */
class TypeData {
  // bits of flag_mask, to store often-used properties and return them without tree traversing
  const int flags;
  // how many slots on a stack this type occupies (calculated on creation), e.g. `int`=1, `(int,int)`=2, `(int,int)?`=3
  const int width_on_stack;

  friend class TypeDataHasherForUnique;

protected:
  enum flag_mask {
    flag_contains_unknown_inside = 1 << 1,
    flag_contains_genericT_inside = 1 << 2,
    flag_contains_type_alias_inside = 1 << 3,
  };

  explicit TypeData(int flags_with_children, int width_on_stack)
    : flags(flags_with_children)
    , width_on_stack(width_on_stack) {
  }

  static bool equal_to_slow_path(TypePtr lhs, TypePtr rhs);
  static TypePtr unwrap_alias_slow_path(TypePtr lhs);

public:
  virtual ~TypeData() = default;

  template<class Derived>
  const Derived* try_as() const {
    return dynamic_cast<const Derived*>(this);
  }

  int get_width_on_stack() const { return width_on_stack; }

  bool equal_to(TypePtr rhs) const {
    return this == rhs || equal_to_slow_path(this, rhs);
  }
  TypePtr unwrap_alias() const {
    return has_type_alias_inside() ? unwrap_alias_slow_path(this) : this;
  }

  bool has_unknown_inside() const { return flags & flag_contains_unknown_inside; }
  bool has_genericT_inside() const { return flags & flag_contains_genericT_inside; }
  bool has_type_alias_inside() const { return flags & flag_contains_type_alias_inside; }

  using ReplacerCallbackT = std::function<TypePtr(TypePtr child)>;

  virtual int get_type_id() const = 0;
  virtual std::string as_human_readable() const = 0;
  virtual bool can_rhs_be_assigned(TypePtr rhs) const = 0;
  virtual bool can_be_casted_with_as_operator(TypePtr cast_to) const = 0;

  virtual bool can_hold_tvm_null_instead() const {
    return true;
  }

  virtual TypePtr replace_children_custom(const ReplacerCallbackT& callback) const {
    return callback(this);
  }
};

/*
 * `type AliasName = underlying_type` is an alias, which is fully interchangeable with its original type.
 * It never occurs at runtime: at IR generation it's erased, replaced by an underlying type.
 * But until IR generation, aliases exists, and `var t: MyTensor2 = (1,2)` is alias "MyTensor", not tensor (int,int).
 * That's why lots of code comparing types use `type->unwrap_alias()` or `try_as<TypeDataAlias>`.
 * Note, that generic aliases, when instantiated, are inserted into in symtable (like structs and functions),
 * so for `WrapperAlias<T>` alias_ref points to a generic alias, and for `WrapperAlias<int>` to an instantiated one.
 */
class TypeDataAlias final : public TypeData {
  explicit TypeDataAlias(int children_flags, AliasDefPtr alias_ref, TypePtr underlying_type)
    : TypeData(children_flags | flag_contains_type_alias_inside, underlying_type->get_width_on_stack())
    , alias_ref(alias_ref)
    , underlying_type(underlying_type) {}

public:
  const AliasDefPtr alias_ref;
  const TypePtr underlying_type;

  static TypePtr create(AliasDefPtr alias_ref);

  int get_type_id() const override;
  std::string as_human_readable() const override;
  bool can_rhs_be_assigned(TypePtr rhs) const override;
  bool can_be_casted_with_as_operator(TypePtr cast_to) const override;
  bool can_hold_tvm_null_instead() const override;
};

/*
 * `int` is TypeDataInt, representation of TVM int.
 */
class TypeDataInt final : public TypeData {
  TypeDataInt() : TypeData(0, 1) {}

  static TypePtr singleton;
  friend void type_system_init();

public:
  static TypePtr create() { return singleton; }

  int get_type_id() const override { return 1; }
  std::string as_human_readable() const override { return "int"; }
  bool can_rhs_be_assigned(TypePtr rhs) const override;
  bool can_be_casted_with_as_operator(TypePtr cast_to) const override;
};

/*
 * `bool` is TypeDataBool. TVM has no bool, only integers. Under the hood, -1 is true, 0 is false.
 * From the type system point of view, int and bool are different, not-autocastable types.
 */
class TypeDataBool final : public TypeData {
  TypeDataBool() : TypeData(0, 1) {}

  static TypePtr singleton;
  friend void type_system_init();

public:
  static TypePtr create() { return singleton; }

  int get_type_id() const override { return 2; }
  std::string as_human_readable() const override { return "bool"; }
  bool can_rhs_be_assigned(TypePtr rhs) const override;
  bool can_be_casted_with_as_operator(TypePtr cast_to) const override;
};

/*
 * `cell` is TypeDataCell, representation of TVM cell.
 */
class TypeDataCell final : public TypeData {
  TypeDataCell() : TypeData(0, 1) {}

  static TypePtr singleton;
  friend void type_system_init();

public:
  static TypePtr create() { return singleton; }

  int get_type_id() const override { return 3; }
  std::string as_human_readable() const override { return "cell"; }
  bool can_rhs_be_assigned(TypePtr rhs) const override;
  bool can_be_casted_with_as_operator(TypePtr cast_to) const override;
};

/*
 * `slice` is TypeDataSlice, representation of TVM slice.
 */
class TypeDataSlice final : public TypeData {
  TypeDataSlice() : TypeData(0, 1) {}

  static TypePtr singleton;
  friend void type_system_init();

public:
  static TypePtr create() { return singleton; }

  int get_type_id() const override { return 4; }
  std::string as_human_readable() const override { return "slice"; }
  bool can_rhs_be_assigned(TypePtr rhs) const override;
  bool can_be_casted_with_as_operator(TypePtr cast_to) const override;
};

/*
 * `builder` is TypeDataBuilder, representation of TVM builder.
 */
class TypeDataBuilder final : public TypeData {
  TypeDataBuilder() : TypeData(0, 1) {}

  static TypePtr singleton;
  friend void type_system_init();

public:
  static TypePtr create() { return singleton; }

  int get_type_id() const override { return 5; }
  std::string as_human_readable() const override { return "builder"; }
  bool can_rhs_be_assigned(TypePtr rhs) const override;
  bool can_be_casted_with_as_operator(TypePtr cast_to) const override;
};

/*
 * `tuple` is TypeDataTuple, representation of TVM tuple.
 * Note, that it's UNTYPED tuple. It occupies 1 stack slot in TVM. Its elements are any TVM values at runtime,
 * so getting its element results in TypeDataUnknown (which must be assigned/cast explicitly).
 */
class TypeDataTuple final : public TypeData {
  TypeDataTuple() : TypeData(0, 1) {}

  static TypePtr singleton;
  friend void type_system_init();

public:
  static TypePtr create() { return singleton; }

  int get_type_id() const override { return 6; }
  std::string as_human_readable() const override { return "tuple"; }
  bool can_rhs_be_assigned(TypePtr rhs) const override;
  bool can_be_casted_with_as_operator(TypePtr cast_to) const override;
};

/*
 * `continuation` is TypeDataContinuation, representation of TVM continuation.
 * It's like "untyped callable", not compatible with other types.
 */
class TypeDataContinuation final : public TypeData {
  TypeDataContinuation() : TypeData(0, 1) {}

  static TypePtr singleton;
  friend void type_system_init();

public:
  static TypePtr create() { return singleton; }

  int get_type_id() const override { return 7; }
  std::string as_human_readable() const override { return "continuation"; }
  bool can_rhs_be_assigned(TypePtr rhs) const override;
  bool can_be_casted_with_as_operator(TypePtr cast_to) const override;
};

/*
 * `null` has TypeDataNullLiteral type.
 * It can be assigned only to nullable types (`int?`, etc.), to ensure null safety.
 * Note, that `var i = null`, though valid (i would be constant null), fires an "always-null" compilation error
 * (it's much better for user to see an error here than when he passes this variable somewhere).
 */
class TypeDataNullLiteral final : public TypeData {
  TypeDataNullLiteral() : TypeData(0, 1) {}

  static TypePtr singleton;
  friend void type_system_init();

public:
  static TypePtr create() { return singleton; }

  int get_type_id() const override { return 0; }
  std::string as_human_readable() const override { return "null"; }
  bool can_rhs_be_assigned(TypePtr rhs) const override;
  bool can_be_casted_with_as_operator(TypePtr cast_to) const override;
};

/*
 * `fun(int, int) -> void` is TypeDataFunCallable, think of is as a typed continuation.
 * A type of function `fun f(x: int) { return x; }` is actually `fun(int) -> int`.
 * So, when assigning it to a variable `var cb = f`, this variable also has this type.
 */
class TypeDataFunCallable final : public TypeData {
  TypeDataFunCallable(int children_flags, std::vector<TypePtr>&& params_types, TypePtr return_type)
    : TypeData(children_flags, 1)
    , params_types(std::move(params_types))
    , return_type(return_type) {}

public:
  const std::vector<TypePtr> params_types;
  const TypePtr return_type;

  static TypePtr create(std::vector<TypePtr>&& params_types, TypePtr return_type);

  int params_size() const { return static_cast<int>(params_types.size()); }

  int get_type_id() const override;
  std::string as_human_readable() const override;
  bool can_rhs_be_assigned(TypePtr rhs) const override;
  bool can_be_casted_with_as_operator(TypePtr cast_to) const override;
  TypePtr replace_children_custom(const ReplacerCallbackT& callback) const override;
};

/*
 * `T` inside generic functions and structs is TypeDataGenericT.
 * Example: `fun f<X,Y>(a: X, b: Y): [X, Y]` (here X and Y are).
 * Example: `struct Wrapper<T> { value: T }` (type of field is generic T).
 * On instantiation like `f(1,"")`, a new function `f<int,slice>` is created with type `fun(int,slice)->[int,slice]`.
 */
class TypeDataGenericT final : public TypeData {
  explicit TypeDataGenericT(std::string&& nameT)
    : TypeData(flag_contains_genericT_inside, -999999)  // width undefined until instantiated
    , nameT(std::move(nameT)) {}

public:
  const std::string nameT;

  static TypePtr create(std::string&& nameT);

  int get_type_id() const override;
  std::string as_human_readable() const override { return nameT; }
  bool can_rhs_be_assigned(TypePtr rhs) const override;
  bool can_be_casted_with_as_operator(TypePtr cast_to) const override;
};

/*
 * `Wrapper<T>` when T is a generic (a struct is not ready to instantiate) is TypeDataGenericTypeWithTs.
 * `Wrapper<int>` is NOT here, it's an instantiated struct. Here is only when type arguments contain generics.
 * Example: `type WrapperAlias<T> = Wrapper<T>`, then `Wrapper<T>` (underlying type of alias) is here.
 * Since structs and type aliases both can be generic, either struct_ref of alias_ref is filled.
 */
class TypeDataGenericTypeWithTs final : public TypeData {
  TypeDataGenericTypeWithTs(int children_flags, StructPtr struct_ref, AliasDefPtr alias_ref, std::vector<TypePtr>&& type_arguments)
    : TypeData(children_flags, -999999)
    , struct_ref(struct_ref)
    , alias_ref(alias_ref)
    , type_arguments(std::move(type_arguments)) {}

public:
  const StructPtr struct_ref;                 // for `Wrapper<T>`, then alias_ref = nullptr
  const AliasDefPtr alias_ref;                // for `PairAlias<int, T2>`, then struct_ref = nullptr
  const std::vector<TypePtr> type_arguments;  // `<T>`, `<int, T2>`, at least one of them contains generic T

  static TypePtr create(StructPtr struct_ref, AliasDefPtr alias_ref, std::vector<TypePtr>&& type_arguments);

  int size() const { return static_cast<int>(type_arguments.size()); }

  int get_type_id() const override;
  std::string as_human_readable() const override;
  bool can_rhs_be_assigned(TypePtr rhs) const override;
  bool can_be_casted_with_as_operator(TypePtr cast_to) const override;
  TypePtr replace_children_custom(const ReplacerCallbackT& callback) const override;
};

/*
 * `A`, `User`, `SomeStruct`, `Wrapper<int>` is TypeDataStruct. At TVM level, structs are tensors.
 * In the code, creating a struct is either `var v: A = { ... }` (by hint) or `var v = A { ... }`.
 * Fields of a struct have their own types (accessed by struct_ref).
 * Note, that instantiated structs like "Wrapper<int>" exist in symtable (like aliases and functions),
 * so for `Wrapper<T>` struct_ref points to a generic struct, and for `Wrapper<int>` to an instantiated one.
 */
class TypeDataStruct final : public TypeData {
  TypeDataStruct(int width_on_stack, StructPtr struct_ref)
    : TypeData(0, width_on_stack)
    , struct_ref(struct_ref) {}

public:
  StructPtr struct_ref;

  static TypePtr create(StructPtr struct_ref);

  int get_type_id() const override;
  std::string as_human_readable() const override;
  bool can_rhs_be_assigned(TypePtr rhs) const override;
  bool can_be_casted_with_as_operator(TypePtr cast_to) const override;
  bool can_hold_tvm_null_instead() const override;
};

/*
 * `(int, slice)` is TypeDataTensor of 2 elements. Tensor of N elements occupies N stack slots.
 * Of course, there may be nested tensors, like `(int, (int, slice), cell)`.
 * Arguments, variables, globals, return values, etc. can be tensors.
 * A tensor can be empty.
 */
class TypeDataTensor final : public TypeData {
  TypeDataTensor(int children_flags, int width_on_stack, std::vector<TypePtr>&& items)
    : TypeData(children_flags, width_on_stack)
    , items(std::move(items)) {}

public:
  const std::vector<TypePtr> items;

  static TypePtr create(std::vector<TypePtr>&& items);

  int size() const { return static_cast<int>(items.size()); }

  int get_type_id() const override;
  std::string as_human_readable() const override;
  bool can_rhs_be_assigned(TypePtr rhs) const override;
  bool can_be_casted_with_as_operator(TypePtr cast_to) const override;
  TypePtr replace_children_custom(const ReplacerCallbackT& callback) const override;
  bool can_hold_tvm_null_instead() const override;
};

/*
 * `[int, slice]` is TypeDataBrackets, a TVM 'tuple' under the hood, contained in 1 stack slot.
 * Unlike TypeDataTuple (untyped tuples), it has a predefined inner structure and can be assigned as
 * `var [i, cs] = [0, ""]`  (where i and cs become two separate variables on a stack, int and slice).
 */
class TypeDataBrackets final : public TypeData {
  TypeDataBrackets(int children_flags, std::vector<TypePtr>&& items)
    : TypeData(children_flags, 1)
    , items(std::move(items)) {}

public:
  const std::vector<TypePtr> items;

  static TypePtr create(std::vector<TypePtr>&& items);

  int size() const { return static_cast<int>(items.size()); }

  int get_type_id() const override;
  std::string as_human_readable() const override;
  bool can_rhs_be_assigned(TypePtr rhs) const override;
  bool can_be_casted_with_as_operator(TypePtr cast_to) const override;
  TypePtr replace_children_custom(const ReplacerCallbackT& callback) const override;
};

/*
 * `int8`, `int32`, `uint1`, `uint257`, `varint16` are TypeDataIntN. At TVM level, it's just int.
 * The purpose of intN is to be used in struct fields, describing the way of serialization (n bits).
 * A field `value: int32` has the TYPE `int32`, so being assigned to a variable, that variable is also `int32`.
 * intN is smoothly cast from/to plain int, mathematical operators on intN also "fall back" to general int.
 */
class TypeDataIntN final : public TypeData {
  TypeDataIntN(bool is_unsigned, bool is_variadic, int n_bits)
    : TypeData(0, 1)
    , is_unsigned(is_unsigned)
    , is_variadic(is_variadic)
    , n_bits(n_bits) {}

public:
  const bool is_unsigned;
  const bool is_variadic;
  const int n_bits;

  static TypePtr create(bool is_unsigned, bool is_variadic, int n_bits);

  int get_type_id() const override;
  std::string as_human_readable() const override;
  bool can_rhs_be_assigned(TypePtr rhs) const override;
  bool can_be_casted_with_as_operator(TypePtr cast_to) const override;
};

/*
 * `coins` is just integer at TVM level, but encoded as varint when serializing structures.
 * Example: `var cost = ton("0.05")` has type `coins`.
 */
class TypeDataCoins final : public TypeData {
  TypeDataCoins() : TypeData(0, 1) {}

  static TypePtr singleton;
  friend void type_system_init();

public:
  static TypePtr create() { return singleton; }

  int get_type_id() const override { return 17; }
  std::string as_human_readable() const override { return "coins"; }
  bool can_rhs_be_assigned(TypePtr rhs) const override;
  bool can_be_casted_with_as_operator(TypePtr cast_to) const override;
};

/*
 * `bytes256`, `bits512`, `bytes8` are TypeDataBytesN. At TVM level, it's just slice.
 * The purpose of bytesN is to be used in struct fields, describing the way of serialization (n bytes / n bits).
 * In this essence, bytesN is very similar to intN.
 * Note, that unlike intN automatically cast to/from int, bytesN does NOT auto cast to slice (without `as`).
 */
class TypeDataBytesN final : public TypeData {
  TypeDataBytesN(bool is_bits, int n_width)
    : TypeData(0, 1)
    , is_bits(is_bits)
    , n_width(n_width) {}

public:
  const bool is_bits;
  const int n_width;

  static TypePtr create(bool is_bits, int n_width);

  int get_type_id() const override;
  std::string as_human_readable() const override;
  bool can_rhs_be_assigned(TypePtr rhs) const override;
  bool can_be_casted_with_as_operator(TypePtr cast_to) const override;
};

/*
 * `T1 | T2 | ...` is a union type. Unions are supported in Tolk, similar to TypeScript, stored like enums in Rust.
 * `T | null` (denoted as `T?`) is also a union from a type system point of view.
 * There is no TypeDataNullable, because mixing unions and nullables would result in a mess.
 * At TVM level:
 * - `T | null`, if T is 1 slot  (like `int | null`), then it's still 1 slot
 * - `T | null`, if T is N slots (like `(int, int)?`), it's stored as N+1 slots (the last for type_id if T or 0 if null)
 * - `T1 | T2 | ...` is a tagged union: occupy max(T_i)+1 slots (1 for type_id)
 */
class TypeDataUnion final : public TypeData {
  TypeDataUnion(int children_flags, int width_on_stack, TypePtr or_null, std::vector<TypePtr>&& variants)
    : TypeData(children_flags, width_on_stack)
    , or_null(or_null)
    , variants(std::move(variants)) {}

  bool has_variant_with_type_id(int type_id) const;
  static void append_union_type_variant(TypePtr variant, std::vector<TypePtr>& out_unique_variants);

public:
  const TypePtr or_null;                  // if `T | null`, then T is here (variants = [T, null] then); otherwise, nullptr
  const std::vector<TypePtr> variants;    // T_i, flattened, no duplicates; may include aliases, but not other unions

  static TypePtr create(std::vector<TypePtr>&& variants);
  static TypePtr create_nullable(TypePtr nullable);

  int size() const { return static_cast<int>(variants.size()); }

  // "primitive nullable" is `T?` which holds TVM NULL in the same slot (it other words, has no UTag slot)
  // true : `int?`, `slice?`, `StructWith1IntField?`
  // false: `(int, int)?`, `ComplexStruct?`, `()?`
  bool is_primitive_nullable() const {
    return get_width_on_stack() == 1 && or_null != nullptr && or_null->get_width_on_stack() == 1;
  }
  bool has_null() const {
    if (or_null) {
      return true;
    }
    return has_variant_with_type_id(0);
  }
  bool has_variant_with_type_id(TypePtr rhs_type) const {
    int type_id = rhs_type->get_type_id();
    if (or_null) {
      return type_id == 0 || type_id == or_null->get_type_id();
    }
    return has_variant_with_type_id(type_id);
  }

  TypePtr calculate_exact_variant_to_fit_rhs(TypePtr rhs_type) const;
  bool has_all_variants_of(const TypeDataUnion* rhs_type) const;

  int get_type_id() const override;
  std::string as_human_readable() const override;
  bool can_rhs_be_assigned(TypePtr rhs) const override;
  bool can_be_casted_with_as_operator(TypePtr cast_to) const override;
  TypePtr replace_children_custom(const ReplacerCallbackT& callback) const override;
  bool can_hold_tvm_null_instead() const override;
};

/*
 * `unknown` is a special type, which can appear in corner cases.
 * The type of exception argument (which can hold any TVM value at runtime) is unknown.
 * The type of `_` used as rvalue is unknown.
 * The only thing available to do with unknown is to cast it: `catch (excNo, arg) { var i = arg as int; }`
 */
class TypeDataUnknown final : public TypeData {
  TypeDataUnknown() : TypeData(flag_contains_unknown_inside, 1) {}

  static TypePtr singleton;
  friend void type_system_init();

public:
  static TypePtr create() { return singleton; }

  int get_type_id() const override;
  std::string as_human_readable() const override { return "unknown"; }
  bool can_rhs_be_assigned(TypePtr rhs) const override;
  bool can_be_casted_with_as_operator(TypePtr cast_to) const override;
};

/*
 * `never` is a special type meaning "no value can be hold".
 * Is may appear due to smart casts, for example `if (x == null && x != null)` makes x "never".
 * Functions returning "never" assume to never exit, calling them interrupts control flow.
 * Such variables can not be cast to any other types, all their usage will trigger type mismatch errors.
 */
class TypeDataNever final : public TypeData {
  TypeDataNever() : TypeData(0, 0) {}

  static TypePtr singleton;
  friend void type_system_init();

public:
  static TypePtr create() { return singleton; }

  int get_type_id() const override { return 19; }
  std::string as_human_readable() const override { return "never"; }
  bool can_rhs_be_assigned(TypePtr rhs) const override;
  bool can_be_casted_with_as_operator(TypePtr cast_to) const override;
  bool can_hold_tvm_null_instead() const override;
};

/*
 * `void` is TypeDataVoid.
 * From the type system point of view, `void` functions return nothing.
 * Empty tensor is not compatible with void, although at IR level they are similar, 0 stack slots.
 */
class TypeDataVoid final : public TypeData {
  TypeDataVoid() : TypeData(0, 0) {}

  static TypePtr singleton;
  friend void type_system_init();

public:
  static TypePtr create() { return singleton; }

  int get_type_id() const override { return 10; }
  std::string as_human_readable() const override { return "void"; }
  bool can_rhs_be_assigned(TypePtr rhs) const override;
  bool can_be_casted_with_as_operator(TypePtr cast_to) const override;
  bool can_hold_tvm_null_instead() const override;
};


// --------------------------------------------

void type_system_init();

} // namespace tolk
