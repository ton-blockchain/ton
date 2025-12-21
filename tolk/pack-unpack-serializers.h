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
  bool skipping_is_dangerous = false;

  bool is_unpredictable_infinity() const {
    return max_bits >= 9999;
  }

  explicit PackSize(int exact_bits)
    : min_bits(exact_bits), max_bits(exact_bits), min_refs(0), max_refs(0) {
  }
  PackSize(int min_bits, int max_bits)
    : min_bits(min_bits), max_bits(max_bits), min_refs(0), max_refs() {
  }
  PackSize(int min_bits, int max_bits, int min_refs, int max_refs, bool skipping_is_dangerous = false)
    : min_bits(min_bits), max_bits(max_bits), min_refs(min_refs), max_refs(max_refs), skipping_is_dangerous(skipping_is_dangerous) {
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
  AnyV origin;
  const FunctionPtr f_storeInt;
  const FunctionPtr f_storeUint;
  mutable PrefixWriteMode prefix_mode = PrefixWriteMode::WritePrefixOfStruct;

public:
  const std::vector<var_idx_t> ir_builder;
  const var_idx_t ir_builder0;
  const var_idx_t option_skipBitsNValidation;

  PackContext(CodeBlob& code, AnyV origin, std::vector<var_idx_t> ir_builder, const std::vector<var_idx_t>& ir_options);

  PrefixWriteMode get_prefix_mode() const { return prefix_mode; }

  void storeInt(var_idx_t ir_idx, int len) const;
  void storeUint(var_idx_t ir_idx, int len) const;
  void storeUint_var(var_idx_t ir_idx, var_idx_t ir_len) const;
  void storeBool(var_idx_t ir_idx) const;
  void storeCoins(var_idx_t ir_idx) const;
  void storeRef(var_idx_t ir_idx) const;
  void storeMaybeRef(var_idx_t ir_idx) const;
  void storeAddressInt(var_idx_t ir_idx) const;
  void storeAddressAny(var_idx_t ir_idx) const;
  void storeBuilder(var_idx_t ir_idx) const;
  void storeSlice(var_idx_t ir_idx) const;
  void storeOpcode(PackOpcode opcode) const;

  void generate_pack_any(TypePtr any_type, std::vector<var_idx_t>&& rvect, PrefixWriteMode prefix_mode = PrefixWriteMode::WritePrefixOfStruct) const;
};


enum class PrefixReadMode {
  LoadAndCheck,
  DoNothingAlreadyLoaded,
};

struct LazyMatchOptions {
  struct MatchBlock {
    TypePtr arm_variant;          // left of `V => ...`; nullptr for `else => ...`
    AnyExprV v_body;              // right of `V => ...`
    TypePtr block_expr_type;      // for match expression, if `V => expr`, it's expr's inferred_type
  };

  TypePtr match_expr_type;        // type of `match` expression, `void` for statement
  bool is_statement;              // it's `match` statement, not expression, so it does not return any result
  bool add_return_to_all_arms;    // it's the last statement in a function, add "return" to its cases for better Fift code
  std::vector<MatchBlock> match_blocks;

  const MatchBlock* find_match_block(TypePtr variant) const;
  void save_match_result_on_arm_end(CodeBlob& code, AnyV origin, const MatchBlock* arm_block, std::vector<var_idx_t>&& ir_arm_result, const std::vector<var_idx_t>& ir_match_expr_result) const;
};

class UnpackContext {
  CodeBlob& code;
  AnyV origin;
  const FunctionPtr f_loadInt;
  const FunctionPtr f_loadUint;
  const FunctionPtr f_skipBits;
  mutable PrefixReadMode prefix_mode = PrefixReadMode::LoadAndCheck;

public:
  const std::vector<var_idx_t> ir_slice;
  const var_idx_t ir_slice0;
  const var_idx_t option_assertEndAfterReading;
  const var_idx_t option_throwIfOpcodeDoesNotMatch;

  UnpackContext(CodeBlob& code, AnyV origin, std::vector<var_idx_t> ir_slice, const std::vector<var_idx_t>& ir_options);

  PrefixReadMode get_prefix_mode() const { return prefix_mode; }

  std::vector<var_idx_t> loadInt(int len, const char* debug_desc) const;
  std::vector<var_idx_t> loadUint(int len, const char* debug_desc) const;
  void loadAndCheckOpcode(PackOpcode opcode) const;
  void skipBits(int len) const;
  void skipBits_var(var_idx_t ir_len) const;
  void assertEndIfOption() const;
  void throwInvalidOpcode() const;

  std::vector<var_idx_t> generate_unpack_any(TypePtr any_type, PrefixReadMode prefix_mode = PrefixReadMode::LoadAndCheck) const;
  void generate_skip_any(TypePtr any_type, PrefixReadMode prefix_mode = PrefixReadMode::LoadAndCheck) const;
  std::vector<var_idx_t> generate_lazy_match_any(TypePtr any_type, const LazyMatchOptions& options) const;
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
    return PackSize(std::min(a.min_bits, b.min_bits), std::max(a.max_bits, b.max_bits), std::min(a.min_refs, b.min_refs), std::max(a.max_refs, b.max_refs), a.skipping_is_dangerous || b.skipping_is_dangerous);
  }
  static PackSize sum(PackSize a, PackSize b) {
    return PackSize(a.min_bits + b.min_bits, std::min(9999, a.max_bits + b.max_bits), a.min_refs + b.min_refs, a.max_refs + b.max_refs, a.skipping_is_dangerous || b.skipping_is_dangerous);
  }

  PackSize estimate_any(TypePtr any_type, PrefixEstimateMode prefix_mode = PrefixEstimateMode::IncludePrefixOfStruct) const;
};


bool is_type_cellT(TypePtr any_type);
void get_custom_pack_unpack_function(TypePtr receiver_type, FunctionPtr& f_pack, FunctionPtr& f_unpack);
std::vector<PackOpcode> auto_generate_opcodes_for_union(TypePtr union_type, std::string& because_msg);
TypePtr calculate_intN_to_serialize_enum(EnumDefPtr enum_ref);

std::vector<var_idx_t> create_default_PackOptions(CodeBlob& code, AnyV origin);
std::vector<var_idx_t> create_default_UnpackOptions(CodeBlob& code, AnyV origin);

} // namespace tolk
