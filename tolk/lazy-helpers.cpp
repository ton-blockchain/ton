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
#include "lazy-helpers.h"
#include "type-system.h"
#include "symtable.h"
#include "compilation-errors.h"

namespace tolk {

/*
 *   This file contains "lazy" state across multiple files.
 *   They all are used after `lazy` operators have been processed and "load xxx" vertices have been inserted.
 * Particularly, while transforming AST to Ops.
 *   For comments about laziness, see pipe-lazy-load-insertions.cpp.
 */

LazyVariableLoadedState::LazyVariableLoadedState(TypePtr declared_type, std::vector<var_idx_t>&& ir_slice, std::vector<var_idx_t>&& ir_options)
  : declared_type(declared_type)
  , ir_slice(std::move(ir_slice))
  , ir_options(std::move(ir_options))
  , loaded_state(declared_type->unwrap_alias()->try_as<TypeDataStruct>() ? declared_type->unwrap_alias()->try_as<TypeDataStruct>()->struct_ref : nullptr) {
  // fill loaded_variants: variants of a lazy union or the last field of a struct if it's a union
  const TypeDataUnion* t_union = declared_type->unwrap_alias()->try_as<TypeDataUnion>();
  if (is_struct() && loaded_state.original_struct->get_num_fields()) {
    t_union = loaded_state.original_struct->fields.back()->declared_type->unwrap_alias()->try_as<TypeDataUnion>();
  }
  if (t_union) {
    variants_state.reserve(t_union->size());
    for (TypePtr variant : t_union->variants) {
      const TypeDataStruct* variant_struct = variant->unwrap_alias()->try_as<TypeDataStruct>();
      variants_state.emplace_back(variant_struct ? variant_struct->struct_ref : nullptr);
    }
  }
}

const LazyStructLoadedState* LazyVariableLoadedState::get_struct_state(StructPtr original_struct) const {
  if (loaded_state.original_struct == original_struct) {
    return &loaded_state;
  }
  for (const LazyStructLoadedState& struct_state : variants_state) {
    if (struct_state.original_struct == original_struct) {
      return &struct_state;
    }
  }
  return nullptr;
}

void LazyVariableLoadedState::assert_field_loaded(StructPtr original_struct, StructFieldPtr original_field) const {
  // on field access `point.x`, ensure that it's loaded, so the value on a stack is not an occasional null
  const LazyStructLoadedState* struct_state = get_struct_state(original_struct);
  tolk_assert(struct_state);
  tolk_assert(struct_state->was_loaded_once());
  StructFieldPtr hidden_field = struct_state->hidden_struct->find_field(original_field->name);
  tolk_assert(hidden_field);
  tolk_assert(struct_state->ith_field_was_loaded[hidden_field->field_idx]);
}

void LazyStructLoadedState::on_started_loading(StructPtr hidden_struct) {
  this->hidden_struct = hidden_struct;
  this->ith_field_was_loaded.resize(hidden_struct->get_num_fields());   // initially false
}

void LazyStructLoadedState::on_original_field_loaded(StructFieldPtr hidden_field) {
  this->ith_field_was_loaded[hidden_field->field_idx] = true;
  // for example, `var p = lazy Point; aux "load x"; return p.x`;
  // we are at "load x", it exists in Point, here just save it was loaded (for assertions and debugging);
  // apart from saving, stack is also updated when loading, `p` becomes `valueX null`
}

void LazyStructLoadedState::on_aside_field_loaded(StructFieldPtr hidden_field, std::vector<var_idx_t>&& ir_field_gap) {
  this->ith_field_was_loaded[hidden_field->field_idx] = true;
  this->aside_gaps_and_tail.emplace_back(hidden_field, std::move(ir_field_gap));
  // for example, `var st = lazy Storage; aux "load gap, load seqno"; st.seqno += 1; st.toCell()`;
  // we are at "load gap", it does not exist in Storage, so save loaded value separately
}

std::vector<var_idx_t> LazyStructLoadedState::get_ir_loaded_aside_field(StructFieldPtr hidden_field) const {
  // for example, `var st = lazy Storage; aux "load gap, load seqno"; st.seqno += 1; st.toCell()`;
  // we are at "st.toCell()" that stores immutable gap before modified "seqno"
  for (const auto& [gap_field, ir_field_gap] : this->aside_gaps_and_tail) {
    if (gap_field == hidden_field) {
      return ir_field_gap;
    }
  }
  tolk_assert(false);
}

} // namespace tolk
