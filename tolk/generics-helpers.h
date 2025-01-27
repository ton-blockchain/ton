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

std::string generate_instantiated_name(const std::string& orig_name, const std::vector<TypePtr>& substitutions);
td::Result<std::vector<TypePtr>> deduce_substitutionTs_on_generic_func_call(const FunctionData* called_fun, std::vector<TypePtr>&& arg_types, TypePtr return_hint);
const FunctionData* instantiate_generic_function(SrcLocation loc, const FunctionData* fun_ref, const std::string& inst_name, std::vector<TypePtr>&& substitutionTs);

}  // namespace tolk
