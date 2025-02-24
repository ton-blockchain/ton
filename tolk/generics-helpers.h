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

  size_t size() const { return itemsT.size(); }
  bool has_nameT(std::string_view nameT) const { return find_nameT(nameT) != -1; }
  int find_nameT(std::string_view nameT) const;
  std::string get_nameT(int idx) const { return static_cast<std::string>(itemsT[idx].nameT); }
};

// when a function call is `f<int>()`, this "<int>" is represented as this class
struct GenericsInstantiation {
  const std::vector<TypePtr> substitutions;           // <SomeStruct, int> for genericTs <T1, T2>
  const SrcLocation loc;                              // first instantiation location

  explicit GenericsInstantiation(SrcLocation loc, std::vector<TypePtr>&& substitutions)
    : substitutions(std::move(substitutions))
    , loc(loc) {
  }
};

// this class helps to deduce Ts on the fly
// purpose: having `f<T>(value: T)` and call `f(5)`, deduce T = int
// while analyzing a call, arguments are handled one by one, by `auto_deduce_from_argument()`
// this class also handles manually specified substitutions like `f<int>(5)`
class GenericSubstitutionsDeduceForCall {
  FunctionPtr fun_ref;
  std::vector<TypePtr> substitutionTs;
  bool manually_specified = false;

  void provide_deducedT(const std::string& nameT, TypePtr deduced);
  void consider_next_condition(TypePtr param_type, TypePtr arg_type);

public:
  explicit GenericSubstitutionsDeduceForCall(FunctionPtr fun_ref);

  bool is_manually_specified() const {
    return manually_specified;
  }

  void provide_manually_specified(std::vector<TypePtr>&& substitutionTs);
  TypePtr replace_by_manually_specified(TypePtr param_type) const;
  TypePtr auto_deduce_from_argument(FunctionPtr cur_f, SrcLocation loc, TypePtr param_type, TypePtr arg_type);
  int get_first_not_deduced_idx() const;

  std::vector<TypePtr>&& flush() {
    return std::move(substitutionTs);
  }
};

struct GenericDeduceError final : std::exception {
  std::string message;
  explicit GenericDeduceError(std::string message)
    : message(std::move(message)) { }

  const char* what() const noexcept override {
    return message.c_str();
  }
};

std::string generate_instantiated_name(const std::string& orig_name, const std::vector<TypePtr>& substitutions);
FunctionPtr instantiate_generic_function(SrcLocation loc, FunctionPtr fun_ref, const std::string& inst_name, std::vector<TypePtr>&& substitutionTs);

}  // namespace tolk
