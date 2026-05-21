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
//
// Each unique type maps to a flat JSON object whose nested types are referenced by
// `ty_idx` (indices into used_types).
//
// For generic instantiations, `abi_json` stores only the "head" —
// { kind, name, type_args_ty_idx }. The resolved fields/target are kept in
// separate parallel containers `struct_instantiations` / `alias_instantiations`
// and emitted as distinct top-level JSON arrays.
struct JsonTypeExporter {
  struct UniqueType {
    TypePtr t_ptr;
    std::string abi_json;
  };

  // One entry per generic-struct instantiation, built alongside used_types.
  // Needed to provide compiler-calculated data (stack_width, etc.) that can not be reconstructed on a client-side.
  struct StructInstantiation {
    int ty_idx;
    StructPtr struct_ref;
    std::vector<int> monomorphic_fields_ty_idx;
  };

  // One entry per generic-alias instantiation, built alongside used_types.
  // Needed to provide compiler-calculated data (stack_width, etc.) that can not be reconstructed on a client-side.
  struct AliasInstantiation {
    int ty_idx;
    AliasDefPtr alias_ref;
    int monomorphic_target_ty_idx;
  };

  // Needed to handle `ConstValCastToType` to output `cast_to_ty_idx` from a registry.
  struct ConstValJson {
    const ConstValExpression& value;
    const JsonTypeExporter& registry;
  };

  std::vector<const Symbol*> used_symbols;
  std::vector<UniqueType> used_types;
  std::vector<StructInstantiation> struct_instantiations;
  std::vector<AliasInstantiation> alias_instantiations;

  int register_used_type(TypePtr type);
  bool register_used_symbol(const Symbol* symbol);
  void register_used_const_val(const ConstValExpression& v);

  int find_unique_type(TypePtr t) const;
  int get_type_idx(TypePtr t) const;
  ConstValJson const_val_json(const ConstValExpression& v) const { return ConstValJson{v, *this}; }

  void seed_primitive_types();

  struct EmitOptions {
    bool emit_ident_loc = false;            // "ident_loc": [file_id, start_line, start_col, end_line, end_col]
    bool emit_default_values = false;       // "default_value": ABIConstExpression
    bool emit_descriptions = false;         // "description": from doc comment
    bool emit_abi_client_types = false;     // emit optional "client_ty_idx" for `@abi.clientType`
  };

  void emit_unique_ty_and_declarations_json(JsonPrettyOutput& json, const EmitOptions& opts) const;
};

std::string get_abi_description(const DocCommentLines& doc_lines);

// shared to_json overloads used by ABI/symbol-types emitters via ADL
void to_json(JsonPrettyOutput& json, const JsonTypeExporter::ConstValJson& v);

} // namespace tolk
