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
#include "pack-unpack-serializers.h"
#include "ast.h"
#include "compilation-errors.h"
#include "type-system.h"
#include "td/utils/crypto.h"

/*
 *   This module implements serializing different types to/from cells.
 *   For any serializable TypePtr, we detect ISerializer, which can pack/unpack/skip/estimate size.
 * See `get_serializer_for_type()`.
 *   Example: given an object of `struct A { f: int32 }` its type is TypeDataStruct(A), its serializer is
 * "custom struct", which iterates fields, for field `f` its serializer is "intN" with N=32.
 *
 *   Serializing compound types is complicated, involving transitioning IR variables. For example, to serialize
 * `int8 | A` (it's Either), we have input rvect of size = 1 + width(A), generate dynamic IF ELSE, and in each branch,
 * transition rvect slots to a narrowed type. Operating with transitions and runtime type checking are implemented
 * in IR generation, here we just reference those prototypes.
 *
 *   For high-level (de)serialization API, consider `pack-unpack-api.cpp`.
 */

namespace tolk {

class LValContext;
std::vector<var_idx_t> pre_compile_expr(AnyExprV v, CodeBlob& code, TypePtr target_type = nullptr, LValContext* lval_ctx = nullptr);
std::vector<var_idx_t> pre_compile_is_type(CodeBlob& code, TypePtr expr_type, TypePtr cmp_type, const std::vector<var_idx_t>& expr_ir_idx, AnyV origin, const char* debug_desc);
std::vector<var_idx_t> transition_to_target_type(std::vector<var_idx_t>&& rvect, CodeBlob& code, TypePtr original_type, TypePtr target_type, AnyV origin);
std::vector<var_idx_t> gen_inline_fun_call_in_place(CodeBlob& code, TypePtr ret_type, AnyV origin, FunctionPtr f_inlined, AnyExprV self_obj, bool is_before_immediate_return, const std::vector<std::vector<var_idx_t>>& vars_per_arg);

bool is_type_cellT(TypePtr any_type) {
  if (const TypeDataStruct* t_struct = any_type->try_as<TypeDataStruct>()) {
    StructPtr struct_ref = t_struct->struct_ref;
    return struct_ref->is_instantiation_of_generic_struct() && struct_ref->base_struct_ref->name == "Cell";
  }
  return false;
}

// For any type alias, one can declare custom pack/unpack functions:
// > type TelegramString = slice
// > fun TelegramString.packToBuilder(self, mutate b: builder) { ... }
// > fun TelegramString.unpackFromSlice(mutate s: slice): TelegramString { ... }
// It's externally checked in advance that it's declared correctly.
void get_custom_pack_unpack_function(TypePtr receiver_type, FunctionPtr& f_pack, FunctionPtr& f_unpack) {
  f_pack = f_unpack = nullptr;
  if (const TypeDataAlias* t_alias = receiver_type->try_as<TypeDataAlias>()) {
    if (t_alias->alias_ref->is_instantiation_of_generic_alias()) {
      // does not work for generic aliases currently, because `MyAlias<ConcreteT>.pack` was not instantiated earlier
      return;
    }
    std::string receiver_name = t_alias->alias_ref->as_human_readable();
    if (const Symbol* sym = lookup_global_symbol(receiver_name + ".packToBuilder")) {
      f_pack = sym->try_as<FunctionPtr>();
    }
    if (const Symbol* sym = lookup_global_symbol(receiver_name + ".unpackFromSlice")) {
      f_unpack = sym->try_as<FunctionPtr>();                                           
    }
  }
}

// --------------------------------------------
//    options, context, common helpers
//
// some of the referenced functions are built-in, some are declared in stdlib
// serialization assumes that stdlib exists and is loaded correctly
//


PackContext::PackContext(CodeBlob& code, AnyV origin, std::vector<var_idx_t> ir_builder, const std::vector<var_idx_t>& ir_options)
  : code(code)
  , origin(origin)
  , f_storeInt(lookup_function("builder.storeInt"))
  , f_storeUint(lookup_function("builder.storeUint"))
  , ir_builder(std::move(ir_builder))
  , ir_builder0(this->ir_builder[0])
  , option_skipBitsNValidation(ir_options[0]) {
}

void PackContext::storeInt(var_idx_t ir_idx, int len) const {
  std::vector args = { ir_builder0, ir_idx, code.create_int(origin, len, "(storeW)") };
  code.emplace_back(origin, Op::_Call, ir_builder, std::move(args), f_storeInt);
}

void PackContext::storeUint(var_idx_t ir_idx, int len) const {
  std::vector args = { ir_builder0, ir_idx, code.create_int(origin, len, "(storeW)") };
  code.emplace_back(origin, Op::_Call, ir_builder, std::move(args), f_storeUint);
}

void PackContext::storeUint_var(var_idx_t ir_idx, var_idx_t ir_len) const {
  std::vector args = { ir_builder0, ir_idx, ir_len };
  code.emplace_back(origin, Op::_Call, ir_builder, std::move(args), f_storeUint);
}

void PackContext::storeBool(var_idx_t ir_idx) const {
  std::vector args = { ir_builder0, ir_idx };
  code.emplace_back(origin, Op::_Call, ir_builder, std::move(args), lookup_function("builder.storeBool"));
}

void PackContext::storeCoins(var_idx_t ir_idx) const {
  std::vector args = { ir_builder0, ir_idx };
  code.emplace_back(origin, Op::_Call, ir_builder, std::move(args), lookup_function("builder.storeCoins"));
}

void PackContext::storeRef(var_idx_t ir_idx) const {
  std::vector args = { ir_builder0, ir_idx };
  code.emplace_back(origin, Op::_Call, ir_builder, std::move(args), lookup_function("builder.storeRef"));
}

void PackContext::storeMaybeRef(var_idx_t ir_idx) const {
  std::vector args = { ir_builder0, ir_idx };
  code.emplace_back(origin, Op::_Call, ir_builder, std::move(args), lookup_function("builder.storeMaybeRef"));
}

void PackContext::storeAddressInt(var_idx_t ir_idx) const {
  std::vector args = { ir_builder0, ir_idx };
  code.emplace_back(origin, Op::_Call, ir_builder, std::move(args), lookup_function("builder.storeAddress"));
}

void PackContext::storeAddressAny(var_idx_t ir_idx) const {
  std::vector args = { ir_builder0, ir_idx };
  code.emplace_back(origin, Op::_Call, ir_builder, std::move(args), lookup_function("builder.storeAddressAny"));
}

void PackContext::storeBuilder(var_idx_t ir_idx) const {
  std::vector args = { ir_builder0, ir_idx };
  code.emplace_back(origin, Op::_Call, ir_builder, std::move(args), lookup_function("builder.storeBuilder"));
}

void PackContext::storeSlice(var_idx_t ir_idx) const {
  std::vector args = { ir_builder0, ir_idx };
  code.emplace_back(origin, Op::_Call, ir_builder, std::move(args), lookup_function("builder.storeSlice"));
}

void PackContext::storeOpcode(PackOpcode opcode) const {
  std::vector args = { ir_builder0, code.create_int(origin, opcode.pack_prefix, "(struct-prefix)"), code.create_int(origin, opcode.prefix_len, "(storeW)") };
  code.emplace_back(origin, Op::_Call, ir_builder, std::move(args), f_storeUint);
}


UnpackContext::UnpackContext(CodeBlob& code, AnyV origin, std::vector<var_idx_t> ir_slice, const std::vector<var_idx_t>& ir_options)
  : code(code)
  , origin(origin)
  , f_loadInt(lookup_function("slice.loadInt"))
  , f_loadUint(lookup_function("slice.loadUint"))
  , f_skipBits(lookup_function("slice.skipBits"))
  , ir_slice(std::move(ir_slice))
  , ir_slice0(this->ir_slice[0])
  , option_assertEndAfterReading(ir_options[0])
  , option_throwIfOpcodeDoesNotMatch(ir_options[1]) {
}

std::vector<var_idx_t> UnpackContext::loadInt(int len, const char* debug_desc) const {
  std::vector args = { ir_slice0, code.create_int(origin, len, "(loadW)") };
  std::vector result = code.create_tmp_var(TypeDataInt::create(), origin, debug_desc);
  code.emplace_back(origin, Op::_Call, std::vector{ir_slice0, result[0]}, std::move(args), f_loadInt);
  return result;
}

std::vector<var_idx_t> UnpackContext::loadUint(int len, const char* debug_desc) const {
  std::vector args = { ir_slice0, code.create_int(origin, len, "(loadW)") };
  std::vector result = code.create_tmp_var(TypeDataInt::create(), origin, debug_desc);
  code.emplace_back(origin, Op::_Call, std::vector{ir_slice0, result[0]}, std::move(args), f_loadUint);
  return result;
}

void UnpackContext::loadAndCheckOpcode(PackOpcode opcode) const {
  std::vector ir_prefix_eq = code.create_tmp_var(TypeDataInt::create(), origin, "(prefix-eq)");
  std::vector args = { ir_slice0, code.create_int(origin, opcode.pack_prefix, "(pack-prefix)"), code.create_int(origin, opcode.prefix_len, "(prefix-len)") };
  code.emplace_back(origin, Op::_Call, std::vector{ir_slice0, ir_prefix_eq[0]}, std::move(args), lookup_function("slice.tryStripPrefix"));
  std::vector args_throwifnot = { option_throwIfOpcodeDoesNotMatch, ir_prefix_eq[0] };
  Op& op_assert = code.emplace_back(origin, Op::_Call, std::vector<var_idx_t>{}, std::move(args_throwifnot), lookup_function("__throw_ifnot"));
  op_assert.set_impure_flag();
}

void UnpackContext::skipBits(int len) const {
  std::vector args = { ir_slice0, code.create_int(origin, len, "(skipW)") };
  code.emplace_back(origin, Op::_Call, ir_slice, std::move(args), f_skipBits);
}

void UnpackContext::skipBits_var(var_idx_t ir_len) const {
  std::vector args = { ir_slice0, ir_len };
  code.emplace_back(origin, Op::_Call, ir_slice, std::move(args), f_skipBits);
}

void UnpackContext::assertEndIfOption() const {
  Op& if_assertEnd = code.emplace_back(origin, Op::_If, std::vector{option_assertEndAfterReading});
  {
    code.push_set_cur(if_assertEnd.block0);
    Op& op_ends = code.emplace_back(origin, Op::_Call, std::vector<var_idx_t>{}, ir_slice, lookup_function("slice.assertEnd"));
    op_ends.set_impure_flag();
    code.close_pop_cur(origin);
  }
  {
    code.push_set_cur(if_assertEnd.block1);
    code.close_pop_cur(origin);
  }
}

void UnpackContext::throwInvalidOpcode() const {
  std::vector args_throw = { option_throwIfOpcodeDoesNotMatch };
  Op& op_throw = code.emplace_back(origin, Op::_Call, std::vector<var_idx_t>{}, std::move(args_throw), lookup_function("__throw"));
  op_throw.set_impure_flag();
}

const LazyMatchOptions::MatchBlock* LazyMatchOptions::find_match_block(TypePtr variant) const {
  for (const MatchBlock& b : match_blocks) {
    if (b.arm_variant->equal_to(variant)) {
      return &b;
    }
  }
  tolk_assert(false);
}

void LazyMatchOptions::save_match_result_on_arm_end(CodeBlob& code, AnyV origin, const MatchBlock* arm_block, std::vector<var_idx_t>&& ir_arm_result, const std::vector<var_idx_t>& ir_match_expr_result) const {
  if (!is_statement) {
    // if it's `match` expression (not statement), then every arm has a result, assigned to a whole `match` result
    ir_arm_result = transition_to_target_type(std::move(ir_arm_result), code, arm_block->block_expr_type, match_expr_type, origin);
    code.emplace_back(origin, Op::_Let, ir_match_expr_result, std::move(ir_arm_result));
  } else if (add_return_to_all_arms) {
    // if it's `match` statement, even if an arm is an expression, it's void, actually
    // moreover, if it's the last statement in a function, add implicit "return" to all match cases to produce IFJMP
    code.emplace_back(origin, Op::_Return);
  }
}


// --------------------------------------------
//    serializers with pack/unpack/skip/estimate
//
// for every struct field, for every atomic type, a corresponding (de)serialization instruction is generated
// we generate IR code (Ops), not ASM directly; so, all later IR analysis will later take place
// some of them are straightforward, e.g., call a predefined function for intN and coins
// some are complicated, e.g., for Either we should check a union type at runtime while packing,
// and while unpacking, read a prefix, follow different branches, and construct a resulting union
//


struct ISerializer {
  virtual ~ISerializer() = default;

  virtual void pack(const PackContext* ctx, CodeBlob& code, AnyV origin, std::vector<var_idx_t>&& rvect) = 0;
  virtual std::vector<var_idx_t> unpack(const UnpackContext* ctx, CodeBlob& code, AnyV origin) = 0;

  virtual void skip(const UnpackContext* ctx, CodeBlob& code, AnyV origin) = 0;
  virtual PackSize estimate(const EstimateContext* ctx) = 0;
};

struct S_IntN final : ISerializer {
  const int n_bits;
  const bool is_unsigned;

  explicit S_IntN(int n_bits, bool is_unsigned)
    : n_bits(n_bits), is_unsigned(is_unsigned) {}

  void pack(const PackContext* ctx, CodeBlob& code, AnyV origin, std::vector<var_idx_t>&& rvect) override {
    if (is_unsigned) {
      ctx->storeUint(rvect[0], n_bits);
    } else {
      ctx->storeInt(rvect[0], n_bits);
    }
  }

  std::vector<var_idx_t> unpack(const UnpackContext* ctx, CodeBlob& code, AnyV origin) override {
    if (is_unsigned) {
      return ctx->loadUint(n_bits, "(loaded-uint)");
    } else {
      return ctx->loadInt(n_bits, "(loaded-int)");
    }
  }

  void skip(const UnpackContext* ctx, CodeBlob& code, AnyV origin) override {
    ctx->skipBits(n_bits);
  }

  PackSize estimate(const EstimateContext* ctx) override {
    return PackSize(n_bits);
  }
};

struct S_VariadicIntN final : ISerializer {
  const int n_bits;           // only 16 and 32 available
  const bool is_unsigned;

  explicit S_VariadicIntN(int n_bits, bool is_unsigned)
    : n_bits(n_bits), is_unsigned(is_unsigned) {}

  void pack(const PackContext* ctx, CodeBlob& code, AnyV origin, std::vector<var_idx_t>&& rvect) override {
    FunctionPtr f_storeVarInt = lookup_function("builder.__storeVarInt");
    std::vector args = { ctx->ir_builder0, rvect[0], code.create_int(origin, n_bits, "(n-bits)"), code.create_int(origin, is_unsigned, "(is-unsigned)") };
    code.emplace_back(origin, Op::_Call, ctx->ir_builder, std::move(args), f_storeVarInt);
  }

  std::vector<var_idx_t> unpack(const UnpackContext* ctx, CodeBlob& code, AnyV origin) override {
    FunctionPtr f_loadVarInt = lookup_function("slice.__loadVarInt");
    std::vector args = { ctx->ir_slice0, code.create_int(origin, n_bits, "(n-bits)"), code.create_int(origin, is_unsigned, "(is-unsigned)") };
    std::vector result = code.create_tmp_var(TypeDataInt::create(), origin, "(loaded-varint)");
    code.emplace_back(origin, Op::_Call, std::vector{ctx->ir_slice0, result[0]}, std::move(args), f_loadVarInt);
    return result;
  }

  void skip(const UnpackContext* ctx, CodeBlob& code, AnyV origin) override {
    // no TVM instruction to skip, just load but don't use the result
    unpack(ctx, code, origin);
  }

  PackSize estimate(const EstimateContext* ctx) override {
    if (n_bits == 32) {
      return PackSize(5, 253);
    } else {
      return PackSize(4, 124);    // same as `coins`
    }
  }
};

struct S_BitsN final : ISerializer {
  const int n_bits;

  explicit S_BitsN(int n_width, bool is_bits)
    : n_bits(is_bits ? n_width : n_width * 8) {}

  void pack(const PackContext* ctx, CodeBlob& code, AnyV origin, std::vector<var_idx_t>&& rvect) override {
    tolk_assert(rvect.size() == 1);

    Op& if_disabled_by_user = code.emplace_back(origin, Op::_If, std::vector{ctx->option_skipBitsNValidation});
    {
      code.push_set_cur(if_disabled_by_user.block0);
      code.close_pop_cur(origin);
    }
    {
      code.push_set_cur(if_disabled_by_user.block1);
      constexpr int EXCNO = 9;

      std::vector ir_counts = code.create_tmp_var(TypeDataTensor::create({TypeDataInt::create(), TypeDataInt::create()}), origin, "(slice-size)");
      code.emplace_back(origin, Op::_Call, ir_counts, rvect, lookup_function("slice.remainingBitsAndRefsCount"));
      std::vector args_throwif = { code.create_int(origin, EXCNO, "(excno)"), ir_counts[1] };
      Op& op_assert0 = code.emplace_back(origin, Op::_Call, std::vector<var_idx_t>{}, std::move(args_throwif), lookup_function("__throw_if"));
      op_assert0.set_impure_flag();
      std::vector ir_eq_n = code.create_tmp_var(TypeDataInt::create(), origin, "(eq-n)");
      code.emplace_back(origin, Op::_Call, ir_eq_n, std::vector{ir_counts[0], code.create_int(origin, n_bits, "(n-bits)")}, lookup_function("_==_"));
      std::vector args_throwifnot = { code.create_int(origin, EXCNO, "(excno)"), ir_eq_n[0] };
      Op& op_assertN = code.emplace_back(origin, Op::_Call, std::vector<var_idx_t>{}, std::move(args_throwifnot), lookup_function("__throw_ifnot"));
      op_assertN.set_impure_flag();
      code.close_pop_cur(origin);
    }

    ctx->storeSlice(rvect[0]);
  }

  std::vector<var_idx_t> unpack(const UnpackContext* ctx, CodeBlob& code, AnyV origin) override {
    FunctionPtr f_loadBits = lookup_function("slice.loadBits");
    std::vector args = { ctx->ir_slice0, code.create_int(origin, n_bits, "(loadW)") };
    std::vector ir_result = code.create_tmp_var(TypeDataSlice::create(), origin, "(loaded-slice)");
    code.emplace_back(origin, Op::_Call, std::vector{ctx->ir_slice0, ir_result[0]}, std::move(args), f_loadBits);
    return ir_result;
  }

  void skip(const UnpackContext* ctx, CodeBlob& code, AnyV origin) override {
    ctx->skipBits(n_bits);
  }

  PackSize estimate(const EstimateContext* ctx) override {
    return PackSize(n_bits);
  }
};

struct S_Bool final : ISerializer {
  void pack(const PackContext* ctx, CodeBlob& code, AnyV origin, std::vector<var_idx_t>&& rvect) override {
    tolk_assert(rvect.size() == 1);
    ctx->storeBool(rvect[0]);
  }

  std::vector<var_idx_t> unpack(const UnpackContext* ctx, CodeBlob& code, AnyV origin) override {
    return ctx->loadInt(1, "(loaded-bool)");
  }

  void skip(const UnpackContext* ctx, CodeBlob& code, AnyV origin) override {
    ctx->skipBits(1);
  }

  PackSize estimate(const EstimateContext* ctx) override {
    return PackSize(1);
  }
};

struct S_RawTVMcell final : ISerializer {
  void pack(const PackContext* ctx, CodeBlob& code, AnyV origin, std::vector<var_idx_t>&& rvect) override {
    tolk_assert(rvect.size() == 1);
    ctx->storeRef(rvect[0]);
  }

  std::vector<var_idx_t> unpack(const UnpackContext* ctx, CodeBlob& code, AnyV origin) override {
    FunctionPtr f_loadRef = lookup_function("slice.loadRef");
    std::vector args = ctx->ir_slice;
    std::vector ir_result = code.create_tmp_var(TypeDataCell::create(), origin, "(loaded-cell)");
    code.emplace_back(origin, Op::_Call, std::vector{ctx->ir_slice0, ir_result[0]}, std::move(args), f_loadRef);
    return ir_result;
  }

  void skip(const UnpackContext* ctx, CodeBlob& code, AnyV origin) override {
    FunctionPtr f_loadRef = lookup_function("slice.loadRef");
    std::vector args = ctx->ir_slice;
    std::vector dummy_loaded = code.create_tmp_var(TypeDataCell::create(), origin, "(loaded-cell)");
    code.emplace_back(origin, Op::_Call, std::vector{ctx->ir_slice0, dummy_loaded[0]}, std::move(args), f_loadRef);
  }

  PackSize estimate(const EstimateContext* ctx) override {
    return PackSize(0, 0, 1, 1);
  }
};

struct S_RawTVMcellOrNull final : ISerializer {
  void pack(const PackContext* ctx, CodeBlob& code, AnyV origin, std::vector<var_idx_t>&& rvect) override {
    tolk_assert(rvect.size() == 1);
    ctx->storeMaybeRef(rvect[0]);
  }

  std::vector<var_idx_t> unpack(const UnpackContext* ctx, CodeBlob& code, AnyV origin) override {
    FunctionPtr f_loadMaybeRef = lookup_function("slice.loadMaybeRef");
    std::vector args = ctx->ir_slice;
    std::vector ir_result = code.create_tmp_var(TypeDataCell::create(), origin, "(loaded-cell)");
    code.emplace_back(origin, Op::_Call, std::vector{ctx->ir_slice0, ir_result[0]}, std::move(args), f_loadMaybeRef);
    return ir_result;
  }

  void skip(const UnpackContext* ctx, CodeBlob& code, AnyV origin) override {
    FunctionPtr f_skipMaybeRef = lookup_function("slice.skipMaybeRef");
    code.emplace_back(origin, Op::_Call, ctx->ir_slice, ctx->ir_slice, f_skipMaybeRef);
  }

  PackSize estimate(const EstimateContext* ctx) override {
    return PackSize(1, 1, 0, 1);
  }
};

struct S_Coins final : ISerializer {
  void pack(const PackContext* ctx, CodeBlob& code, AnyV origin, std::vector<var_idx_t>&& rvect) override {
    tolk_assert(rvect.size() == 1);
    ctx->storeCoins(rvect[0]);
  }

  std::vector<var_idx_t> unpack(const UnpackContext* ctx, CodeBlob& code, AnyV origin) override {
    FunctionPtr f_loadCoins = lookup_function("slice.loadCoins");
    std::vector args = ctx->ir_slice;
    std::vector ir_result = code.create_tmp_var(TypeDataInt::create(), origin, "(loaded-coins)");
    code.emplace_back(origin, Op::_Call, std::vector{ctx->ir_slice0, ir_result[0]}, std::move(args), f_loadCoins);
    return ir_result;
  }

  void skip(const UnpackContext* ctx, CodeBlob& code, AnyV origin) override {
    // no TVM instruction to skip, just load but don't use the result
    unpack(ctx, code, origin);
  }

  PackSize estimate(const EstimateContext* ctx) override {
    return PackSize(4, 124);
  }
};

struct S_AddressInt final : ISerializer {
  void pack(const PackContext* ctx, CodeBlob& code, AnyV origin, std::vector<var_idx_t>&& rvect) override {
    tolk_assert(rvect.size() == 1);
    ctx->storeAddressInt(rvect[0]);
  }

  std::vector<var_idx_t> unpack(const UnpackContext* ctx, CodeBlob& code, AnyV origin) override {
    FunctionPtr f_loadAddress = lookup_function("slice.loadAddress");
    std::vector ir_address = code.create_tmp_var(TypeDataSlice::create(), origin, "(loaded-addr)");
    code.emplace_back(origin, Op::_Call, std::vector{ctx->ir_slice0, ir_address[0]}, ctx->ir_slice, f_loadAddress);
    return ir_address;
  }

  void skip(const UnpackContext* ctx, CodeBlob& code, AnyV origin) override {
    // not just skip 267 bits: do real unpack, so that an address is validated to be internal (not none, etc.)
    unpack(ctx, code, origin);
  }

  PackSize estimate(const EstimateContext* ctx) override {
    PackSize size = PackSize(3 + 8 + 256);   // '10' + 0 (anycast disabled) + workchain + hash
    size.skipping_is_dangerous = true;    // can't just "skip bits267" when unused in lazy
    return size;
  }
};

struct S_AddressIntOrNull final : ISerializer {
  void pack(const PackContext* ctx, CodeBlob& code, AnyV origin, std::vector<var_idx_t>&& rvect) override {
    // `address?`, when null, is stored as '00' (addr_none), so `address?` is not TL/B (Maybe MsgAddressInt)
    tolk_assert(rvect.size() == 1);
    std::vector args = { ctx->ir_builder0, rvect[0] };
    code.emplace_back(origin, Op::_Call, ctx->ir_builder, std::move(args), lookup_function("builder.storeAddressOpt"));
  }

  std::vector<var_idx_t> unpack(const UnpackContext* ctx, CodeBlob& code, AnyV origin) override {
    FunctionPtr f_loadAddressOpt = lookup_function("slice.loadAddressOpt");
    std::vector ir_address_orN = code.create_tmp_var(TypeDataSlice::create(), origin, "(loaded-addr)");
    code.emplace_back(origin, Op::_Call, std::vector{ctx->ir_slice0, ir_address_orN[0]}, ctx->ir_slice, f_loadAddressOpt);
    return ir_address_orN;
  }

  void skip(const UnpackContext* ctx, CodeBlob& code, AnyV origin) override {
    unpack(ctx, code, origin);
  }

  PackSize estimate(const EstimateContext* ctx) override {
    return PackSize(2, 3 + 8 + 256);
  }
};

struct S_AddressAny final : ISerializer {
  void pack(const PackContext* ctx, CodeBlob& code, AnyV origin, std::vector<var_idx_t>&& rvect) override {
    tolk_assert(rvect.size() == 1);
    ctx->storeAddressAny(rvect[0]);
  }

  std::vector<var_idx_t> unpack(const UnpackContext* ctx, CodeBlob& code, AnyV origin) override {
    FunctionPtr f_loadAddressAny = lookup_function("slice.loadAddressAny");
    std::vector ir_address_any = code.create_tmp_var(TypeDataSlice::create(), origin, "(loaded-addr)");
    code.emplace_back(origin, Op::_Call, std::vector{ctx->ir_slice0, ir_address_any[0]}, ctx->ir_slice, f_loadAddressAny);
    return ir_address_any;
  }

  void skip(const UnpackContext* ctx, CodeBlob& code, AnyV origin) override {
    unpack(ctx, code, origin);
  }

  PackSize estimate(const EstimateContext* ctx) override {
    return PackSize(2, 2 + 9 + 512);    // an extern address could be really large
  }
};

struct S_RemainingBitsAndRefs final : ISerializer {
  void pack(const PackContext* ctx, CodeBlob& code, AnyV origin, std::vector<var_idx_t>&& rvect) override {
    tolk_assert(rvect.size() == 1);
    ctx->storeSlice(rvect[0]);
  }

  std::vector<var_idx_t> unpack(const UnpackContext* ctx, CodeBlob& code, AnyV origin) override {
    std::vector ir_rem_slice = code.create_tmp_var(TypeDataSlice::create(), origin, "(remainder)");
    code.emplace_back(origin, Op::_Let, ir_rem_slice, ctx->ir_slice);
    code.emplace_back(origin, Op::_Call, ctx->ir_slice, std::vector<var_idx_t>{}, lookup_function("createEmptySlice"));
    return ir_rem_slice;
  }

  void skip(const UnpackContext* ctx, CodeBlob& code, AnyV origin) override {
    unpack(ctx, code, origin);
  }

  PackSize estimate(const EstimateContext* ctx) override {
    return PackSize::unpredictable_infinity();
  }
};

struct S_Builder final : ISerializer {
  void pack(const PackContext* ctx, CodeBlob& code, AnyV origin, std::vector<var_idx_t>&& rvect) override {
    tolk_assert(rvect.size() == 1);
    std::vector args = { ctx->ir_builder0, rvect[0] };
    code.emplace_back(origin, Op::_Call, ctx->ir_builder, std::move(args), lookup_function("builder.storeBuilder"));
  }

  std::vector<var_idx_t> unpack(const UnpackContext* ctx, CodeBlob& code, AnyV origin) override {
    tolk_assert(false);   // `builder` can only be used for writing, checked earlier
  }

  void skip(const UnpackContext* ctx, CodeBlob& code, AnyV origin) override {
    tolk_assert(false);   // `builder` can only be used for writing, checked earlier
  }

  PackSize estimate(const EstimateContext* ctx) override {
    return PackSize::unpredictable_infinity();
  }
};

struct S_Slice final : ISerializer {
  void pack(const PackContext* ctx, CodeBlob& code, AnyV origin, std::vector<var_idx_t>&& rvect) override {
    tolk_assert(rvect.size() == 1);
    ctx->storeSlice(rvect[0]);
  }

  std::vector<var_idx_t> unpack(const UnpackContext* ctx, CodeBlob& code, AnyV origin) override {
    tolk_assert(false);   // `slice` can only be used for writing, checked earlier
  }

  void skip(const UnpackContext* ctx, CodeBlob& code, AnyV origin) override {
    tolk_assert(false);   // `slice` can only be used for writing, checked earlier
  }

  PackSize estimate(const EstimateContext* ctx) override {
    return PackSize::unpredictable_infinity();
  }
};

struct S_Null final : ISerializer {
  void pack(const PackContext* ctx, CodeBlob& code, AnyV origin, std::vector<var_idx_t>&& rvect) override {
    // while `null` itself is not serializable, it may be contained inside a union:
    // `int32 | int64 | null`, for example;
    // then the compiler generates prefixes for every variant, and `null` variant does nothing
  }

  std::vector<var_idx_t> unpack(const UnpackContext* ctx, CodeBlob& code, AnyV origin) override {
    std::vector ir_null = code.create_tmp_var(TypeDataNullLiteral::create(), origin, "(null)");
    code.emplace_back(origin, Op::_Call, ir_null, std::vector<var_idx_t>{}, lookup_function("__null"));
    return ir_null;
  }

  void skip(const UnpackContext* ctx, CodeBlob& code, AnyV origin) override {
  }

  PackSize estimate(const EstimateContext* ctx) override {
    return PackSize(0);
  }
};

struct S_Void final : ISerializer {
  void pack(const PackContext* ctx, CodeBlob& code, AnyV origin, std::vector<var_idx_t>&& rvect) override {
    tolk_assert(rvect.empty());
  }

  std::vector<var_idx_t> unpack(const UnpackContext* ctx, CodeBlob& code, AnyV origin) override {
    return {};
  }

  void skip(const UnpackContext* ctx, CodeBlob& code, AnyV origin) override {
  }

  PackSize estimate(const EstimateContext* ctx) override {
    return PackSize(0);
  }
};

struct S_Maybe final : ISerializer {
  const TypeDataUnion* t_union;
  TypePtr or_null;

  explicit S_Maybe(const TypeDataUnion* t_union)
    : t_union(t_union)
    , or_null(t_union->or_null) {
  }

  void pack(const PackContext* ctx, CodeBlob& code, AnyV origin, std::vector<var_idx_t>&& rvect) override {
    std::vector ir_is_null = pre_compile_is_type(code, t_union, TypeDataNullLiteral::create(), rvect, origin, "(is-null)");
    Op& if_op = code.emplace_back(origin, Op::_If, ir_is_null);
    {
      code.push_set_cur(if_op.block0);
      ctx->storeUint(code.create_int(origin, 0, "(maybeBit)"), 1);
      code.close_pop_cur(origin);
    }
    {
      code.push_set_cur(if_op.block1);
      ctx->storeUint(code.create_int(origin, 1, "(maybeBit)"), 1);
      rvect = transition_to_target_type(std::move(rvect), code, t_union, t_union->or_null, origin);
      ctx->generate_pack_any(t_union->or_null, std::move(rvect));
      code.close_pop_cur(origin);
    }
  }

  std::vector<var_idx_t> unpack(const UnpackContext* ctx, CodeBlob& code, AnyV origin) override {
    std::vector ir_result = code.create_tmp_var(t_union, origin, "(loaded-maybe)");
    std::vector ir_not_null = { ctx->loadUint(1, "(maybeBit)") };
    Op& if_op = code.emplace_back(origin, Op::_If, std::move(ir_not_null));
    {
      code.push_set_cur(if_op.block0);
      std::vector rvect_maybe = ctx->generate_unpack_any(t_union->or_null);
      rvect_maybe = transition_to_target_type(std::move(rvect_maybe), code, t_union->or_null, t_union, origin);
      code.emplace_back(origin, Op::_Let, ir_result, std::move(rvect_maybe));
      code.close_pop_cur(origin);
    }
    {
      code.push_set_cur(if_op.block1);
      std::vector rvect_null = code.create_tmp_var(TypeDataNullLiteral::create(), origin, "(maybe-null)");
      code.emplace_back(origin, Op::_Call, rvect_null, std::vector<var_idx_t>{}, lookup_function("__null"));
      rvect_null = transition_to_target_type(std::move(rvect_null), code, TypeDataNullLiteral::create(), t_union, origin);
      code.emplace_back(origin, Op::_Let, ir_result, std::move(rvect_null));
      code.close_pop_cur(origin);
    }
    return ir_result;
  }

  void skip(const UnpackContext* ctx, CodeBlob& code, AnyV origin) override {
    std::vector ir_not_null = { ctx->loadUint(1, "(maybeBit)") };
    Op& if_op = code.emplace_back(origin, Op::_If, std::move(ir_not_null));
    {
      code.push_set_cur(if_op.block0);
      ctx->generate_skip_any(t_union->or_null);
      code.close_pop_cur(origin);
    }
    {
      code.push_set_cur(if_op.block1);
      code.close_pop_cur(origin);
    }
  }

  PackSize estimate(const EstimateContext* ctx) override {
    PackSize maybe_size = ctx->estimate_any(t_union->or_null);
    return PackSize(1, 1 + maybe_size.max_bits, 0, maybe_size.max_refs);
  }
};

struct S_Either final : ISerializer {
  const TypeDataUnion* t_union;
  TypePtr t_left;
  TypePtr t_right;

  explicit S_Either(const TypeDataUnion* t_union)
    : t_union(t_union)
    , t_left(t_union->variants[0])
    , t_right(t_union->variants[1]) {
  }

  void pack(const PackContext* ctx, CodeBlob& code, AnyV origin, std::vector<var_idx_t>&& rvect) override {
    std::vector ir_is_right = pre_compile_is_type(code, t_union, t_right, rvect, origin, "(is-right)");
    Op& if_op = code.emplace_back(origin, Op::_If, ir_is_right);
    {
      code.push_set_cur(if_op.block0);
      ctx->storeUint(code.create_int(origin, 1, "(eitherBit)"), 1);
      std::vector rvect_right = transition_to_target_type(std::vector(rvect), code, t_union, t_right, origin);
      ctx->generate_pack_any(t_right, std::move(rvect_right));
      code.close_pop_cur(origin);
    }
    {
      code.push_set_cur(if_op.block1);
      ctx->storeUint(code.create_int(origin, 0, "(eitherBit)"), 1);
      std::vector rvect_left = transition_to_target_type(std::move(rvect), code, t_union, t_left, origin);
      ctx->generate_pack_any(t_left, std::move(rvect_left));
      code.close_pop_cur(origin);
    }
  }

  std::vector<var_idx_t> unpack(const UnpackContext* ctx, CodeBlob& code, AnyV origin) override {
    std::vector ir_result = code.create_tmp_var(t_union, origin, "(loaded-either)");
    std::vector ir_is_right = ctx->loadUint(1, "(eitherBit)");
    Op& if_op = code.emplace_back(origin, Op::_If, std::move(ir_is_right));
    {
      code.push_set_cur(if_op.block0);
      std::vector rvect_right = ctx->generate_unpack_any(t_right);
      rvect_right = transition_to_target_type(std::move(rvect_right), code, t_right, t_union, origin);
      code.emplace_back(origin, Op::_Let, ir_result, std::move(rvect_right));
      code.close_pop_cur(origin);
    }
    {
      code.push_set_cur(if_op.block1);
      std::vector rvect_left = ctx->generate_unpack_any(t_left);
      rvect_left = transition_to_target_type(std::move(rvect_left), code, t_left, t_union, origin);
      code.emplace_back(origin, Op::_Let, ir_result, std::move(rvect_left));
      code.close_pop_cur(origin);
    }
    return ir_result;
  }

  std::vector<var_idx_t> lazy_match(const UnpackContext* ctx, CodeBlob& code, AnyV origin, const LazyMatchOptions& options) const {
    for (const LazyMatchOptions::MatchBlock& m : options.match_blocks) {
      if (m.arm_variant == nullptr) {   // `else => ...` not allowed for Either
        // it's not the best place to fire an error, but let it be
        err("`else` is unreachable, because this `match` has only two options (0/1 prefixes)").fire(SrcRange::empty_at_start(m.v_body->range));
      }
    }
    tolk_assert(options.match_blocks.size() == 2);
    std::vector ir_result = code.create_tmp_var(options.match_expr_type, origin, "(match-expression)");
    std::vector ir_is_right = ctx->loadUint(1, "(eitherBit)");
    Op& if_op = code.emplace_back(origin, Op::_If, std::move(ir_is_right));
    {
      code.push_set_cur(if_op.block0);
      const LazyMatchOptions::MatchBlock* m_block = options.find_match_block(t_right);
      std::vector ith_result = pre_compile_expr(m_block->v_body, code);
      options.save_match_result_on_arm_end(code, origin, m_block, std::move(ith_result), ir_result);
      code.close_pop_cur(origin);
    }
    {
      code.push_set_cur(if_op.block1);
      const LazyMatchOptions::MatchBlock* m_block = options.find_match_block(t_left);
      std::vector ith_result = pre_compile_expr(m_block->v_body, code);
      options.save_match_result_on_arm_end(code, origin, m_block, std::move(ith_result), ir_result);
      code.close_pop_cur(origin);
    }
    return ir_result;
  }

  void skip(const UnpackContext* ctx, CodeBlob& code, AnyV origin) override {
    std::vector ir_is_right = ctx->loadUint(1, "(eitherBit)");
    Op& if_op = code.emplace_back(origin, Op::_If, std::move(ir_is_right));
    {
      code.push_set_cur(if_op.block0);
      ctx->generate_skip_any(t_right);
      code.close_pop_cur(origin);
    }
    {
      code.push_set_cur(if_op.block1);
      ctx->generate_skip_any(t_left);
      code.close_pop_cur(origin);
    }
  }

  PackSize estimate(const EstimateContext* ctx) override {
    PackSize either_size = EstimateContext::minmax(ctx->estimate_any(t_left), ctx->estimate_any(t_right));
    return EstimateContext::sum(PackSize(1), either_size);
  }
};

struct S_MultipleConstructors final : ISerializer {
  const TypeDataUnion* t_union;
  std::vector<PackOpcode> opcodes;

  explicit S_MultipleConstructors(const TypeDataUnion* t_union, std::vector<PackOpcode>&& opcodes)
    : t_union(t_union)
    , opcodes(std::move(opcodes)) {
    tolk_assert(this->opcodes.size() == t_union->variants.size());
  }

  void pack(const PackContext* ctx, CodeBlob& code, AnyV origin, std::vector<var_idx_t>&& rvect) override {
    for (int i = 0; i < t_union->size() - 1; ++i) {
      TypePtr variant = t_union->variants[i];
      std::vector ir_eq_ith = pre_compile_is_type(code, t_union, variant, rvect, origin, "(arm-cond-eq)");
      Op& if_op = code.emplace_back(origin, Op::_If, std::move(ir_eq_ith));
      code.push_set_cur(if_op.block0);
      std::vector ith_rvect = transition_to_target_type(std::vector(rvect), code, t_union, variant, origin);
      ctx->storeUint(code.create_int(origin, opcodes[i].pack_prefix, "(ith-prefix)"), opcodes[i].prefix_len);
      ctx->generate_pack_any(variant, std::move(ith_rvect), PrefixWriteMode::DoNothingAlreadyWritten);
      code.close_pop_cur(origin);
      code.push_set_cur(if_op.block1);  // open ELSE
    }

    // we're inside the last ELSE
    TypePtr last_variant = t_union->variants.back();
    std::vector last_rvect = transition_to_target_type(std::move(rvect), code, t_union, last_variant, origin);
    ctx->storeUint(code.create_int(origin, opcodes.back().pack_prefix, "(ith-prefix)"), opcodes.back().prefix_len);
    ctx->generate_pack_any(last_variant, std::move(last_rvect), PrefixWriteMode::DoNothingAlreadyWritten);
    for (int i = 0; i < t_union->size() - 1; ++i) {
      code.close_pop_cur(origin);    // close all outer IFs
    }
  }

  std::vector<var_idx_t> unpack(const UnpackContext* ctx, CodeBlob& code, AnyV origin) override {
    // assume that opcodes (either automatically generated or manually specified)
    // form a valid prefix tree, and the order of reading does not matter; we'll definitely match the one;
    FunctionPtr f_tryStripPrefix = lookup_function("slice.tryStripPrefix");

    std::vector ir_result = code.create_tmp_var(t_union, origin, "(loaded-union)");
    std::vector ir_prefix_eq = code.create_tmp_var(TypeDataInt::create(), origin, "(prefix-eq)");

    for (int i = 0; i < t_union->size(); ++i) {
      TypePtr variant = t_union->variants[i];
      std::vector args = { ctx->ir_slice0, code.create_int(origin, opcodes[i].pack_prefix, "(pack-prefix)"), code.create_int(origin, opcodes[i].prefix_len, "(prefix-len)") };
      code.emplace_back(origin, Op::_Call, std::vector{ctx->ir_slice0, ir_prefix_eq[0]}, std::move(args), f_tryStripPrefix);
      Op& if_prefix_eq = code.emplace_back(origin, Op::_If, ir_prefix_eq);
      code.push_set_cur(if_prefix_eq.block0);
      std::vector ith_rvect = ctx->generate_unpack_any(variant, PrefixReadMode::DoNothingAlreadyLoaded);
      ith_rvect = transition_to_target_type(std::move(ith_rvect), code, variant, t_union, origin);
      code.emplace_back(origin, Op::_Let, ir_result, std::move(ith_rvect));
      code.close_pop_cur(origin);
      code.push_set_cur(if_prefix_eq.block1);    // open ELSE
    }

    // we're inside last ELSE
    ctx->throwInvalidOpcode();
    for (int j = 0; j < t_union->size(); ++j) {
      code.close_pop_cur(origin);    // close all outer IFs
    }
    return ir_result;
  }

  std::vector<var_idx_t> lazy_match(const UnpackContext* ctx, CodeBlob& code, AnyV origin, const LazyMatchOptions& options) const {
    std::vector<int> opcodes_order_mapping(t_union->size(), -1);
    const LazyMatchOptions::MatchBlock* else_block = nullptr;
    for (int i = 0; i < static_cast<int>(options.match_blocks.size()); ++i) {
      if (options.match_blocks[i].arm_variant) {
        int variant_idx = t_union->get_variant_idx(options.match_blocks[i].arm_variant);
        tolk_assert(variant_idx != -1);
        opcodes_order_mapping[i] = variant_idx;
      } else {
        tolk_assert(else_block == nullptr);
        else_block = &options.match_blocks[i];
      }
    }

    FunctionPtr f_tryStripPrefix = lookup_function("slice.tryStripPrefix");

    std::vector ir_result = code.create_tmp_var(options.match_expr_type, origin, "(match-expression)");
    std::vector ir_prefix_eq = code.create_tmp_var(TypeDataInt::create(), origin, "(prefix-eq)");

    for (int i = 0; i < t_union->size(); ++i) {
      StructData::PackOpcode opcode = opcodes[opcodes_order_mapping[i]];
      std::vector args = { ctx->ir_slice0, code.create_int(origin, opcode.pack_prefix, "(pack-prefix)"), code.create_int(origin, opcode.prefix_len, "(prefix-len)") };
      code.emplace_back(origin, Op::_Call, std::vector{ctx->ir_slice0, ir_prefix_eq[0]}, std::move(args), f_tryStripPrefix);
      Op& if_op = code.emplace_back(origin, Op::_If, ir_prefix_eq);
      code.push_set_cur(if_op.block0);
      std::vector ith_result = pre_compile_expr(options.match_blocks[i].v_body, code);
      options.save_match_result_on_arm_end(code, origin, &options.match_blocks[i], std::move(ith_result), ir_result);
      code.close_pop_cur(origin);
      code.push_set_cur(if_op.block1);    // open ELSE
    }

    if (else_block) {
      std::vector else_result = pre_compile_expr(else_block->v_body, code);
      options.save_match_result_on_arm_end(code, origin, else_block, std::move(else_result), ir_result);
    } else {
      ctx->throwInvalidOpcode();
    }
    for (int j = 0; j < t_union->size(); ++j) {
      code.close_pop_cur(origin);    // close all outer IFs
    }

    return ir_result;
  }

  void skip(const UnpackContext* ctx, CodeBlob& code, AnyV origin) override {
    FunctionPtr f_tryStripPrefix = lookup_function("slice.tryStripPrefix");
    std::vector ir_prefix_eq = code.create_tmp_var(TypeDataInt::create(), origin, "(prefix-eq)");

    for (int i = 0; i < t_union->size(); ++i) {
      TypePtr variant = t_union->variants[i];
      std::vector args = { ctx->ir_slice0, code.create_int(origin, opcodes[i].pack_prefix, "(pack-prefix)"), code.create_int(origin, opcodes[i].prefix_len, "(prefix-len)") };
      code.emplace_back(origin, Op::_Call, std::vector{ctx->ir_slice0, ir_prefix_eq[0]}, std::move(args), f_tryStripPrefix);
      Op& if_prefix_eq = code.emplace_back(origin, Op::_If, ir_prefix_eq);
      code.push_set_cur(if_prefix_eq.block0);
      ctx->generate_skip_any(variant, PrefixReadMode::DoNothingAlreadyLoaded);
      code.close_pop_cur(origin);
      code.push_set_cur(if_prefix_eq.block1);    // open ELSE
    }

    // we're inside last ELSE
    ctx->throwInvalidOpcode();
    for (int j = 0; j < t_union->size(); ++j) {
      code.close_pop_cur(origin);    // close all outer IFs
    }
  }

  PackSize estimate(const EstimateContext* ctx) override {
    PackSize variants_size = ctx->estimate_any(t_union->variants[0], PrefixEstimateMode::DoNothingAlreadyIncluded);
    PackSize prefix_size(opcodes[0].prefix_len);

    for (int i = 1; i < t_union->size(); ++i) {
      variants_size = EstimateContext::minmax(variants_size, ctx->estimate_any(t_union->variants[i], PrefixEstimateMode::DoNothingAlreadyIncluded));
      prefix_size = EstimateContext::minmax(prefix_size, PackSize(opcodes[i].prefix_len));
    }

    return EstimateContext::sum(variants_size, prefix_size);
  }
};

struct S_Tensor final : ISerializer {
  const TypeDataTensor* t_tensor;

  explicit S_Tensor(const TypeDataTensor* t_tensor)
    : t_tensor(t_tensor) {
  }

  void pack(const PackContext* ctx, CodeBlob& code, AnyV origin, std::vector<var_idx_t>&& rvect) override {
    int stack_offset = 0;
    for (TypePtr item : t_tensor->items) {
      int stack_width = item->get_width_on_stack();
      std::vector item_vars(rvect.begin() + stack_offset, rvect.begin() + stack_offset + stack_width);
      ctx->generate_pack_any(item, std::move(item_vars));
      stack_offset += stack_width;
    }
    tolk_assert(stack_offset == t_tensor->get_width_on_stack());
  }

  std::vector<var_idx_t> unpack(const UnpackContext* ctx, CodeBlob& code, AnyV origin) override {
    std::vector<var_idx_t> tensor_vars;
    tensor_vars.reserve(t_tensor->get_width_on_stack());
    for (TypePtr item : t_tensor->items) {
      std::vector item_vars = ctx->generate_unpack_any(item);
      tensor_vars.insert(tensor_vars.end(), item_vars.begin(), item_vars.end());
    }
    tolk_assert(static_cast<int>(tensor_vars.size()) == t_tensor->get_width_on_stack());
    return tensor_vars;
  }

  void skip(const UnpackContext* ctx, CodeBlob& code, AnyV origin) override {
    for (TypePtr item : t_tensor->items) {
      ctx->generate_skip_any(item);
    }
  }

  PackSize estimate(const EstimateContext* ctx) override {
    PackSize sum = PackSize(0);
    for (TypePtr item : t_tensor->items) {
      sum = EstimateContext::sum(sum, ctx->estimate_any(item));
    }
    return sum;
  }
};

struct S_CustomStruct final : ISerializer {
  StructPtr struct_ref;

  explicit S_CustomStruct(StructPtr struct_ref)
    : struct_ref(struct_ref) {
  }

  void pack(const PackContext* ctx, CodeBlob& code, AnyV origin, std::vector<var_idx_t>&& rvect) override {
    if (struct_ref->opcode.exists() && ctx->get_prefix_mode() == PrefixWriteMode::WritePrefixOfStruct) {
      ctx->storeOpcode(struct_ref->opcode);
    }

    int stack_offset = 0;
    for (StructFieldPtr field_ref : struct_ref->fields) {
      int stack_width = field_ref->declared_type->get_width_on_stack();
      std::vector field_vars(rvect.begin() + stack_offset, rvect.begin() + stack_offset + stack_width);
      ctx->generate_pack_any(field_ref->declared_type, std::move(field_vars));
      stack_offset += stack_width;
    }
    tolk_assert(stack_offset == TypeDataStruct::create(struct_ref)->get_width_on_stack());
  }

  std::vector<var_idx_t> unpack(const UnpackContext* ctx, CodeBlob& code, AnyV origin) override {
    if (struct_ref->opcode.exists() && ctx->get_prefix_mode() == PrefixReadMode::LoadAndCheck) {
      ctx->loadAndCheckOpcode(struct_ref->opcode);
    }

    int total_stack_w = TypeDataStruct::create(struct_ref)->get_width_on_stack();
    std::vector<var_idx_t> ir_struct;
    ir_struct.reserve(total_stack_w);
    for (StructFieldPtr field_ref : struct_ref->fields) {
      std::vector field_vars = ctx->generate_unpack_any(field_ref->declared_type);
      ir_struct.insert(ir_struct.end(), field_vars.begin(), field_vars.end());
    }
    tolk_assert(static_cast<int>(ir_struct.size()) == total_stack_w);
    return ir_struct;
  }

  std::vector<var_idx_t> lazy_match(const UnpackContext* ctx, CodeBlob& code, AnyV origin, const LazyMatchOptions& options) const {
    const LazyMatchOptions::MatchBlock* when_block = nullptr;   // Point => ...
    const LazyMatchOptions::MatchBlock* else_block = nullptr;   // else  => ...
    for (const LazyMatchOptions::MatchBlock& match_block : options.match_blocks) {
      if (match_block.arm_variant) {
        tolk_assert(match_block.arm_variant->equal_to(TypeDataStruct::create(struct_ref)));
        when_block = &match_block;
      } else {
        else_block = &match_block;
      }
    }

    std::vector ir_result = code.create_tmp_var(options.match_expr_type, origin, "(match-expression)");
    std::vector ir_prefix_eq = code.create_tmp_var(TypeDataInt::create(), origin, "(prefix-eq)");

    StructData::PackOpcode opcode = struct_ref->opcode;
    if (opcode.exists()) {    // it's `match` over a struct (makes sense for a struct with prefix and `else` branch)
      std::vector args = { ctx->ir_slice0, code.create_int(origin, opcode.pack_prefix, "(pack-prefix)"), code.create_int(origin, opcode.prefix_len, "(prefix-len)") };
      code.emplace_back(origin, Op::_Call, std::vector{ctx->ir_slice0, ir_prefix_eq[0]}, std::move(args), lookup_function("slice.tryStripPrefix"));
    } else {
      code.emplace_back(origin, Op::_Let, ir_prefix_eq, std::vector{code.create_int(origin, -1, "(true)")});
    }
    Op& if_op = code.emplace_back(origin, Op::_If, ir_prefix_eq);
    {
      code.push_set_cur(if_op.block0);
      std::vector when_result = pre_compile_expr(when_block->v_body, code);
      options.save_match_result_on_arm_end(code, origin, when_block, std::move(when_result), ir_result);
      code.close_pop_cur(origin);
    }
    {
      code.push_set_cur(if_op.block1);
      if (else_block) {
        std::vector else_result = pre_compile_expr(else_block->v_body, code);
        options.save_match_result_on_arm_end(code, origin, else_block, std::move(else_result), ir_result);
      } else {
        ctx->throwInvalidOpcode();
      }
      code.close_pop_cur(origin);
    }

    return ir_result;
  }

  void skip(const UnpackContext* ctx, CodeBlob& code, AnyV origin) override {
    if (struct_ref->opcode.exists() && ctx->get_prefix_mode() == PrefixReadMode::LoadAndCheck) {
      ctx->loadAndCheckOpcode(struct_ref->opcode);
    }

    for (StructFieldPtr field_ref : struct_ref->fields) {
      ctx->generate_skip_any(field_ref->declared_type);
    }
  }

  PackSize estimate(const EstimateContext* ctx) override {
    PackSize sum(0);

    if (struct_ref->opcode.exists() && ctx->get_prefix_mode() == PrefixEstimateMode::IncludePrefixOfStruct) {
      sum = EstimateContext::sum(sum, PackSize(struct_ref->opcode.prefix_len));
    }

    for (StructFieldPtr field_ref : struct_ref->fields) {
      sum = EstimateContext::sum(sum, ctx->estimate_any(field_ref->declared_type));
    }
    return sum;
  }
};

struct S_IntegerEnum final : ISerializer {
  EnumDefPtr enum_ref;

  explicit S_IntegerEnum(EnumDefPtr enum_ref)
    : enum_ref(enum_ref) {
  }

  void pack(const PackContext* ctx, CodeBlob& code, AnyV origin, std::vector<var_idx_t>&& rvect) override {
    TypePtr pack_type = calculate_intN_to_serialize_enum(enum_ref);
    return ctx->generate_pack_any(pack_type, std::move(rvect));
  }

  std::vector<var_idx_t> unpack(const UnpackContext* ctx, CodeBlob& code, AnyV origin) override {
    TypePtr pack_type = calculate_intN_to_serialize_enum(enum_ref);
    std::vector ir_num = ctx->generate_unpack_any(pack_type);
    const TypeDataIntN* intN = pack_type->try_as<TypeDataIntN>();

    // when reading an integer value, we need to validate that it's a valid enum member;
    // at first, detect whether it's a sequence (A, A+1, ..., A+N)
    bool is_sequence = true;
    td::RefInt256 expected_cur = enum_ref->members.front()->computed_value;
    for (EnumMemberPtr member_ref : enum_ref->members) {
      is_sequence &= td::cmp(member_ref->computed_value, expected_cur) == 0;
      expected_cur += 1;
    }

    if (is_sequence) {
      // enum's members are A...B one by one (probably, 0...M);
      // then validation is: "throw if v<A or v>B", but "LESSINT + THROWIF" 2 times is more generalized
      td::RefInt256 min_value = enum_ref->members.front()->computed_value;
      bool dont_check_min = intN != nullptr && intN->is_unsigned && min_value == 0;
      if (!dont_check_min) {    // LDU can't load < 0 
        std::vector ir_min_value = code.create_tmp_var(TypeDataInt::create(), origin, "(enum-min)");
        code.emplace_back(origin, Op::_IntConst, ir_min_value, min_value);
        std::vector ir_lt_min = code.create_tmp_var(TypeDataInt::create(), origin, "(enum-lt-min)");
        code.emplace_back(origin, Op::_Call, ir_lt_min, std::vector{ir_num[0], ir_min_value[0]}, lookup_function("_<_"));
        std::vector args_throwif = { code.create_int(origin, 5, "(excno)"), ir_lt_min[0] };
        Op& op_assert = code.emplace_back(origin, Op::_Call, std::vector<var_idx_t>{}, std::move(args_throwif), lookup_function("__throw_if"));
        op_assert.set_impure_flag();
      }
      td::RefInt256 max_value = enum_ref->members.back()->computed_value;
      bool dont_check_max = intN != nullptr && intN->is_unsigned && max_value == (1ULL << intN->n_bits) - 1;
      if (!dont_check_max) {    // LDU can't load >= 1<<N
        std::vector ir_max_value = code.create_tmp_var(TypeDataInt::create(), origin, "(enum-max)");
        code.emplace_back(origin, Op::_IntConst, ir_max_value, max_value);
        std::vector ir_gt_max = code.create_tmp_var(TypeDataInt::create(), origin, "(enum-gt-ax)");
        code.emplace_back(origin, Op::_Call, ir_gt_max, std::vector{ir_num[0], ir_max_value[0]}, lookup_function("_>_"));
        std::vector args_throwif = { code.create_int(origin, 5, "(excno)"), ir_gt_max[0] };
        Op& op_assert = code.emplace_back(origin, Op::_Call, std::vector<var_idx_t>{}, std::move(args_throwif), lookup_function("__throw_if"));
        op_assert.set_impure_flag();
      }
    } else {
      // okay, enum is not a sequence, just a set of values;
      // then validation is: "throw if v is not contained in V", check v==V_i and combine with OR
      var_idx_t ir_any_of = code.create_int(origin, 0, "(any-of-equals)");
      for (EnumMemberPtr member_ref : enum_ref->members) {
        std::vector ir_ith_value = code.create_tmp_var(TypeDataInt::create(), origin, "(enum-ith)");
        code.emplace_back(origin, Op::_IntConst, ir_ith_value, member_ref->computed_value);
        std::vector ir_ith_eq = code.create_tmp_var(TypeDataInt::create(), origin, "(enum-ith-eq)");
        code.emplace_back(origin, Op::_Call, ir_ith_eq, std::vector{ir_num[0], ir_ith_value[0]}, lookup_function("_==_"));
        code.emplace_back(origin, Op::_Call, std::vector{ir_any_of}, std::vector{ir_any_of, ir_ith_eq[0]}, lookup_function("_|_"));
      }
      std::vector args_throwifnot = { code.create_int(origin, 5, "(excno)"), ir_any_of };
      Op& op_assert = code.emplace_back(origin, Op::_Call, std::vector<var_idx_t>{}, std::move(args_throwifnot), lookup_function("__throw_ifnot"));
      op_assert.set_impure_flag();
    }
    return ir_num;
  }

  void skip(const UnpackContext* ctx, CodeBlob& code, AnyV origin) override {
    TypePtr pack_type = calculate_intN_to_serialize_enum(enum_ref);
    return ctx->generate_skip_any(pack_type);
  }

  PackSize estimate(const EstimateContext* ctx) override {
    TypePtr pack_type = calculate_intN_to_serialize_enum(enum_ref);
    return ctx->estimate_any(pack_type);
  }
};

struct S_CustomReceiverForPackUnpack final : ISerializer {
  TypePtr receiver_type;

  explicit S_CustomReceiverForPackUnpack(TypePtr receiver_type)
    : receiver_type(receiver_type) {}

  void pack(const PackContext* ctx, CodeBlob& code, AnyV origin, std::vector<var_idx_t>&& rvect) override {
    FunctionPtr f_pack = nullptr, f_unpack = nullptr;
    get_custom_pack_unpack_function(receiver_type, f_pack, f_unpack);
    tolk_assert(f_pack && f_pack->does_accept_self() && f_pack->inferred_return_type->get_width_on_stack() == 0);
    std::vector vars_per_arg = { std::move(rvect), ctx->ir_builder };
    std::vector ir_mutated_builder = gen_inline_fun_call_in_place(code, TypeDataBuilder::create(), origin, f_pack, nullptr, false, vars_per_arg);
    code.emplace_back(origin, Op::_Let, ctx->ir_builder, std::move(ir_mutated_builder));
  }

  std::vector<var_idx_t> unpack(const UnpackContext* ctx, CodeBlob& code, AnyV origin) override {
    FunctionPtr f_pack = nullptr, f_unpack = nullptr;
    get_custom_pack_unpack_function(receiver_type, f_pack, f_unpack);
    tolk_assert(f_unpack && f_unpack->inferred_return_type->get_width_on_stack() == receiver_type->get_width_on_stack());
    TypePtr ret_type = TypeDataTensor::create({TypeDataSlice::create(), receiver_type});
    std::vector ir_slice_and_res = gen_inline_fun_call_in_place(code, ret_type, origin, f_unpack, nullptr, false, {ctx->ir_slice});
    code.emplace_back(origin, Op::_Let, ctx->ir_slice, std::vector{ir_slice_and_res.front()});
    return std::vector(ir_slice_and_res.begin() + 1, ir_slice_and_res.end());
  }

  void skip(const UnpackContext* ctx, CodeBlob& code, AnyV origin) override {
    // just load and ignore the result
    unpack(ctx, code, origin);
  }

  PackSize estimate(const EstimateContext* ctx) override {
    return PackSize::unpredictable_infinity();
  }
};


// --------------------------------------------
//    automatically generate opcodes
//
// for union types like `T1 | T2 | ...`, if prefixes for structs are not manually specified,
// the compiler generates a valid prefix tree: for `int32 | int64 | int128` it's '00' '01' '10';
// it works both for structs (with unspecified prefixes) and primitives: `int32 | A | B` is ok;
// but if some prefixes are specified, some not — it's an error
//


std::vector<PackOpcode> auto_generate_opcodes_for_union(TypePtr union_type, std::string& because_msg) {
  const TypeDataUnion* t_union = union_type->try_as<TypeDataUnion>();
  std::vector<PackOpcode> result;
  result.reserve(t_union->size());

  int n_have_opcode = 0;
  bool has_null = false;
  StructPtr last_struct_with_opcode = nullptr;    // for error message
  StructPtr last_struct_no_opcode = nullptr;
  for (TypePtr variant : t_union->variants) {
    if (const TypeDataStruct* variant_struct = variant->unwrap_alias()->try_as<TypeDataStruct>()) {
      if (variant_struct->struct_ref->opcode.exists()) {
        n_have_opcode++;
        last_struct_with_opcode = variant_struct->struct_ref;
      } else {
        last_struct_no_opcode = variant_struct->struct_ref;
      }
    } else if (variant == TypeDataNullLiteral::create()) {
      has_null = true;
    }
  }

  // `A | B | C`, all of them have opcodes — just use them;
  // for instance, `A | B` is not either (0/1 + data), but uses manual opcodes
  if (n_have_opcode == t_union->size()) {
    for (TypePtr variant : t_union->variants) {
      result.push_back(variant->unwrap_alias()->try_as<TypeDataStruct>()->struct_ref->opcode);
    }
    return result;
  }

  // invalid: `A | B | C`, some of them have opcodes, some not;
  // example: `A | B` if A has opcode, B not;
  // example: `int32 | A` if A has opcode;
  // example: `int32 | int64 | A` if A has opcode;
  if (n_have_opcode) {
    if (last_struct_with_opcode && last_struct_no_opcode) {
      because_msg = "because struct `" + last_struct_with_opcode->as_human_readable() + "` has opcode, but `" + last_struct_no_opcode->as_human_readable() + "` does not\n""hint: manually specify opcodes to all structures";
    } else {
      because_msg = "because of mixing primitives and struct `" + last_struct_with_opcode->as_human_readable() + "` with serialization prefix\n""hint: extract primitives to single-field structs and provide prefixes";
    }
    return result;
  }

  // okay, none of the opcodes are specified, generate a prefix tree;
  // examples: `int32 | int64 | int128` / `int32 | A | null` / `A | B` / `A | B | C`;
  // if `null` exists, it's 0, all others are 1+tree: A|B|C|D|null => 0 | 100+A | 101+B | 110+C | 111+D;
  // if no `null`, just distribute sequentially: A|B|C => 00+A | 01+B | 10+C
  int n_without_null = t_union->size() - has_null; 
  int prefix_len = static_cast<int>(std::ceil(std::log2(n_without_null)));
  int cur_prefix = 0;
  for (TypePtr variant : t_union->variants) {
    if (variant == TypeDataNullLiteral::create()) {
      result.emplace_back(0, 1);
    } else if (has_null) {
      result.emplace_back((1<<prefix_len) + (cur_prefix++), prefix_len + 1);
    } else {
      result.emplace_back(cur_prefix++, prefix_len);
    }
  }
  return result;
}

// given a `enum`, calculate N bits enough to store all values;
// example: `enum Color { Red, Green, Blue }` is 00/01/10 - uint2
// example: `enum Role: int8 { ... }` — manually specified
TypePtr calculate_intN_to_serialize_enum(EnumDefPtr enum_ref) {
  if (enum_ref->colon_type) {       // intN / coins
    return enum_ref->colon_type;
  }
  
  bool is_unsigned = false;
  int n_bits = 1;
  for (; n_bits <= 256; ++n_bits) {
    bool fits_unsigned = std::all_of(enum_ref->members.begin(), enum_ref->members.end(), [n_bits](EnumMemberPtr member_ref) {
      return member_ref->computed_value->unsigned_fits_bits(n_bits);
    });
    if (fits_unsigned) {
      is_unsigned = true;
      break;
    }
    bool fits_signed = std::all_of(enum_ref->members.begin(), enum_ref->members.end(), [n_bits](EnumMemberPtr member_ref) {
      return member_ref->computed_value->signed_fits_bits(n_bits);
    });
    if (fits_signed) {
      break;
    }
  }

  return TypeDataIntN::create(n_bits, is_unsigned, false);
}

// there is no way to pass custom pack options to createMessage / map.set / etc., using hardcoded ones
std::vector<var_idx_t> create_default_PackOptions(CodeBlob& code, AnyV origin) {
  StructPtr s_PackOptions = lookup_global_symbol("PackOptions")->try_as<StructPtr>();
  std::vector ir_options = code.create_tmp_var(TypeDataStruct::create(s_PackOptions), origin, "(pack-options)");
  tolk_assert(ir_options.size() == 1);

  std::vector ir_defaults = {
    code.create_int(origin, 0, "(zero)"),    // skipBitsNFieldsValidation
  };
  code.emplace_back(origin, Op::_Let, ir_options, std::move(ir_defaults));  
  return ir_options;
}

// there is no way to pass custom unpack options to map.get / etc., using hardcoded ones
std::vector<var_idx_t> create_default_UnpackOptions(CodeBlob& code, AnyV origin) {
  StructPtr s_UnpackOptions = lookup_global_symbol("UnpackOptions")->try_as<StructPtr>();
  std::vector ir_options = code.create_tmp_var(TypeDataStruct::create(s_UnpackOptions), origin, "(unpack-options)");
  tolk_assert(ir_options.size() == 2);

  std::vector ir_defaults = {
    code.create_int(origin, -1, "(true)"),     // assertEndAfterReading
    code.create_int(origin, 63, "(excno)"),    // throwIfOpcodeDoesNotMatch
  };
  code.emplace_back(origin, Op::_Let, ir_options, std::move(ir_defaults));  
  return ir_options;
}


// --------------------------------------------
//    detect serializer by TypePtr
//
// note that at earlier compilation steps there already passed a check that any_type is serializable;
// see `check_struct_can_be_packed_or_unpacked()`, its structure reminds this function
//


static std::unique_ptr<ISerializer> get_serializer_for_type(TypePtr any_type) {
  if (const auto* t_intN = any_type->try_as<TypeDataIntN>()) {
    if (t_intN->is_variadic) {
      return std::make_unique<S_VariadicIntN>(t_intN->n_bits, t_intN->is_unsigned);
    }
    return std::make_unique<S_IntN>(t_intN->n_bits, t_intN->is_unsigned);
  }
  if (const auto* t_bitsN = any_type->try_as<TypeDataBitsN>()) {
    return std::make_unique<S_BitsN>(t_bitsN->n_width, t_bitsN->is_bits);
  }
  if (any_type == TypeDataCoins::create()) {
    return std::make_unique<S_Coins>();
  }
  if (any_type == TypeDataBool::create()) {
    return std::make_unique<S_Bool>();
  }
  if (any_type == TypeDataCell::create() || is_type_cellT(any_type)) {
    return std::make_unique<S_RawTVMcell>();
  }
  if (any_type == TypeDataBuilder::create()) {
    return std::make_unique<S_Builder>();
  }
  if (any_type == TypeDataSlice::create()) {
    return std::make_unique<S_Slice>();
  }
  if (any_type == TypeDataNullLiteral::create()) {
    return std::make_unique<S_Null>();
  }
  if (any_type == TypeDataVoid::create()) {
    return std::make_unique<S_Void>();
  }

  if (any_type->try_as<TypeDataMapKV>()) {
    return std::make_unique<S_RawTVMcellOrNull>();
  }
  if (const auto* t_address = any_type->try_as<TypeDataAddress>()) {
    if (t_address->is_internal()) {
      return std::make_unique<S_AddressInt>();
    }
    return std::make_unique<S_AddressAny>();
  }
  if (const auto* t_struct = any_type->try_as<TypeDataStruct>()) {
    return std::make_unique<S_CustomStruct>(t_struct->struct_ref);
  }
  if (const auto* t_enum = any_type->try_as<TypeDataEnum>()) {
    return std::make_unique<S_IntegerEnum>(t_enum->enum_ref);
  }

  if (const auto* t_union = any_type->try_as<TypeDataUnion>()) {
    // `T?` is always `(Maybe T)`, even if T has custom opcode (opcode will follow bit '1')
    if (t_union->or_null) {
      TypePtr or_null = t_union->or_null->unwrap_alias();
      if (or_null == TypeDataCell::create() || is_type_cellT(or_null)) {
        return std::make_unique<S_RawTVMcellOrNull>();
      }
      if (or_null->try_as<TypeDataAddress>() && or_null->try_as<TypeDataAddress>()->is_internal()) {
        return std::make_unique<S_AddressIntOrNull>();  // `address?` is stored as '00' (none) for null
      }
      return std::make_unique<S_Maybe>(t_union);
    }

    // `T1 | T2` is `(Either T1 T2)` (0/1 + contents) unless they both have custom prefixes
    bool all_have_opcode = true;
    for (TypePtr variant : t_union->variants) {
      const TypeDataStruct* variant_struct = variant->unwrap_alias()->try_as<TypeDataStruct>();
      all_have_opcode &= variant_struct && variant_struct->struct_ref->opcode.exists();
    }
    if (t_union->size() == 2 && !all_have_opcode) {
      return std::make_unique<S_Either>(t_union);
    }
    // `T1 | T2 | T3`, probably nullable, probably with primitives, probably with custom opcodes;
    // compiler is able to generate serialization prefixes automatically;
    // and this type is valid, it was checked earlier
    std::string err_msg;
    std::vector<PackOpcode> opcodes = auto_generate_opcodes_for_union(t_union, err_msg);
    tolk_assert(err_msg.empty());
    return std::make_unique<S_MultipleConstructors>(t_union, std::move(opcodes));
  }

  if (const auto* t_tensor = any_type->try_as<TypeDataTensor>()) {
    return std::make_unique<S_Tensor>(t_tensor);
  }

  if (const auto* t_alias = any_type->try_as<TypeDataAlias>()) {
    if (t_alias->alias_ref->name == "RemainingBitsAndRefs") {
      return std::make_unique<S_RemainingBitsAndRefs>();
    }
    FunctionPtr f_pack = nullptr, f_unpack = nullptr;
    get_custom_pack_unpack_function(t_alias, f_pack, f_unpack);
    if (f_pack || f_unpack) {
      return std::make_unique<S_CustomReceiverForPackUnpack>(t_alias);
    }
    return get_serializer_for_type(t_alias->underlying_type);
  }

  // this should not be reachable, serialization availability is checked earlier
  throw Fatal("type `" + any_type->as_human_readable() + "` can not be serialized");
}


void PackContext::generate_pack_any(TypePtr any_type, std::vector<var_idx_t>&& rvect, PrefixWriteMode prefix_mode) const {
  PrefixWriteMode backup = this->prefix_mode;
  this->prefix_mode = prefix_mode;
  get_serializer_for_type(any_type)->pack(this, code, origin, std::move(rvect));
  this->prefix_mode = backup;
}

std::vector<var_idx_t> UnpackContext::generate_unpack_any(TypePtr any_type, PrefixReadMode prefix_mode) const {
  PrefixReadMode backup = this->prefix_mode;
  this->prefix_mode = prefix_mode;
  std::vector result = get_serializer_for_type(any_type)->unpack(this, code, origin);
  this->prefix_mode = backup;
  return result;
}

void UnpackContext::generate_skip_any(TypePtr any_type, PrefixReadMode prefix_mode) const {
  PrefixReadMode backup = this->prefix_mode;
  this->prefix_mode = prefix_mode;
  get_serializer_for_type(any_type)->skip(this, code, origin);
  this->prefix_mode = backup;
}

std::vector<var_idx_t> UnpackContext::generate_lazy_match_any(TypePtr any_type, const LazyMatchOptions& options) const {
  std::unique_ptr<ISerializer> serializer = get_serializer_for_type(any_type);
  if (auto* s = dynamic_cast<S_MultipleConstructors*>(serializer.get())) {
    return s->lazy_match(this, code, origin, options);
  }
  if (auto* s = dynamic_cast<S_Either*>(serializer.get())) {
    return s->lazy_match(this, code, origin, options);
  }
  if (auto* s = dynamic_cast<S_CustomStruct*>(serializer.get())) {
    return s->lazy_match(this, code, origin, options);
  }
  tolk_assert(false);                           
}

PackSize EstimateContext::estimate_any(TypePtr any_type, PrefixEstimateMode prefix_mode) const {
  PrefixEstimateMode backup = this->prefix_mode;
  this->prefix_mode = prefix_mode;
  PackSize result = get_serializer_for_type(any_type)->estimate(this);
  this->prefix_mode = backup;
  return result;
}

} // namespace tolk
