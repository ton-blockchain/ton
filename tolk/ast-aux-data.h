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

#include "ast.h"
#include "lazy-helpers.h"

/*
 *   This file contains a schema of aux_data inside ast_artificial_aux_vertex
 * (it's a compiler-inserted vertex that can't occur in source code).
 */

namespace tolk {

struct CodeBlob;

// AuxData_LazyObjectLoadFields is a special auto-inserted vertex to load fields of a lazy struct;
// example: `var p = lazy Point.fromSlice(s); aux "load x"; return p.x`
struct AuxData_LazyObjectLoadFields final : ASTAuxData {
  LocalVarPtr var_ref;              // comes from `lazy`
  TypePtr union_variant;            // not just `o` but `match(o) { V1 => here }`
  StructFieldPtr field_ref;         // not just `o` but `match(o.field) { V1 => here }`
  LazyStructLoadInfo load_info;     // instructions, which fields to load, which to skip, etc.

  AuxData_LazyObjectLoadFields(LocalVarPtr var_ref, TypePtr union_variant, StructFieldPtr field_ref, LazyStructLoadInfo load_info)
    : var_ref(var_ref), union_variant(union_variant), field_ref(field_ref), load_info(std::move(load_info)) {
  }
};

// AuxData_LazyMatchForUnion wraps `match(lazy_var)` or its field
struct AuxData_LazyMatchForUnion final : ASTAuxData {
  LocalVarPtr var_ref;              // comes from `lazy`
  StructFieldPtr field_ref;         // not `match(o)`, but `match(o.field)`

  AuxData_LazyMatchForUnion(LocalVarPtr var_ref, StructFieldPtr field_ref)
    : var_ref(var_ref), field_ref(field_ref) {
  }
};

struct AuxData_OnInternalMessage_getField final : ASTAuxData {
  FunctionPtr f_onInternalMessage;
  const std::string_view field_name;

  AuxData_OnInternalMessage_getField(FunctionPtr f_onInternalMessage, std::string_view field_name)
    : f_onInternalMessage(f_onInternalMessage)
    , field_name(field_name) {
  }

  std::vector<var_idx_t> generate_get_InMessage_field(CodeBlob& code, AnyV origin) const;
};

} // namespace tolk
