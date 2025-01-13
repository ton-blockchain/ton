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

#include "src-file.h"
#include <cstdint>
#include <string>
#include <functional>

namespace tolk {

/*
 *   TypeData is both a user-given and an inferred type representation.
 *   `int`, `cell`, `T`, `(int, [tuple])` are instances of TypeData.
 *   Every unique TypeData is created only once, so for example TypeDataTensor::create(int, int)
 * returns one and the same pointer always. This "uniqueness" is called type_id, calculated before creation.
 *
 *   In Tolk code, types after colon `var v: (int, T)` are parsed to TypeData.
 *   See parse_type_from_tokens().
 *   So, AST nodes which can have declared types (local/global variables and others) store a pointer to TypeData.
 *
 *   Type inferring also creates TypeData for inferred expressions. All AST expression nodes have inferred_type.
 * For example, `1 + 2`, both operands are TypeDataInt, its result is also TypeDataInt.
 *   Type checking also uses TypeData. For example, `var i: slice = 1 + 2`, at first rhs (TypeDataInt) is inferred,
 * then lhs (TypeDataSlice from declaration) is checked whether rhs can be assigned.
 *   See can_rhs_be_assigned().
 *
 *   Note, that while initial parsing Tolk files to AST, known types (`int`, `cell`, etc.) are created as-is,
 * but user-defined types (`T`, `MyStruct`, `MyAlias`) are saved as TypeDataUnresolved.
 *   After all symbols have been registered, resolving identifiers step is executed, where particularly
 * all TypeDataUnresolved instances are converted to a resolved type. At inferring, no unresolved remain.
 *   For instance, `fun f<T>(v: T)`, at first "T" of `v` is unresolved, and then converted to TypeDataGenericT.
 */
class TypeData {
  // all unique types have unique type_id; it's used both for allocating memory once and for tagged unions
  const uint64_t type_id;
  // bits of flag_mask, to store often-used properties and return them without tree traversing
  const int flags;

  friend class TypeDataTypeIdCalculation;

protected:
  enum flag_mask {
    flag_contains_unknown_inside = 1 << 1,
    flag_contains_genericT_inside = 1 << 2,
    flag_contains_unresolved_inside = 1 << 3,
  };

  explicit TypeData(uint64_t type_id, int flags_with_children)
    : type_id(type_id)
    , flags(flags_with_children) {
  }

public:
  virtual ~TypeData() = default;

  template<class Derived>
  const Derived* try_as() const {
    return dynamic_cast<const Derived*>(this);
  }

  uint64_t get_type_id() const { return type_id; }

  bool has_unknown_inside() const { return flags & flag_contains_unknown_inside; }
  bool has_genericT_inside() const { return flags & flag_contains_genericT_inside; }
  bool has_unresolved_inside() const { return flags & flag_contains_unresolved_inside; }

  using TraverserCallbackT = std::function<void(TypePtr child)>;
  using ReplacerCallbackT = std::function<TypePtr(TypePtr child)>;

  virtual std::string as_human_readable() const = 0;
  virtual bool can_rhs_be_assigned(TypePtr rhs) const = 0;
  virtual bool can_be_casted_with_as_operator(TypePtr cast_to) const = 0;

  virtual void traverse(const TraverserCallbackT& callback) const {
    callback(this);
  }

  virtual TypePtr replace_children_custom(const ReplacerCallbackT& callback) const {
    return callback(this);
  }

  virtual int calc_width_on_stack() const {
    return 1;
  }

  virtual void extract_components(std::vector<TypePtr>& comp_types) const {
    comp_types.push_back(this);
  }
};

/*
 * `int` is TypeDataInt, representation of TVM int.
 */
class TypeDataInt final : public TypeData {
  TypeDataInt() : TypeData(1ULL, 0) {}

  static TypePtr singleton;
  friend void type_system_init();

public:
  static TypePtr create() { return singleton; }

  std::string as_human_readable() const override { return "int"; }
  bool can_rhs_be_assigned(TypePtr rhs) const override;
  bool can_be_casted_with_as_operator(TypePtr cast_to) const override;
};

/*
 * `bool` is TypeDataBool. TVM has no bool, only integers. Under the hood, -1 is true, 0 is false.
 * From the type system point of view, int and bool are different, not-autocastable types.
 */
class TypeDataBool final : public TypeData {
  TypeDataBool() : TypeData(2ULL, 0) {}

  static TypePtr singleton;
  friend void type_system_init();

public:
  static TypePtr create() { return singleton; }

  std::string as_human_readable() const override { return "bool"; }
  bool can_rhs_be_assigned(TypePtr rhs) const override;
  bool can_be_casted_with_as_operator(TypePtr cast_to) const override;
};

/*
 * `cell` is TypeDataCell, representation of TVM cell.
 */
class TypeDataCell final : public TypeData {
  TypeDataCell() : TypeData(3ULL, 0) {}

  static TypePtr singleton;
  friend void type_system_init();

public:
  static TypePtr create() { return singleton; }

  std::string as_human_readable() const override { return "cell"; }
  bool can_rhs_be_assigned(TypePtr rhs) const override;
  bool can_be_casted_with_as_operator(TypePtr cast_to) const override;
};

/*
 * `slice` is TypeDataSlice, representation of TVM slice.
 */
class TypeDataSlice final : public TypeData {
  TypeDataSlice() : TypeData(4ULL, 0) {}

  static TypePtr singleton;
  friend void type_system_init();

public:
  static TypePtr create() { return singleton; }

  std::string as_human_readable() const override { return "slice"; }
  bool can_rhs_be_assigned(TypePtr rhs) const override;
  bool can_be_casted_with_as_operator(TypePtr cast_to) const override;
};

/*
 * `builder` is TypeDataBuilder, representation of TVM builder.
 */
class TypeDataBuilder final : public TypeData {
  TypeDataBuilder() : TypeData(5ULL, 0) {}

  static TypePtr singleton;
  friend void type_system_init();

public:
  static TypePtr create() { return singleton; }

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
  TypeDataTuple() : TypeData(6ULL, 0) {}

  static TypePtr singleton;
  friend void type_system_init();

public:
  static TypePtr create() { return singleton; }

  std::string as_human_readable() const override { return "tuple"; }
  bool can_rhs_be_assigned(TypePtr rhs) const override;
  bool can_be_casted_with_as_operator(TypePtr cast_to) const override;
};

/*
 * `continuation` is TypeDataContinuation, representation of TVM continuation.
 * It's like "untyped callable", not compatible with other types.
 */
class TypeDataContinuation final : public TypeData {
  TypeDataContinuation() : TypeData(7ULL, 0) {}

  static TypePtr singleton;
  friend void type_system_init();

public:
  static TypePtr create() { return singleton; }

  std::string as_human_readable() const override { return "continuation"; }
  bool can_rhs_be_assigned(TypePtr rhs) const override;
  bool can_be_casted_with_as_operator(TypePtr cast_to) const override;
};

/*
 * `null` has TypeDataNullLiteral type.
 * Currently, it can be assigned to int/slice/etc., but later Tolk will have T? types and null safety.
 * Note, that `var i = null`, though valid (i would be constant null), fires an "always-null" compilation error
 * (it's much better for user to see an error here than when he passes this variable somewhere).
 */
class TypeDataNullLiteral final : public TypeData {
  TypeDataNullLiteral() : TypeData(8ULL, 0) {}

  static TypePtr singleton;
  friend void type_system_init();

public:
  static TypePtr create() { return singleton; }

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
  TypeDataFunCallable(uint64_t type_id, int children_flags, std::vector<TypePtr>&& params_types, TypePtr return_type)
    : TypeData(type_id, children_flags)
    , params_types(std::move(params_types))
    , return_type(return_type) {}

public:
  const std::vector<TypePtr> params_types;
  const TypePtr return_type;

  static TypePtr create(std::vector<TypePtr>&& params_types, TypePtr return_type);

  int params_size() const { return static_cast<int>(params_types.size()); }

  std::string as_human_readable() const override;
  bool can_rhs_be_assigned(TypePtr rhs) const override;
  bool can_be_casted_with_as_operator(TypePtr cast_to) const override;
  void traverse(const TraverserCallbackT& callback) const override;
  TypePtr replace_children_custom(const ReplacerCallbackT& callback) const override;
};

/*
 * `T` inside generic functions is TypeDataGenericT.
 * Example: `fun f<X,Y>(a: X, b: Y): [X, Y]` (here X and Y are).
 * On instantiation like `f(1,"")`, a new function `f<int,slice>` is created with type `fun(int,slice)->[int,slice]`.
 */
class TypeDataGenericT final : public TypeData {
  TypeDataGenericT(uint64_t type_id, std::string&& nameT)
    : TypeData(type_id, flag_contains_genericT_inside)
    , nameT(std::move(nameT)) {}

public:
  const std::string nameT;

  static TypePtr create(std::string&& nameT);

  std::string as_human_readable() const override { return nameT; }
  bool can_rhs_be_assigned(TypePtr rhs) const override;
  bool can_be_casted_with_as_operator(TypePtr cast_to) const override;
  int calc_width_on_stack() const override;
  void extract_components(std::vector<TypePtr>& comp_types) const override;
};

/*
 * `(int, slice)` is TypeDataTensor of 2 elements. Tensor of N elements occupies N stack slots.
 * Of course, there may be nested tensors, like `(int, (int, slice), cell)`.
 * Arguments, variables, globals, return values, etc. can be tensors.
 * A tensor can be empty.
 */
class TypeDataTensor final : public TypeData {
  TypeDataTensor(uint64_t type_id, int children_flags, std::vector<TypePtr>&& items)
    : TypeData(type_id, children_flags)
    , items(std::move(items)) {}

public:
  const std::vector<TypePtr> items;

  static TypePtr create(std::vector<TypePtr>&& items);

  int size() const { return static_cast<int>(items.size()); }

  std::string as_human_readable() const override;
  bool can_rhs_be_assigned(TypePtr rhs) const override;
  bool can_be_casted_with_as_operator(TypePtr cast_to) const override;
  void traverse(const TraverserCallbackT& callback) const override;
  TypePtr replace_children_custom(const ReplacerCallbackT& callback) const override;
  int calc_width_on_stack() const override;
  void extract_components(std::vector<TypePtr>& comp_types) const override;
};

/*
 * `[int, slice]` is TypeDataTypedTuple, a TVM 'tuple' under the hood, contained in 1 stack slot.
 * Unlike TypeDataTuple (untyped tuples), it has a predefined inner structure and can be assigned as
 * `var [i, cs] = [0, ""]`  (where a and b become two separate variables on a stack, int and slice).
 */
class TypeDataTypedTuple final : public TypeData {
  TypeDataTypedTuple(uint64_t type_id, int children_flags, std::vector<TypePtr>&& items)
    : TypeData(type_id, children_flags)
    , items(std::move(items)) {}

public:
  const std::vector<TypePtr> items;

  static TypePtr create(std::vector<TypePtr>&& items);

  int size() const { return static_cast<int>(items.size()); }

  std::string as_human_readable() const override;
  bool can_rhs_be_assigned(TypePtr rhs) const override;
  bool can_be_casted_with_as_operator(TypePtr cast_to) const override;
  void traverse(const TraverserCallbackT& callback) const override;
  TypePtr replace_children_custom(const ReplacerCallbackT& callback) const override;
};

/*
 * `unknown` is a special type, which can appear in corner cases.
 * The type of exception argument (which can hold any TVM value at runtime) is unknown.
 * The type of `_` used as rvalue is unknown.
 * The only thing available to do with unknown is to cast it: `catch (excNo, arg) { var i = arg as int; }`
 */
class TypeDataUnknown final : public TypeData {
  TypeDataUnknown() : TypeData(20ULL, flag_contains_unknown_inside) {}

  static TypePtr singleton;
  friend void type_system_init();

public:
  static TypePtr create() { return singleton; }

  std::string as_human_readable() const override { return "unknown"; }
  bool can_rhs_be_assigned(TypePtr rhs) const override;
  bool can_be_casted_with_as_operator(TypePtr cast_to) const override;
};

/*
 * "Unresolved" is not actually a type — it's an intermediate state between parsing and resolving.
 * At parsing to AST, unrecognized type names (MyEnum, MyStruct, T) are parsed as TypeDataUnresolved,
 * and after all source files parsed and global symbols registered, they are replaced by actual ones.
 * Example: `fun f<T>(v: T)` at first v is TypeDataUnresolved("T"), later becomes TypeDataGenericT.
 */
class TypeDataUnresolved final : public TypeData {
  TypeDataUnresolved(uint64_t type_id, std::string&& text, SrcLocation loc)
    : TypeData(type_id, flag_contains_unresolved_inside)
    , text(std::move(text))
    , loc(loc) {}

public:
  const std::string text;
  const SrcLocation loc;

  static TypePtr create(std::string&& text, SrcLocation loc);

  std::string as_human_readable() const override { return text + "*"; }
  bool can_rhs_be_assigned(TypePtr rhs) const override;
  bool can_be_casted_with_as_operator(TypePtr cast_to) const override;
  int calc_width_on_stack() const override;
  void extract_components(std::vector<TypePtr>& comp_types) const override;
};

/*
 * `void` is TypeDataVoid.
 * From the type system point of view, `void` functions return nothing.
 * Empty tensor is not compatible with void, although at IR level they are similar, 0 stack slots.
 */
class TypeDataVoid final : public TypeData {
  TypeDataVoid() : TypeData(10ULL, 0) {}

  static TypePtr singleton;
  friend void type_system_init();

public:
  static TypePtr create() { return singleton; }

  std::string as_human_readable() const override { return "void"; }
  bool can_rhs_be_assigned(TypePtr rhs) const override;
  bool can_be_casted_with_as_operator(TypePtr cast_to) const override;
  int calc_width_on_stack() const override;
  void extract_components(std::vector<TypePtr>& comp_types) const override;
};


// --------------------------------------------


class Lexer;
TypePtr parse_type_from_tokens(Lexer& lex);

void type_system_init();

} // namespace tolk
