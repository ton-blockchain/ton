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
#include "tolk.h"

namespace tolk {

using PackOpcode = StructData::PackOpcode;

struct PackSize {
  int min_bits;
  int max_bits;
  int min_refs;
  int max_refs;

  bool is_unpredictable_infinity() const {
    return max_bits >= 9999;
  }

  explicit PackSize(int exact_bits)
    : min_bits(exact_bits), max_bits(exact_bits), min_refs(0), max_refs(0) {
  }
  PackSize(int min_bits, int max_bits)
    : min_bits(min_bits), max_bits(max_bits), min_refs(0), max_refs() {
  }
  PackSize(int min_bits, int max_bits, int min_refs, int max_refs)
    : min_bits(min_bits), max_bits(max_bits), min_refs(min_refs), max_refs(max_refs) {
  }

  static PackSize unpredictable_infinity() {
    return PackSize(0, 9999, 0, 4);
  }
};


enum class PrefixWriteMode {
  WritePrefixOfStruct,
  DoNothingAlreadyWritten,
};

class PackContext {
  CodeBlob& code;
  SrcLocation loc;
  const FunctionPtr f_storeInt;
  const FunctionPtr f_storeUint;
  mutable PrefixWriteMode prefix_mode = PrefixWriteMode::WritePrefixOfStruct;

public:
  const std::vector<var_idx_t> ir_builder;
  const var_idx_t ir_builder0;
  const var_idx_t option_skipBitsNFieldsValidation;

  PackContext(CodeBlob& code, SrcLocation loc, std::vector<var_idx_t> ir_builder, const std::vector<var_idx_t>& ir_options);

  PrefixWriteMode get_prefix_mode() const { return prefix_mode; }

  void storeInt(var_idx_t ir_idx, int len) const;
  void storeUint(var_idx_t ir_idx, int len) const;
  void storeBool(var_idx_t ir_idx) const;
  void storeCoins(var_idx_t ir_idx) const;
  void storeRef(var_idx_t ir_idx) const;
  void storeMaybeRef(var_idx_t ir_idx) const;
  void storeAddress(var_idx_t ir_idx) const;
  void storeBuilder(var_idx_t ir_idx) const;
  void storeSlice(var_idx_t ir_idx) const;
  void storeOpcode(PackOpcode opcode) const;

  void generate_pack_any(TypePtr any_type, std::vector<var_idx_t>&& rvect, PrefixWriteMode prefix_mode = PrefixWriteMode::WritePrefixOfStruct) const;
};


enum class PrefixReadMode {
  LoadAndCheck,
  DoNothingAlreadyLoaded,
};

class UnpackContext {
  CodeBlob& code;
  SrcLocation loc;
  const FunctionPtr f_loadInt;
  const FunctionPtr f_loadUint;
  const FunctionPtr f_skipBits;
  mutable PrefixReadMode prefix_mode = PrefixReadMode::LoadAndCheck;

public:
  const std::vector<var_idx_t> ir_slice;
  const var_idx_t ir_slice0;
  const var_idx_t option_assertEndAfterReading;
  const var_idx_t option_throwIfOpcodeDoesNotMatch;

  UnpackContext(CodeBlob& code, SrcLocation loc, std::vector<var_idx_t> ir_slice, const std::vector<var_idx_t>& ir_options);

  PrefixReadMode get_prefix_mode() const { return prefix_mode; }

  std::vector<var_idx_t> loadInt(int len, const char* debug_desc) const;
  std::vector<var_idx_t> loadUint(int len, const char* debug_desc) const;
  void loadAndCheckOpcode(PackOpcode opcode) const;
  void skipBits(int len) const;
  void skipBits_var(var_idx_t ir_len) const;
  void assertEndIfOption() const;

  std::vector<var_idx_t> generate_unpack_any(TypePtr any_type, PrefixReadMode prefix_mode = PrefixReadMode::LoadAndCheck) const;
  void generate_skip_any(TypePtr any_type, PrefixReadMode prefix_mode = PrefixReadMode::LoadAndCheck) const;
};


enum class PrefixEstimateMode {
  IncludePrefixOfStruct,
  DoNothingAlreadyIncluded,
};

class EstimateContext {
  mutable PrefixEstimateMode prefix_mode = PrefixEstimateMode::IncludePrefixOfStruct;

public:

  PrefixEstimateMode get_prefix_mode() const { return prefix_mode; }

  static PackSize minmax(PackSize a, PackSize b) {
    return PackSize(std::min(a.min_bits, b.min_bits), std::max(a.max_bits, b.max_bits), std::min(a.min_refs, b.min_refs), std::max(a.max_refs, b.max_refs));
  }
  static PackSize sum(PackSize a, PackSize b) {
    return PackSize(a.min_bits + b.min_bits, std::min(9999, a.max_bits + b.max_bits), a.min_refs + b.min_refs, a.max_refs + b.max_refs);
  }

  PackSize estimate_any(TypePtr any_type, PrefixEstimateMode prefix_mode = PrefixEstimateMode::IncludePrefixOfStruct) const;
};


bool is_type_cellT(TypePtr any_type);
std::vector<PackOpcode> auto_generate_opcodes_for_union(TypePtr union_type, std::string& because_msg);

} // namespace tolk
