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
#include "pack-unpack-api.h"
#include "generics-helpers.h"
#include "lazy-helpers.h"
#include "type-system.h"
#include <optional>

/*
 *   This module provides high-level (de)serialization functions to be used from outer code:
 * - pack to cell/builder
 * - unpack from cell/slice
 * - etc.
 *
 *   For the implementation of packing primitives, consider `pack-unpack-serializers.cpp`.
 */

namespace tolk {


// --------------------------------------------
//    checking serialization availability
//
// for every call `obj.toCell()` and similar, checks are executed to ensure that `obj` can be serialized
// if it can, the compilation process continues
// if not, a detailed explanation is shown
//


struct CantSerializeBecause {
  std::string because_msg;

  explicit CantSerializeBecause(std::string because_msg)
    : because_msg(std::move(because_msg)) {}
  explicit CantSerializeBecause(const std::string& because_msg, const CantSerializeBecause& why)
    : because_msg(because_msg + "\n" + why.because_msg) {}
};

class PackUnpackAvailabilityChecker {
  std::vector<StructPtr> already_checked;

  static bool check_declared_packToBuilder(TypePtr receiver_type, FunctionPtr f_pack) {
    if (!f_pack->does_accept_self() || f_pack->does_mutate_self() || f_pack->get_num_params() != 2) {
      return false;
    }
    if (f_pack->get_param(1).declared_type != TypeDataBuilder::create() || !f_pack->has_mutate_params()) {
      return false;
    }
    return f_pack->inferred_return_type->get_width_on_stack() == 0;
  }

  static bool check_declared_unpackFromSlice(TypePtr receiver_type, FunctionPtr f_unpack) {
    if (f_unpack->does_accept_self() || f_unpack->get_num_params() != 1) {
      return false;
    }
    if (f_unpack->get_param(0).declared_type != TypeDataSlice::create() || !f_unpack->has_mutate_params()) {
      return false;
    }
    return f_unpack->inferred_return_type->equal_to(receiver_type);
  }

public:
  std::optional<CantSerializeBecause> detect_why_cant_serialize(TypePtr any_type, bool is_pack) {
    if (any_type->try_as<TypeDataIntN>()) {
      return {};
    }
    if (any_type->try_as<TypeDataBitsN>()) {
      return {};
    }
    if (any_type == TypeDataCoins::create()) {
      return {};
    }
    if (any_type == TypeDataBool::create()) {
      return {};
    }
    if (any_type == TypeDataCell::create()) {
      return {};
    }
    if (any_type == TypeDataVoid::create()) {
      return {};
    }
    if (any_type->try_as<TypeDataMapKV>()) {
      return {};
    }
    if (any_type->try_as<TypeDataAddress>()) {
      return {};
    }

    if (const auto* t_struct = any_type->try_as<TypeDataStruct>()) {
      StructPtr struct_ref = t_struct->struct_ref;
      if (std::find(already_checked.begin(), already_checked.end(), struct_ref) != already_checked.end()) {
        return {};
      }
      already_checked.push_back(struct_ref);    // prevent recursion and visiting one struct multiple times

      for (StructFieldPtr field_ref : struct_ref->fields) {
        if (auto why = detect_why_cant_serialize(field_ref->declared_type, is_pack)) {
          return CantSerializeBecause("because field `" + struct_ref->name + "." + field_ref->name + "` of type `" + field_ref->declared_type->as_human_readable() + "` can't be serialized", why.value());
        }
      }
      if (is_type_cellT(t_struct)) {
        TypePtr cellT = struct_ref->substitutedTs->typeT_at(0);
        if (auto why = detect_why_cant_serialize(cellT, is_pack)) {
          return CantSerializeBecause("because type `" + cellT->as_human_readable() + "` can't be serialized", why.value());
        }
      }
      return {};
    }

    if (const auto* t_enum = any_type->try_as<TypeDataEnum>()) {
      if (t_enum->enum_ref->members.empty()) {
        return CantSerializeBecause("because `enum` is empty");
      }
      return {};
    }
    
    if (const auto* t_union = any_type->try_as<TypeDataUnion>()) {
      // a union can almost always be serialized if every of its variants can:
      // - `T?` is TL/B `(Maybe T)`
      // - `T1 | T2` is TL/B `(Either T1 T2)` (or, if opcodes manually set, just by opcodes)
      // - `T1 | T2 | ...` is either by manual opcodes, or the compiler implicitly defines them
      // so, even `int32 | int64 | int128` or `A | B | C | null` are serializable
      // (unless corner cases occur, like duplicated opcodes, etc.)
      for (int i = 0; i < t_union->size(); ++i) {
        TypePtr variant = t_union->variants[i];
        if (variant == TypeDataNullLiteral::create()) {
          continue;
        }
        if (auto why = detect_why_cant_serialize(variant, is_pack)) {
          return CantSerializeBecause("because variant #" + std::to_string(i + 1) + " of type `" + variant->as_human_readable() + "` can't be serialized", why.value());
        }
      }
      if (!t_union->or_null) {
        std::string because_msg;
        auto_generate_opcodes_for_union(t_union, because_msg);
        if (!because_msg.empty()) {
          return CantSerializeBecause("because could not automatically generate serialization prefixes for a union\n" + because_msg);
        }
      }
      return {};
    }

    if (const auto* t_tensor = any_type->try_as<TypeDataTensor>()) {
      for (int i = 0; i < t_tensor->size(); ++i) {
        if (auto why = detect_why_cant_serialize(t_tensor->items[i], is_pack)) {
          return CantSerializeBecause("because element `tensor." + std::to_string(i) + "` of type `" + t_tensor->items[i]->as_human_readable() + "` can't be serialized", why.value());
        }
      }
      return {};
    }

    if (const auto* t_alias = any_type->try_as<TypeDataAlias>()) {
      if (t_alias->alias_ref->name == "RemainingBitsAndRefs") {   // it's built-in RemainingBitsAndRefs (slice)
        return {};
      }
      FunctionPtr f_pack = nullptr, f_unpack = nullptr;
      get_custom_pack_unpack_function(t_alias, f_pack, f_unpack);
      if (f_pack) {
        std::string receiver_name = t_alias->alias_ref->as_human_readable();
        if (!check_declared_packToBuilder(t_alias, f_pack)) {
          return CantSerializeBecause("because `" + receiver_name + ".packToBuilder()` is declared incorrectly\n""hint: it must accept 2 parameters and return nothing:\n> fun " + receiver_name + ".packToBuilder(self, mutate b: builder)");
        }
        if (!f_pack->is_inlined_in_place()) {
          return CantSerializeBecause("because `" + receiver_name + ".packToBuilder()` can't be inlined; probably, it contains `return` in the middle");
        }
        if (!is_pack && !f_unpack) {
          return CantSerializeBecause("because type `" + receiver_name + "` defines a custom pack function, but does not define unpack\n""hint: declare unpacker like this:\n> fun " + receiver_name + ".unpackFromSlice(mutate s: slice): " + receiver_name);
        }
      }
      if (f_unpack) {
        std::string receiver_name = t_alias->alias_ref->as_human_readable();
        if (!check_declared_unpackFromSlice(t_alias, f_unpack)) {
          return CantSerializeBecause("because `" + receiver_name + ".unpackFromSlice()` is declared incorrectly\n""hint: it must accept 1 parameter and return an object:\n> fun " + receiver_name + ".unpackFromSlice(mutate s: slice): " + receiver_name);
        }
        if (!f_unpack->is_inlined_in_place()) {
          return CantSerializeBecause("because `" + receiver_name + ".unpackFromSlice()` can't be inlined; probably, it contains `return` in the middle");
        }
        if (is_pack && !f_pack) {
          return CantSerializeBecause("because type `" + receiver_name + "` defines a custom unpack function, but does not define pack\n""hint: declare packer like this:\n> fun " + receiver_name + ".packToBuilder(self, mutate b: builder)");
        }
      }
      if (!f_pack && !f_unpack) {
        if (auto why = detect_why_cant_serialize(t_alias->underlying_type, is_pack)) {
          return CantSerializeBecause("because alias `" + t_alias->as_human_readable() + "` expands to `" + t_alias->underlying_type->as_human_readable() + "`", why.value());
        }
      }
      return {};
    }

    // `builder` and `slice` can be used for writing, but not for reading
    if (any_type == TypeDataBuilder::create()) {
      if (is_pack) {
        return {};
      }
      return CantSerializeBecause("because type `builder` can not be used for reading, only for writing\n""hint: use `bitsN` or `RemainingBitsAndRefs` for reading\n""hint: using generics, you can substitute `builder` for writing and something other for reading");
    }
    if (any_type == TypeDataSlice::create()) {
      if (is_pack) {
        return {};
      }
      return CantSerializeBecause("because type `slice` can not be used for reading, it doesn't define binary width\n""hint: replace `slice` with `address` if it's an address, actually\n""hint: replace `slice` with `bits128` and similar if it represents fixed-width data without refs");
    }

    // serialization not available
    // for common types, make a detailed explanation with a hint how to fix

    if (any_type == TypeDataInt::create()) {
      return CantSerializeBecause("because type `int` is not serializable, it doesn't define binary width\n""hint: replace `int` with `int32` / `uint64` / `coins` / etc.");
    }
    if (any_type == TypeDataNullLiteral::create()) {
      return CantSerializeBecause("because type `null` is not serializable\n""hint: `int32?` and other nullable types will work");
    }
    if (any_type == TypeDataTuple::create() || any_type->try_as<TypeDataBrackets>()) {
      return CantSerializeBecause("because tuples are not serializable\n""hint: use tensors instead of tuples, they will work");
    }

    return CantSerializeBecause("because type `" + any_type->as_human_readable() + "` is not serializable");
  }
};

bool check_struct_can_be_packed_or_unpacked(TypePtr any_type, bool is_pack, std::string& because_msg) {
  PackUnpackAvailabilityChecker checker;
  if (auto why = checker.detect_why_cant_serialize(any_type, is_pack)) {
    because_msg = why.value().because_msg;
    return false;
  }
  return true;
}

static int calc_offset_on_stack(StructPtr struct_ref, int field_idx) {
  int stack_offset = 0;
  for (int i = 0; i < field_idx; ++i) {
    stack_offset += struct_ref->get_field(i)->declared_type->get_width_on_stack();
  }
  return stack_offset;
}


// --------------------------------------------
//    high-level API for outer code
//


// fun T.toCell(self, options: PackOptions): Cell<T>
std::vector<var_idx_t> generate_T_toCell(FunctionPtr called_f, CodeBlob& code, AnyV origin, const std::vector<std::vector<var_idx_t>>& args) {
  TypePtr typeT = called_f->substitutedTs->typeT_at(0);
  FunctionPtr f_beginCell = lookup_function("beginCell");
  FunctionPtr f_endCell = lookup_function("builder.endCell");
  std::vector rvect_builder = code.create_var(TypeDataBuilder::create(), origin, "b");
  code.emplace_back(origin, Op::_Call, rvect_builder, std::vector<var_idx_t>{}, f_beginCell);

  PackContext ctx(code, origin, rvect_builder, args[1]);
  ctx.generate_pack_any(typeT, std::vector(args[0]));

  std::vector rvect_cell = code.create_tmp_var(TypeDataCell::create(), origin, "(cell)");
  code.emplace_back(origin, Op::_Call, rvect_cell, std::move(rvect_builder), f_endCell);

  return rvect_cell;
}

// fun builder.storeAny<T>(mutate self, v: T, options: PackOptions = {}): self
std::vector<var_idx_t> generate_builder_storeAny(FunctionPtr called_f, CodeBlob& code, AnyV origin, const std::vector<std::vector<var_idx_t>>& args) {
  TypePtr typeT = called_f->substitutedTs->typeT_at(0);
  PackContext ctx(code, origin, args[0], args[2]);   // mutate this builder
  ctx.generate_pack_any(typeT, std::vector(args[1]));

  return args[0];  // return mutated builder
}

// fun T.fromSlice(rawSlice: slice, options: UnpackOptions): T
std::vector<var_idx_t> generate_T_fromSlice(FunctionPtr called_f, CodeBlob& code, AnyV origin, const std::vector<std::vector<var_idx_t>>& args) {
  std::vector slice_copy = code.create_var(TypeDataSlice::create(), origin, "s");
  code.emplace_back(origin, Op::_Let, slice_copy, args[0]);

  TypePtr typeT = called_f->substitutedTs->typeT_at(0);
  UnpackContext ctx(code, origin, std::move(slice_copy), args[1]);
  std::vector rvect_struct = ctx.generate_unpack_any(typeT);
  tolk_assert(typeT->get_width_on_stack() == static_cast<int>(rvect_struct.size()));

  if (!estimate_serialization_size(typeT).is_unpredictable_infinity()) {
    ctx.assertEndIfOption();
  }
  return rvect_struct;
}

// fun slice.loadAny<T>(mutate self, options: UnpackOptions): T
std::vector<var_idx_t> generate_slice_loadAny(FunctionPtr called_f, CodeBlob& code, AnyV origin, const std::vector<std::vector<var_idx_t>>& args) {
  TypePtr typeT = called_f->substitutedTs->typeT_at(0);
  UnpackContext ctx(code, origin, args[0], args[1]);
  std::vector rvect_struct = ctx.generate_unpack_any(typeT);
  tolk_assert(typeT->get_width_on_stack() == static_cast<int>(rvect_struct.size()));

  // slice.loadAny() ignores options.assertEndAfterReading, because it's intended to read data in the middle
  std::vector ir_slice_and_result = args[0];
  ir_slice_and_result.insert(ir_slice_and_result.end(), rvect_struct.begin(), rvect_struct.end());
  return ir_slice_and_result;
}

// fun T.fromCell(packedCell: cell, options: UnpackOptions): T
// fun Cell<T>.load(self, options: UnpackOptions): T
std::vector<var_idx_t> generate_T_fromCell(FunctionPtr called_f, CodeBlob& code, AnyV origin, const std::vector<std::vector<var_idx_t>>& args) {
  TypePtr typeT = called_f->substitutedTs->typeT_at(0);
  FunctionPtr f_beginParse = lookup_function("cell.beginParse");
  std::vector ir_slice = code.create_var(TypeDataSlice::create(), origin, "s");
  code.emplace_back(origin, Op::_Call, ir_slice, args[0], f_beginParse);

  UnpackContext ctx(code, origin, std::move(ir_slice), args[1]);
  std::vector rvect_struct = ctx.generate_unpack_any(typeT);
  tolk_assert(typeT->get_width_on_stack() == static_cast<int>(rvect_struct.size()));

  // if a struct has RemainingBitsAndRefs, don't test it for assertEnd
  if (!estimate_serialization_size(typeT).is_unpredictable_infinity()) {
    ctx.assertEndIfOption();
  }
  return rvect_struct;
}

// fun slice.skipAny<T>(mutate self, options: UnpackOptions): self
std::vector<var_idx_t> generate_slice_skipAny(FunctionPtr called_f, CodeBlob& code, AnyV origin, const std::vector<std::vector<var_idx_t>>& args) {
  TypePtr typeT = called_f->substitutedTs->typeT_at(0);
  UnpackContext ctx(code, origin, args[0], args[1]);    // mutate this slice
  ctx.generate_skip_any(typeT);

  return args[0];  // return mutated slice
}

void generate_lazy_struct_from_slice(CodeBlob& code, AnyV origin, const LazyVariableLoadedState* lazy_variable, const LazyStructLoadInfo& load_info, const std::vector<var_idx_t>& ir_obj) {
  StructPtr original_struct = load_info.original_struct;
  StructPtr hidden_struct = load_info.hidden_struct;
  tolk_assert(hidden_struct->fields.size() == load_info.ith_field_action.size());

  const LazyStructLoadedState* loaded_state = lazy_variable->get_struct_state(original_struct);
  tolk_assert(loaded_state && !loaded_state->was_loaded_once());
  loaded_state->mutate()->on_started_loading(hidden_struct);

  UnpackContext ctx(code, origin, lazy_variable->ir_slice, lazy_variable->ir_options);

  if (hidden_struct->opcode.exists()) {
    ctx.loadAndCheckOpcode(hidden_struct->opcode);
  }

  for (int field_idx = 0; field_idx < hidden_struct->get_num_fields(); ++field_idx) {
    StructFieldPtr hidden_field = hidden_struct->get_field(field_idx);
    tolk_assert(!loaded_state->ith_field_was_loaded[field_idx]);

    // note that as opposed to regular loading, lazy loading doesn't return rvect, it fills stack slots (ir_obj) instead
    switch (load_info.ith_field_action[field_idx]) {
      case LazyStructLoadInfo::LoadField: {
        if (StructFieldPtr original_field = original_struct->find_field(hidden_field->name)) {
          tolk_assert(hidden_field->declared_type == original_field->declared_type);
          std::vector ir_field = ctx.generate_unpack_any(hidden_field->declared_type);
          int stack_offset = calc_offset_on_stack(original_struct, original_field->field_idx);
          int stack_width = hidden_field->declared_type->get_width_on_stack();
          code.emplace_back(origin, Op::_Let, std::vector(ir_obj.begin() + stack_offset, ir_obj.begin() + stack_offset + stack_width), std::move(ir_field));
          loaded_state->mutate()->on_original_field_loaded(hidden_field);
        } else {
          tolk_assert(hidden_field->name == "(gap)");
          std::vector ir_gap = ctx.generate_unpack_any(hidden_field->declared_type);
          loaded_state->mutate()->on_aside_field_loaded(hidden_field, std::move(ir_gap));
        }
        break;
      }
      case LazyStructLoadInfo::SkipField: {
        ctx.generate_skip_any(hidden_field->declared_type);
        break;
      }
      case LazyStructLoadInfo::LazyMatchField: {
        StructFieldPtr original_field = original_struct->find_field(hidden_field->name);
        tolk_assert(original_field && hidden_field->declared_type == original_field->declared_type);
        loaded_state->mutate()->on_original_field_loaded(hidden_field);
        break;
      }
      case LazyStructLoadInfo::SaveImmutableTail: {
        std::vector ir_immutable_tail = code.create_tmp_var(TypeDataSlice::create(), origin, "(lazy-tail-slice)");
        code.emplace_back(origin, Op::_Let, ir_immutable_tail, lazy_variable->ir_slice);
        loaded_state->mutate()->on_aside_field_loaded(hidden_field, std::move(ir_immutable_tail));
        break;
      }
    }
  }

  // options.assertEndAfterReading is ignored by `lazy`, because tail fields may be skipped, it's okay
}

std::vector<var_idx_t> generate_lazy_struct_to_cell(CodeBlob& code, AnyV origin, const LazyStructLoadedState* loaded_state, std::vector<var_idx_t>&& ir_obj, const std::vector<var_idx_t>& ir_options) {
  StructPtr original_struct = loaded_state->original_struct;
  StructPtr hidden_struct = loaded_state->hidden_struct;

  std::vector rvect_builder = code.create_var(TypeDataBuilder::create(), origin, "b");
  code.emplace_back(origin, Op::_Call, rvect_builder, std::vector<var_idx_t>{}, lookup_function("beginCell"));

  PackContext ctx(code, origin, rvect_builder, ir_options);

  if (hidden_struct->opcode.exists()) {
    ctx.storeUint(code.create_int(origin, hidden_struct->opcode.pack_prefix, "(struct-prefix)"), hidden_struct->opcode.prefix_len);
  }

  for (int field_idx = 0; field_idx < hidden_struct->get_num_fields(); ++field_idx) {
    StructFieldPtr hidden_field = hidden_struct->get_field(field_idx);
    tolk_assert(loaded_state->ith_field_was_loaded[field_idx]);

    if (StructFieldPtr original_field = original_struct->find_field(hidden_field->name)) {
      int stack_offset = calc_offset_on_stack(original_struct, original_field->field_idx);
      int stack_width = hidden_field->declared_type->get_width_on_stack();
      std::vector ir_field(ir_obj.begin() + stack_offset, ir_obj.begin() + stack_offset + stack_width);
      ctx.generate_pack_any(hidden_field->declared_type, std::move(ir_field));
    } else {
      std::vector ir_gap_or_tail = loaded_state->get_ir_loaded_aside_field(hidden_field);
      if (hidden_field->declared_type->unwrap_alias()->try_as<TypeDataBitsN>()) {
        ctx.storeSlice(ir_gap_or_tail[0]);
      } else {
        ctx.generate_pack_any(hidden_field->declared_type, std::move(ir_gap_or_tail));
      }
      if (hidden_field->name == "(tail)") {
        break;
      }
    }
  }

  std::vector rvect_cell = code.create_tmp_var(TypeDataCell::create(), origin, "(cell)");
  code.emplace_back(origin, Op::_Call, rvect_cell, std::move(rvect_builder), lookup_function("builder.endCell"));

  return rvect_cell;
}

std::vector<var_idx_t> generate_lazy_match_for_union(CodeBlob& code, AnyV origin, TypePtr union_type, const LazyVariableLoadedState* lazy_variable, const LazyMatchOptions& options) {
  tolk_assert(lazy_variable->ir_options.size() == 2);
  if (options.match_blocks.empty()) {   // empty `match` statement, no arms
    return {};
  }
  UnpackContext ctx(code, origin, lazy_variable->ir_slice, lazy_variable->ir_options);
  std::vector rvect_match = ctx.generate_lazy_match_any(union_type, options);

  return rvect_match;
}

std::vector<var_idx_t> generate_lazy_object_finish_loading(CodeBlob& code, AnyV origin, const LazyVariableLoadedState* lazy_variable, std::vector<var_idx_t>&& ir_obj) {
  tolk_assert(lazy_variable->ir_slice.size() == 1);

  // the call to `lazy_var.forceLoadLazyObject()` does not do anything: at the moment of analyzing,
  // it had marked all the object as "used", all fields where loaded, and the slice points after the last field;
  // so, just return the held slice
  static_cast<void>(code);
  static_cast<void>(origin);
  static_cast<void>(ir_obj);

  return lazy_variable->ir_slice;
}

std::vector<var_idx_t> generate_T_forceLoadLazyObject(FunctionPtr called_f, CodeBlob& code, AnyV origin, const std::vector<std::vector<var_idx_t>>& args) {
  // a call to `T.forceLoadLazyObject()` was handled separately
  tolk_assert(false);
}

PackSize estimate_serialization_size(TypePtr any_type) {
  EstimateContext ctx;
  return ctx.estimate_any(any_type);
}

std::vector<var_idx_t> generate_T_estimatePackSize(FunctionPtr called_f, CodeBlob& code, AnyV origin, const std::vector<std::vector<var_idx_t>>& args) {
  TypePtr typeT = called_f->substitutedTs->typeT_at(0);
  PackSize pack_size = estimate_serialization_size(typeT);

  std::vector ir_tensor = code.create_tmp_var(TypeDataTensor::create({TypeDataInt::create(), TypeDataInt::create(), TypeDataInt::create(), TypeDataInt::create()}), origin, "(result-tensor)");
  code.emplace_back(origin, Op::_IntConst, std::vector{ir_tensor[0]}, td::make_refint(pack_size.min_bits));
  code.emplace_back(origin, Op::_IntConst, std::vector{ir_tensor[1]}, td::make_refint(pack_size.max_bits));
  code.emplace_back(origin, Op::_IntConst, std::vector{ir_tensor[2]}, td::make_refint(pack_size.min_refs));
  code.emplace_back(origin, Op::_IntConst, std::vector{ir_tensor[3]}, td::make_refint(pack_size.max_refs));

  return ir_tensor;
}

} // namespace tolk
