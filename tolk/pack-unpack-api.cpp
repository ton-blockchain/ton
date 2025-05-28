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
public:
  static std::optional<CantSerializeBecause> detect_why_cant_serialize(TypePtr any_type, bool is_pack) {
    if (any_type->try_as<TypeDataIntN>()) {
      return {};
    }
    if (any_type->try_as<TypeDataBytesN>()) {
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
    if (any_type == TypeDataAddress::create()) {
      return {};
    }
    if (any_type == TypeDataNever::create()) {
      return {};
    }

    if (const auto* t_struct = any_type->try_as<TypeDataStruct>()) {
      StructPtr struct_ref = t_struct->struct_ref;
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
      if (auto why = detect_why_cant_serialize(t_alias->underlying_type, is_pack)) {
        return CantSerializeBecause("because alias `" + t_alias->as_human_readable() + "` expands to `" + t_alias->underlying_type->as_human_readable() + "`", why.value());
      }
      return {};
    }

    // `builder` can be used for writing, but not for reading
    if (any_type == TypeDataBuilder::create()) {
      if (is_pack) {
        return {};
      }
      return CantSerializeBecause("because type `builder` can not be used for reading, only for writing\nhint: use `bitsN` or `RemainingBitsAndRefs` for reading\nhint: using generics, you can substitute `builder` for writing and something other for reading");
    }

    // serialization not available
    // for common types, make a detailed explanation with a hint how to fix

    if (any_type == TypeDataInt::create()) {
      return CantSerializeBecause("because type `int` is not serializable, it doesn't define binary width\nhint: replace `int` with `int32` / `uint64` / `coins` / etc.");
    }
    if (any_type == TypeDataSlice::create()) {
      return CantSerializeBecause("because type `slice` is not serializable, it doesn't define binary width\nhint: replace `slice` with `address` if it's an address, actually\nhint: replace `slice` with `bits128` and similar if it represents fixed-width data without refs");
    }
    if (any_type == TypeDataNullLiteral::create()) {
      return CantSerializeBecause("because type `null` is not serializable\nhint: `int32?` and other nullable types will work");
    }
    if (any_type == TypeDataTuple::create() || any_type->try_as<TypeDataBrackets>()) {
      return CantSerializeBecause("because tuples are not serializable\nhint: use tensors instead of tuples, they will work");
    }

    return CantSerializeBecause("because type `" + any_type->as_human_readable() + "` is not serializable");
  }
};

bool check_struct_can_be_packed_or_unpacked(TypePtr any_type, bool is_pack, std::string& because_msg) {
  if (auto why = PackUnpackAvailabilityChecker::detect_why_cant_serialize(any_type, is_pack)) {
    because_msg = why.value().because_msg;
    return false;
  }
  return true;
}


// --------------------------------------------
//    high-level API for outer code
//


std::vector<var_idx_t> generate_pack_struct_to_cell(CodeBlob& code, SrcLocation loc, TypePtr any_type, std::vector<var_idx_t>&& ir_obj, const std::vector<var_idx_t>& ir_options) {
  FunctionPtr f_beginCell = lookup_function("beginCell");
  FunctionPtr f_endCell = lookup_function("builder.endCell");
  std::vector rvect_builder = code.create_var(TypeDataBuilder::create(), loc, "b");
  code.emplace_back(loc, Op::_Call, rvect_builder, std::vector<var_idx_t>{}, f_beginCell);

  tolk_assert(ir_options.size() == 1);      // struct PackOptions
  PackContext ctx(code, loc, rvect_builder, ir_options);
  ctx.generate_pack_any(any_type, std::move(ir_obj));

  std::vector rvect_cell = code.create_tmp_var(TypeDataCell::create(), loc, "(cell)");
  code.emplace_back(loc, Op::_Call, rvect_cell, std::move(rvect_builder), f_endCell);

  return rvect_cell;
}

std::vector<var_idx_t> generate_pack_struct_to_builder(CodeBlob& code, SrcLocation loc, TypePtr any_type, std::vector<var_idx_t>&& ir_builder, std::vector<var_idx_t>&& ir_obj, const std::vector<var_idx_t>& ir_options) {
  PackContext ctx(code, loc, ir_builder, ir_options);   // mutate this builder
  ctx.generate_pack_any(any_type, std::move(ir_obj));

  return ir_builder;  // return mutated builder
}

std::vector<var_idx_t> generate_unpack_struct_from_slice(CodeBlob& code, SrcLocation loc, TypePtr any_type, std::vector<var_idx_t>&& ir_slice, bool mutate_slice, const std::vector<var_idx_t>& ir_options) {
  if (!mutate_slice) {
    std::vector slice_copy = code.create_var(TypeDataSlice::create(), loc, "s");
    code.emplace_back(loc, Op::_Let, slice_copy, std::move(ir_slice));
    ir_slice = std::move(slice_copy);
  }

  tolk_assert(ir_options.size() == 2);      // struct UnpackOptions
  UnpackContext ctx(code, loc, std::move(ir_slice), ir_options);
  std::vector rvect_struct = ctx.generate_unpack_any(any_type);
  tolk_assert(any_type->get_width_on_stack() == static_cast<int>(rvect_struct.size()));

  // slice.loadAny() ignores options.assertEndAfterReading, because it's intended to read data in the middle
  if (!mutate_slice && !estimate_serialization_size(any_type).is_unpredictable_infinity()) {
    ctx.assertEndIfOption();
  }
  return rvect_struct;
}

std::vector<var_idx_t> generate_unpack_struct_from_cell(CodeBlob& code, SrcLocation loc, TypePtr any_type, std::vector<var_idx_t>&& ir_cell, const std::vector<var_idx_t>& ir_options) {
  FunctionPtr f_beginParse = lookup_function("cell.beginParse");
  std::vector ir_slice = code.create_var(TypeDataSlice::create(), loc, "s");
  code.emplace_back(loc, Op::_Call, ir_slice, std::move(ir_cell), f_beginParse);

  tolk_assert(ir_options.size() == 2);      // struct UnpackOptions
  UnpackContext ctx(code, loc, std::move(ir_slice), ir_options);
  std::vector rvect_struct = ctx.generate_unpack_any(any_type);
  tolk_assert(any_type->get_width_on_stack() == static_cast<int>(rvect_struct.size()));

  // if a struct has RemainingBitsAndRefs, don't test it for assertEnd
  if (!estimate_serialization_size(any_type).is_unpredictable_infinity()) {
    ctx.assertEndIfOption();
  }
  return rvect_struct;
}

std::vector<var_idx_t> generate_skip_struct_in_slice(CodeBlob& code, SrcLocation loc, TypePtr any_type, std::vector<var_idx_t>&& ir_slice, const std::vector<var_idx_t>& ir_options) {
  UnpackContext ctx(code, loc, ir_slice, ir_options);    // mutate this slice
  ctx.generate_skip_any(any_type);

  return ir_slice;  // return mutated slice
}

PackSize estimate_serialization_size(TypePtr any_type) {
  EstimateContext ctx;
  return ctx.estimate_any(any_type);
}

std::vector<var_idx_t> generate_estimate_size_call(CodeBlob& code, SrcLocation loc, TypePtr any_type) {
  EstimateContext ctx;
  PackSize pack_size = ctx.estimate_any(any_type);

  std::vector ir_tensor = code.create_tmp_var(TypeDataTensor::create({TypeDataInt::create(), TypeDataInt::create(), TypeDataInt::create(), TypeDataInt::create()}), loc, "(result-tensor)");
  code.emplace_back(loc, Op::_IntConst, std::vector{ir_tensor[0]}, td::make_refint(pack_size.min_bits));
  code.emplace_back(loc, Op::_IntConst, std::vector{ir_tensor[1]}, td::make_refint(pack_size.max_bits));
  code.emplace_back(loc, Op::_IntConst, std::vector{ir_tensor[2]}, td::make_refint(pack_size.min_refs));
  code.emplace_back(loc, Op::_IntConst, std::vector{ir_tensor[3]}, td::make_refint(pack_size.max_refs));

  FunctionPtr f_toTuple = lookup_function("T.__toTuple");
  std::vector ir_tuple = code.create_tmp_var(TypeDataTuple::create(), loc, "(result-tuple)");
  code.emplace_back(loc, Op::_Call, ir_tuple, ir_tensor, f_toTuple);

  return ir_tuple;
}

} // namespace tolk
