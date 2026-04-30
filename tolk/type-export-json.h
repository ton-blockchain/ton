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
#include "type-system.h"
#include "json-output.h"
#include "constant-evaluator.h"
#include <string>
#include <vector>

namespace tolk {

// JsonTypeExporter gathers unique types and declarations (structs, aliases, enums)
// that are reachable from the compiled program. Used by ABI and symbol-types output.
struct JsonTypeExporter {
  struct UniqueType {
    TypePtr t_ptr;
    std::string abi_json;
  };

  std::vector<const Symbol*> used_symbols;
  std::vector<UniqueType> used_types;

  void register_used_type(TypePtr type);
  void register_used_symbol(const Symbol* symbol);

  int find_unique_type(TypePtr t) const;
  int get_type_idx(TypePtr t) const;

  void seed_primitive_types();

  struct EmitOptions {
    bool emit_ident_loc = false;            // "ident_loc": [file_id, start_line, start_col, end_line, end_col]
    bool emit_default_values = false;       // "default_value": ABIConstExpression
    bool emit_descriptions = false;         // "description": from doc comment
    bool use_abi_client_types = false;      // use `@abi.clientType` if exists, not field's declared_type
  };

  void emit_declarations_json(JsonPrettyOutput& json, const EmitOptions& opts) const;
  void emit_unique_ty_json(JsonPrettyOutput& json) const;
};

std::string get_abi_description(const DocCommentLines& doc_lines);

// shared to_json overloads used by both abi.cpp and source-maps.cpp via ADL
void to_json(JsonPrettyOutput& out, TypePtr type);
void to_json(JsonPrettyOutput& out, const ConstValExpression& v);

} // namespace tolk
