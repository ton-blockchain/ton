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
#include <vector>

namespace tolk {

// LazyStructLoadInfo describes how to load a struct: which fields to load, which to skip.
// It's calculated based on variable usages and passed through the pipeline carried by auxiliary AST vertices.
// Based on it, lazy loading Ops are generated in pack-unpack api.
// To understand `hidden_struct`, read pipe-lazy-load-insertions.cpp.
struct LazyStructLoadInfo {
  enum ActionWithField {
    LoadField,
    SkipField,
    LazyMatchField,
    SaveImmutableTail,
  };

  StructPtr original_struct;                      // original (e.g. `Point`)
  StructPtr hidden_struct;                        // "lazy Point" — only requested fields, matching binary shape
  std::vector<ActionWithField> ith_field_action;  // each for corresponding field of a struct

  LazyStructLoadInfo(StructPtr original_struct, StructPtr hidden_struct, std::vector<ActionWithField>&& ith_field_action)
    : original_struct(original_struct)
    , hidden_struct(hidden_struct)
    , ith_field_action(std::move(ith_field_action)) {
  }
};

// LazyStructLoadedState represents state (which fields were already loaded) while generating AST to Ops.
// For example, variable `var p = lazy Point.fromSlice(s); aux "load x"; return p.x` is initially "nothing loaded",
// and after "load x" ith_field_action[0] becomes true (and `p` is updated on a stack and becomes `valueX null`).
struct LazyStructLoadedState {
  StructPtr original_struct;                      // original (e.g. `Point`)
  StructPtr hidden_struct = nullptr;              // "lazy Point" — only requested fields, matching binary shape
  std::vector<bool> ith_field_was_loaded;         // each for corresponding field of hidden_struct
  std::vector<std::pair<StructFieldPtr, std::vector<var_idx_t>>> aside_gaps_and_tail;

  explicit LazyStructLoadedState(StructPtr original_struct)
    : original_struct(original_struct) {}

  void on_started_loading(StructPtr hidden_struct);
  void on_original_field_loaded(StructFieldPtr hidden_field);
  void on_aside_field_loaded(StructFieldPtr hidden_field, std::vector<var_idx_t>&& ir_field_gap);

  bool was_loaded_once() const { return hidden_struct != nullptr; }
  std::vector<var_idx_t> get_ir_loaded_aside_field(StructFieldPtr hidden_field) const;

  LazyStructLoadedState* mutate() const { return const_cast<LazyStructLoadedState*>(this); }
};

// LazyVariableLoadedState contains a state of a whole lazy variable while generating AST to Ops.
// For example, `var p = lazy Point.fromSlice(s)` contains one struct.
// But `var msg = lazy MyMsgUnion.fromSlice(s)` contains N variants, each with own state, but common lazy slice `s`.
// When inlining a function, like `p.getX()`, `self` of `getX` also becomes a lazy variable pointing to the same state.
struct LazyVariableLoadedState {
  TypePtr declared_type;
  std::vector<var_idx_t> ir_slice;                    // filled by `lazy` operator
  std::vector<var_idx_t> ir_options;                  // same, comes from `lazy T.fromSlice(s, options)`
  LazyStructLoadedState loaded_state;                 // for struct: filled; for union: empty
  std::vector<LazyStructLoadedState> variants_state;  // variants of a lazy union or the last field if it's a union

  LazyVariableLoadedState(TypePtr declared_type, std::vector<var_idx_t>&& ir_slice, std::vector<var_idx_t>&& ir_options);

  bool is_struct() const { return loaded_state.original_struct != nullptr; }
  bool is_union() const { return loaded_state.original_struct == nullptr; }

  const LazyStructLoadedState* get_struct_state(StructPtr original_struct) const;
  void assert_field_loaded(StructPtr original_struct, StructFieldPtr original_field) const;
};


} // namespace tolk

