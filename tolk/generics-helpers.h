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
// (not at AST, but at symbol storage level)
struct GenericsDeclaration {
  struct GenericsItem {
    std::string_view nameT;

    explicit GenericsItem(std::string_view nameT)
      : nameT(nameT) {}
  };

  explicit GenericsDeclaration(std::vector<GenericsItem>&& itemsT)
    : itemsT(std::move(itemsT)) {}

  const std::vector<GenericsItem> itemsT;

  std::string as_human_readable() const;

  int size() const { return static_cast<int>(itemsT.size()); }
  int find_nameT(std::string_view nameT) const;
  std::string_view get_nameT(int idx) const { return itemsT[idx].nameT; }
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

  std::string as_human_readable() const;

  int size() const { return static_cast<int>(valuesTs.size()); }
  bool has_nameT(std::string_view nameT) const;
  TypePtr get_substitution_for_nameT(std::string_view nameT) const;
  std::string_view nameT_at(int idx) const { return genericTs->get_nameT(idx); }
  TypePtr typeT_at(int idx) const { return valuesTs.at(idx); }
  bool equal_to(const GenericsSubstitutions* rhs) const;

  void set_typeT(std::string_view nameT, TypePtr typeT);
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
  TypePtr auto_deduce_from_argument(FunctionPtr cur_f, SrcLocation loc, TypePtr param_type, TypePtr arg_type);
  std::string_view get_first_not_deduced_nameT() const;
  void fire_error_can_not_deduce(FunctionPtr cur_f, SrcLocation loc, std::string_view nameT) const;

  GenericsSubstitutions&& flush() {
    return std::move(deducedTs);
  }
};

struct GenericDeduceError final : std::exception {
  std::string nameT;

  explicit GenericDeduceError(std::string_view nameT)
    : nameT(nameT) { }

  const char* what() const noexcept override {
    return nameT.c_str();
  }
};

FunctionPtr instantiate_generic_function(FunctionPtr fun_ref, GenericsSubstitutions&& substitutedTs);
StructPtr instantiate_generic_struct(StructPtr struct_ref, GenericsSubstitutions&& substitutedTs);
AliasDefPtr instantiate_generic_alias(AliasDefPtr alias_ref, GenericsSubstitutions&& substitutedTs);

}  // namespace tolk
