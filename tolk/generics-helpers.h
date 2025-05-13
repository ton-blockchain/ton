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
#include "fwd-declarations.h"
#include "td/utils/Status.h"
#include <vector>

namespace tolk {

// when a function is declared `f<T>`, this "<T>" is represented as this class
// - `fun tuple.push<T>(self, value:T)`  itemsT = [T]
// - `struct Pair<A,B>`                  itemsT = [A,B]
// - `fun Container<T>.compareWith<U>`   itemsT = [T,U], n_from_receiver = 1
// - `fun Pair<int,int>.createFrom<U,V>` itemsT = [U,V]
// - `fun Pair<A,B>.create`              itemsT = [A,B], n_from_receiver = 2
// - `struct Opts<T=never>`              itemsT = [T default_type=never]
struct GenericsDeclaration {
  struct ItemT {
    std::string_view nameT;
    TypePtr default_type;       // exists for `<T = int>`, nullptr otherwise

    ItemT(std::string_view nameT, TypePtr default_type)
      : nameT(nameT), default_type(default_type) {}
  };

  explicit GenericsDeclaration(std::vector<ItemT>&& itemsT, int n_from_receiver)
    : itemsT(std::move(itemsT))
    , n_from_receiver(n_from_receiver) {}

  const std::vector<ItemT> itemsT;
  const int n_from_receiver;

  std::string as_human_readable(bool include_from_receiver = false) const;

  int size() const { return static_cast<int>(itemsT.size()); }
  int find_nameT(std::string_view nameT) const;
  std::string_view get_nameT(int idx) const { return itemsT[idx].nameT; }
  TypePtr get_defaultT(int idx) const { return itemsT[idx].default_type; }
};

// when a function call is `f<int>()`, this "<int>" is represented as this class
// same for `Wrapper<slice>`, "<slice>" is substitution
struct GenericsSubstitutions {
private:
  const GenericsDeclaration* genericTs;   // [T1, T2]
  std::vector<TypePtr> valuesTs;          // [SomeStruct, int]

public:
  explicit GenericsSubstitutions(const GenericsDeclaration* genericTs)
    : genericTs(genericTs)
    , valuesTs(genericTs == nullptr ? 0 : genericTs->size()) {
  }
  explicit GenericsSubstitutions(const GenericsDeclaration* genericTs, const std::vector<TypePtr>& type_arguments);

  std::string as_human_readable(bool show_nullptr) const;

  int size() const { return static_cast<int>(valuesTs.size()); }
  bool has_nameT(std::string_view nameT) const;
  TypePtr get_substitution_for_nameT(std::string_view nameT) const;
  TypePtr get_default_for_nameT(std::string_view nameT) const;
  std::string_view nameT_at(int idx) const { return genericTs->get_nameT(idx); }
  TypePtr typeT_at(int idx) const { return valuesTs.at(idx); }
  bool equal_to(const GenericsSubstitutions* rhs) const;

  void set_typeT(std::string_view nameT, TypePtr typeT);
  void provide_type_arguments(const std::vector<TypePtr>& type_arguments);
  void rewrite_missing_with_defaults();
};

// this class helps to deduce Ts on the fly
// purpose: having `f<T>(value: T)` and call `f(5)`, deduce T = int
// while analyzing a call, arguments are handled one by one, by `auto_deduce_from_argument()`
// note, that manually specified substitutions like `f<int>(5)` are NOT handled by this class, it's not deducing
class GenericSubstitutionsDeducing {
  FunctionPtr fun_ref;
  StructPtr struct_ref;
  GenericsSubstitutions deducedTs;

  void consider_next_condition(TypePtr param_type, TypePtr arg_type);

public:
  explicit GenericSubstitutionsDeducing(FunctionPtr fun_ref);
  explicit GenericSubstitutionsDeducing(StructPtr struct_ref);

  TypePtr replace_Ts_with_currently_deduced(TypePtr orig) const;
  TypePtr auto_deduce_from_argument(TypePtr param_type, TypePtr arg_type);
  TypePtr auto_deduce_from_argument(FunctionPtr cur_f, SrcLocation loc, TypePtr param_type, TypePtr arg_type);
  std::string_view get_first_not_deduced_nameT() const;
  void apply_defaults_from_declaration();
  void fire_error_can_not_deduce(FunctionPtr cur_f, SrcLocation loc, std::string_view nameT) const;

  GenericsSubstitutions&& flush() {
    return std::move(deducedTs);
  }
};

typedef std::pair<FunctionPtr, GenericsSubstitutions> MethodCallCandidate;

FunctionPtr instantiate_generic_function(FunctionPtr fun_ref, GenericsSubstitutions&& substitutedTs);
StructPtr instantiate_generic_struct(StructPtr struct_ref, GenericsSubstitutions&& substitutedTs);
AliasDefPtr instantiate_generic_alias(AliasDefPtr alias_ref, GenericsSubstitutions&& substitutedTs);

FunctionPtr match_exact_method_for_call_not_generic(TypePtr called_receiver, std::string_view called_name);
std::vector<MethodCallCandidate> match_methods_for_call_including_generic(TypePtr called_receiver, std::string_view called_name);

}  // namespace tolk
