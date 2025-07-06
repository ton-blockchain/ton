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
#include "send-message-api.h"
#include "pack-unpack-serializers.h"
#include "type-system.h"

namespace tolk {

std::vector<var_idx_t> pre_compile_is_type(CodeBlob& code, TypePtr expr_type, TypePtr cmp_type, const std::vector<var_idx_t>& expr_ir_idx, SrcLocation loc, const char* debug_desc);
std::vector<var_idx_t> transition_to_target_type(std::vector<var_idx_t>&& rvect, CodeBlob& code, TypePtr original_type, TypePtr target_type, SrcLocation loc);

static bool is_type_UnsafeBodyNoRef_T(TypePtr bodyT) {
  if (const TypeDataStruct* t_struct = bodyT->unwrap_alias()->try_as<TypeDataStruct>()) {
    if (t_struct->struct_ref->is_instantiation_of_generic_struct() && t_struct->struct_ref->base_struct_ref->name == "UnsafeBodyNoRef") {
      return true;
    }
  }
  return false;
}

// currently, there is no way to pass custom pack options to createMessage, using hardcoded ones
static std::vector<var_idx_t> create_default_PackOptions(CodeBlob& code, SrcLocation loc) {
  StructPtr s_PackOptions = lookup_global_symbol("PackOptions")->try_as<StructPtr>();
  std::vector ir_options = code.create_tmp_var(TypeDataStruct::create(s_PackOptions), loc, "(pack-options)");
  tolk_assert(ir_options.size() == 1);

  var_idx_t ir_zero = code.create_int(loc, 0, "(zero)");
  code.emplace_back(loc, Op::_Let, std::vector{ir_options[0]}, std::vector{ir_zero});  // skipBitsNFieldsValidation
  return ir_options;
}

std::vector<var_idx_t> generate_createMessage(CodeBlob& code, SrcLocation loc, TypePtr bodyT, std::vector<var_idx_t>&& rvect) {
  StructPtr s_Options = lookup_global_symbol("CreateMessageOptions")->try_as<StructPtr>();
  StructPtr s_AutoDeployAddress = lookup_global_symbol("AutoDeployAddress")->try_as<StructPtr>();

  const TypeDataBool* t_bounce = s_Options->find_field("bounce")->declared_type->try_as<TypeDataBool>();
  const TypeDataUnion* t_dest = s_Options->find_field("dest")->declared_type->try_as<TypeDataUnion>();
  const TypeDataUnion* t_value = s_Options->find_field("value")->declared_type->try_as<TypeDataUnion>();
  const TypeDataUnion* t_stateInit = s_AutoDeployAddress->find_field("stateInit")->declared_type->try_as<TypeDataUnion>();
  const TypeDataUnion* t_toShard = s_AutoDeployAddress->find_field("toShard")->declared_type->try_as<TypeDataUnion>();
  tolk_assert(t_bounce);
  tolk_assert(t_dest && t_dest->get_width_on_stack() == (1+3+3+1) && t_dest->size() == 4);
  tolk_assert(t_value && t_value->get_width_on_stack() == (2+1) && t_value->size() == 2);
  tolk_assert(t_stateInit && t_stateInit->get_width_on_stack() == (2+1) && t_stateInit->size() == 2);
  tolk_assert(t_toShard && t_toShard->get_width_on_stack() == (2+1) && t_toShard->or_null);

  int offset = 0;
  auto next_slice = [&rvect, &offset](int width) -> std::vector<var_idx_t> {
    int start = offset;
    offset += width;
    return std::vector(rvect.begin() + start, rvect.begin() + start + width);
  };

  std::vector ir_bounce = next_slice(t_bounce->get_width_on_stack());
  std::vector ir_value  = next_slice(t_value->get_width_on_stack());
  std::vector ir_dest   = next_slice(t_dest->get_width_on_stack());
  std::vector ir_body   = next_slice(bodyT->get_width_on_stack());
  tolk_assert(offset == static_cast<int>(rvect.size()));

  // field `dest` is `dest: address | AutoDeployAddress | (int8, uint256) | builder`;
  // struct AutoDeployAddress { workchain: int8; stateInit: ContractState | cell; toShard: AddressShardingOptions?; }
  // struct ContractState { code: cell; data: cell; }
  // struct AddressShardingOptions { fixedPrefixLength: uint5; closeTo: address; }
  std::vector ir_dest_is_address = pre_compile_is_type(code, t_dest, TypeDataAddress::create(), ir_dest, loc, "(is-address)");
  std::vector ir_dest_is_AutoDeploy = pre_compile_is_type(code, t_dest, TypeDataStruct::create(s_AutoDeployAddress), ir_dest, loc, "(is-address)");
  std::vector ir_dest_is_builder = pre_compile_is_type(code, t_dest, TypeDataBuilder::create(), ir_dest, loc, "(is-builder)");
  std::vector ir_dest_AutoDeployAddress = transition_to_target_type(std::vector(ir_dest), code, t_dest, TypeDataStruct::create(s_AutoDeployAddress), loc);
  // dest.workchain
  std::vector ir_dest_workchain(ir_dest_AutoDeployAddress.begin(), ir_dest_AutoDeployAddress.begin() + 1);
  // dest.stateInit (code+data or cell)
  std::vector ir_dest_stateInitUnion(ir_dest_AutoDeployAddress.begin() + 1, ir_dest_AutoDeployAddress.begin() + 1 + 3);
  std::vector ir_is_ContractState = pre_compile_is_type(code, t_stateInit, t_stateInit->variants[0], ir_dest_stateInitUnion, loc, "(is-ContractState)");
  std::vector ir_ContractState = transition_to_target_type(std::vector(ir_dest_stateInitUnion), code, t_stateInit, t_stateInit->variants[0], loc);
  std::vector ir_StateInitCell = transition_to_target_type(std::vector(ir_dest_stateInitUnion), code, t_stateInit, t_stateInit->variants[1], loc);
  // dest.toShard (shardPrefix+closeTo or null)
  std::vector ir_dest_toShardOrNull(ir_dest_AutoDeployAddress.begin() + 1 + 3, ir_dest_AutoDeployAddress.begin() + 1 + 3 + 3);
  std::vector ir_is_AddressSharding = pre_compile_is_type(code, t_toShard, t_toShard->or_null, ir_dest_toShardOrNull, loc, "(is-AddressSharding)");
  std::vector ir_AddressSharding = transition_to_target_type(std::vector(ir_dest_toShardOrNull), code, t_toShard, t_toShard->or_null, loc);

  // currently, there is no way to pass PackOptions, defaults are used
  std::vector ir_options = create_default_PackOptions(code, loc);

  FunctionPtr f_beginCell = lookup_function("beginCell");
  FunctionPtr f_endCell = lookup_function("builder.endCell");

  // detect whether to store `body: (Either X ^X)` inline or as ref
  // if it's small (guaranteed to fit), store it inside the same builder, without creating a cell
  PackSize body_size = EstimateContext().estimate_any(bodyT);
  // if `body` is already `cell` / `Cell<T>`
  bool body_already_ref = bodyT == TypeDataCell::create() || is_type_cellT(bodyT);
  // if `body` is `UnsafeBodyNoRef<T>`
  bool body_force_no_ref = is_type_UnsafeBodyNoRef_T(bodyT);
  // max size of all fields before body = 514 (502 CommonMsgInfoRelaxed + 12 StateInit), so 500 bits will fit
  bool body_100p_fits_no_ref = body_size.max_bits <= 500 && body_size.max_refs < 2;
  // final decision: 1 (^X) or 0 (X)
  bool body_store_as_ref = body_already_ref || (!body_100p_fits_no_ref && !body_force_no_ref);

  // if we need to store body ref, convert it to a cell here, before creating a builder for the message;
  // it's more optimal, since the `body` field is the topmost at the stack
  if (body_store_as_ref && !body_already_ref) {
    std::vector ir_ref_builder = code.create_var(TypeDataBuilder::create(), loc, "refb");
    code.emplace_back(loc, Op::_Call, ir_ref_builder, std::vector<var_idx_t>{}, f_beginCell);
    PackContext ref_ctx(code, loc, ir_ref_builder, ir_options);
    ref_ctx.generate_pack_any(bodyT, std::move(ir_body));
    std::vector ir_ref_cell = code.create_tmp_var(TypeDataCell::create(), loc, "(ref-cell)");
    code.emplace_back(loc, Op::_Call, ir_ref_cell, std::move(ir_ref_builder), f_endCell);
    ir_body = std::move(ir_ref_cell);
  }

  std::vector ir_builder = code.create_var(TypeDataSlice::create(), loc, "b");
  code.emplace_back(loc, Op::_Call, ir_builder, std::vector<var_idx_t>{}, f_beginCell);
  PackContext ctx(code, loc, ir_builder, ir_options);
  var_idx_t ir_zero = code.create_int(loc, 0, "(zero)");
  var_idx_t ir_one = code.create_int(loc, 1, "(one)");

  // '0' prefix int_msg_info
  ctx.storeUint(ir_zero, 1);
  // fill `ihr_disabled:Bool` always 1
  ctx.storeUint(ir_one, 1);
  // fill `bounce:Bool` from p.bounce (if it's constant (most likely), it will be concatenated with prev and next)
  ctx.storeBool(ir_bounce[0]);
  // fill `bounced:Bool` + `src:MsgAddress` 00
  ctx.storeUint(ir_zero, 1 + 2);

  // fill `dest:MsgAddressInt` from p.dest (complex union)
  Op& if_address = code.emplace_back(loc, Op::_If, ir_dest_is_address);
  {
    // input is `dest: someAddress`
    code.push_set_cur(if_address.block0);
    std::vector ir_dest_address = transition_to_target_type(std::vector(ir_dest), code, t_dest, TypeDataAddress::create(), loc);
    ctx.storeAddress(ir_dest_address[0]);
    code.close_pop_cur(loc);
  }
  {
    code.push_set_cur(if_address.block1);
    Op& if_AutoDeploy = code.emplace_back(loc, Op::_If, ir_dest_is_AutoDeploy);
    {
      // input is `dest: { workchain, stateInit, [toShard] }`;
      // then calculate hash equal to StateInit cell would be and fill "addr_std$10 + 0 anycast + workchain + hash";
      // and, if toShard, take first D bits from dest.toShard.closeTo and mix with 256-D bits of hash
      code.push_set_cur(if_AutoDeploy.block0);
      ctx.storeUint(code.create_int(loc, 0b100, "(addr-prefix)"), 3);  // addr_std$10 + 0 anycast
      ctx.storeInt(ir_dest_workchain[0], 8);
      std::vector ir_hash = code.create_tmp_var(TypeDataInt::create(), loc, "(addr-hash)");
      Op& if_ContractState = code.emplace_back(loc, Op::_If, ir_is_ContractState);
      {
        // input is `dest: { ... stateInit: { code, data } }`
        code.push_set_cur(if_ContractState.block0);
        Op& if_sharded = code.emplace_back(loc, Op::_If, ir_is_AddressSharding);
        {
          // input is `dest: { ... stateInit: { code, data }, toShard: { fixedPrefixLength, closeTo } };
          // then stateInitHash = (hash of StateInit = 0b1(depth)0110 (prefix + code + data))
          code.push_set_cur(if_sharded.block0);
          std::vector ir_args_depth_code_data = { ir_AddressSharding[0], ir_ContractState[0], ir_ContractState[1] };
          code.emplace_back(loc, Op::_Call, ir_hash, std::move(ir_args_depth_code_data), lookup_function("StateInit.calcHashPrefixCodeData"));
          code.close_pop_cur(loc);
        }
        {
          // input is: `dest: { ... stateInit: { code, data } }` (toShard is null);
          // then hash = (hash of StateInit = 0b00110 (only code + data))
          code.push_set_cur(if_sharded.block1);
          code.emplace_back(loc, Op::_Call, ir_hash, ir_ContractState, lookup_function("StateInit.calcHashCodeData"));
          code.close_pop_cur(loc);
        }
        code.close_pop_cur(loc);
      }
      {
        // input is `dest: { ... stateInit: cell }`
        code.push_set_cur(if_ContractState.block1);
        code.emplace_back(loc, Op::_Call, ir_hash, ir_StateInitCell, lookup_function("cell.hash"));
        code.close_pop_cur(loc);
      }
      Op& if_sharded = code.emplace_back(loc, Op::_If, ir_is_AddressSharding);
      {
        // input is `dest: { ... toShard: { fixedPrefixLength, closeTo } }`
        // we already calculated stateInitHash (ir_hash): either cell.hash() or based on prefix+code+data;
        // now, we need: hash = (first D bits from dest.toShard.closeTo) + (last 256-D bits from stateInitHash);
        // example for fixedPrefixLength (shard depth) = 8:
        // | closeTo       | 01010101...xxx |      given as input, by user (it's address, internally slice)
        // | shardPrefix   | 01010101       |      first 8 bits of closeTo
        // | stateInitHash | yyyyyyyy...yyy |      mask = (1 << (256-D)) - 1 = 00000000111...111 (8 zeroes)
        // | hash (result) | 01010101...yyy |
        // remember, that closeTo is addr_std$10 + 0 + workchain + xxx...xxx, so skip 11 bits and read 8
        code.push_set_cur(if_sharded.block0);
        std::vector ir_shardPrefix = code.create_tmp_var(TypeDataSlice::create(), loc, "(shardPrefix)");
        std::vector args_subslice = { ir_AddressSharding[1], code.create_int(loc, 3+8, ""), ir_AddressSharding[0] };
        code.emplace_back(loc, Op::_Call, ir_shardPrefix, std::move(args_subslice), lookup_function("slice.getMiddleBits"));
        ctx.storeSlice(ir_shardPrefix[0]);  // first 8 bits of closeTo hash
        std::vector ir_mask = code.create_tmp_var(TypeDataInt::create(), loc, "(mask)");
        code.emplace_back(loc, Op::_Call, ir_mask, std::vector{code.create_int(loc, 256, ""), ir_AddressSharding[0]}, lookup_function("_-_"));
        code.emplace_back(loc, Op::_Call, ir_mask, std::vector{ir_one, ir_mask[0]}, lookup_function("_<<_"));
        code.emplace_back(loc, Op::_Call, ir_mask, std::vector{ir_mask[0], ir_one}, lookup_function("_-_"));
        code.emplace_back(loc, Op::_Call, ir_hash, std::vector{ir_hash[0], ir_mask[0]}, lookup_function("_&_"));
        code.emplace_back(loc, Op::_Call, ir_mask, std::vector{code.create_int(loc, 256, ""), ir_AddressSharding[0]}, lookup_function("_-_"));
        ctx.storeUint_var(ir_hash[0], ir_mask[0]);  // 248 STU (stateInitHash & mask)
        code.close_pop_cur(loc);
      }
      {
        // input is `dest: { workchain, stateInit }` (toShard is null);
        // we already calculated stateInitHash: either cell.hash() or based on code+data
        code.push_set_cur(if_sharded.block1);
        ctx.storeUint(ir_hash[0], 256);
        code.close_pop_cur(loc);
      }
      code.close_pop_cur(loc);
    }
    {
      code.push_set_cur(if_AutoDeploy.block1);
      Op& if_builder = code.emplace_back(loc, Op::_If, ir_dest_is_builder);
      {
        // input is `dest: someBuilder`
        code.push_set_cur(if_builder.block0);
        std::vector ir_dest_builder = transition_to_target_type(std::vector(ir_dest), code, t_dest, TypeDataBuilder::create(), loc);
        ctx.storeBuilder(ir_dest_builder[0]);
        code.close_pop_cur(loc);
      }
      {
        // input is `dest: (workchain, hash)`
        code.push_set_cur(if_builder.block1);
        std::vector ir_dest_wc_hash = transition_to_target_type(std::vector(ir_dest), code, t_dest, t_dest->variants[2], loc);
        ctx.storeUint(code.create_int(loc, 0b100, "(addr-prefix)"), 3);
        ctx.storeInt(ir_dest_wc_hash[0], 8);    // most likely, it's 0 (basechain), will be merged with above
        ctx.storeUint(ir_dest_wc_hash[1], 256);
        code.close_pop_cur(loc);
      }
      code.close_pop_cur(loc);
    }
    code.close_pop_cur(loc);
  }

  // fill `value:CurrencyCollection` from p.value `coins | (coins, dict)`
  std::vector ir_is_coins = pre_compile_is_type(code, t_value, TypeDataCoins::create(), ir_value, loc, "(is-coins)");
  Op& if_coins = code.emplace_back(loc, Op::_If, ir_is_coins);
  {
    code.push_set_cur(if_coins.block0);
    std::vector ir_coins = transition_to_target_type(std::vector(ir_value), code, t_value, TypeDataCoins::create(), loc);
    ctx.storeCoins(ir_coins[0]);
    ctx.storeUint(ir_zero, 1);
    code.close_pop_cur(loc);
  }
  {
    code.push_set_cur(if_coins.block1);
    std::vector ir_coins_dict = transition_to_target_type(std::move(ir_value), code, t_value, t_value->variants[1], loc);
    ctx.storeCoins(ir_coins_dict[0]);
    ctx.storeMaybeRef(ir_coins_dict[1]);
    code.close_pop_cur(loc);
  }

  // tail of CommonMsgInfoRelaxed: 4*0 ihr_fee + 4*0 fwd_fee + 64*0 created_lt + 32*0 created_at
  ctx.storeUint(ir_zero, 4 + 4 + 64 + 32);

  // fill `init: (Maybe (Either StateInit ^StateInit))`
  // it's present only if p.dest contains StateInit
  // also fill the either bit of `body: (Either X ^X)`
  Op& if_no_init = code.emplace_back(loc, Op::_If, ir_dest_is_AutoDeploy);
  {
    // when it's known at compile-time (always in practice), this `if` is simplified, and bits join with above
    code.push_set_cur(if_no_init.block1);
    ctx.storeUint(body_store_as_ref ? ir_one : ir_zero, 1 + 1);
    code.close_pop_cur(loc);
  }
  {
    code.push_set_cur(if_no_init.block0);
    Op& if_ContractState = code.emplace_back(loc, Op::_If, ir_is_ContractState);
    {
      // input is `dest: { ... stateInit: { code, data } }` and need to compose TL/B StateInit;
      // it's either just code+data OR (if `toShard: { ... }` is set) fixedPrefixLength+code+data
      code.push_set_cur(if_ContractState.block0);
      Op& if_sharded = code.emplace_back(loc, Op::_If, ir_is_AddressSharding);
      {
        // 1 (maybe true) + 0 (either left) + 1 (maybe true of StateInit) + fixedPrefixLength + 0110 + body ref or not
        code.push_set_cur(if_sharded.block0);
        ctx.storeUint(code.create_int(loc, 0b101, ""), 1 + 1 + 1);
        ctx.storeUint(ir_AddressSharding[0], 5);  // fixedPrefixLength (shard depth)
        ctx.storeUint(code.create_int(loc, 0b01100 + body_store_as_ref, ""), 4 + 1);
        code.close_pop_cur(loc);
        // also, we used dest.toShard to fill CommonMsgInfoRelaxed.dest.address (with a mask for stateInitHash, see above)
      }
      {
        // 1 (maybe true) + 0 (either left) + 00110 (only code and data from StateInit) + body ref or not
        code.push_set_cur(if_sharded.block1);
        var_idx_t ir_rest_bits = code.create_int(loc, 0b10001100 + body_store_as_ref, "(rest-bits)");
        ctx.storeUint(ir_rest_bits, 1 + 1 + 5 + 1);
        code.close_pop_cur(loc);
      }
      ctx.storeRef(ir_ContractState[0]);    // dest.stateInit.code
      ctx.storeRef(ir_ContractState[1]);    // dest.stateInit.data
      code.close_pop_cur(loc);
    }
    {
      // so, we have `dest: { stateInit: someCell }`, store it as ref
      // 1 (maybe true) + 1 (either right) + body ref or not
      code.push_set_cur(if_ContractState.block1);
      var_idx_t ir_rest_bits = code.create_int(loc, 0b110 + body_store_as_ref, "(rest-bits)");
      ctx.storeUint(ir_rest_bits, 1 + 1 + 1);
      ctx.storeRef(ir_StateInitCell[0]);
      code.close_pop_cur(loc);
    }
    code.close_pop_cur(loc);
  }

  // store body; previously, we've calculated whether to store is as a ref or not
  if (body_size.max_bits == 0 && body_size.max_refs == 0) {
    tolk_assert(ir_body.empty());
  } else if (body_store_as_ref) {
    tolk_assert(ir_body.size() == 1);   // it was either an input cell or a automatically created one
    ctx.storeRef(ir_body[0]);
  } else {
    ctx.generate_pack_any(bodyT, std::move(ir_body));
  }

  std::vector ir_cell = code.create_tmp_var(TypeDataCell::create(), loc, "(msg-cell)");
  code.emplace_back(loc, Op::_Call, ir_cell, std::move(ir_builder), f_endCell);
  return ir_cell;
}

std::vector<var_idx_t> generate_createExternalLogMessage(CodeBlob& code, SrcLocation loc, TypePtr bodyT, std::vector<var_idx_t>&& rvect) {
  StructPtr s_Options = lookup_global_symbol("createExternalLogMessageOptions")->try_as<StructPtr>();
  StructPtr s_ExtOutLogBucket = lookup_global_symbol("ExtOutLogBucket")->try_as<StructPtr>();

  const TypeDataUnion* t_dest = s_Options->find_field("dest")->declared_type->try_as<TypeDataUnion>();
  const TypeDataUnion* t_topic = s_ExtOutLogBucket->find_field("topic")->declared_type->try_as<TypeDataUnion>();
  tolk_assert(t_dest && t_dest->get_width_on_stack() == (2+1) && t_dest->size() == 3);
  tolk_assert(t_topic && t_topic->get_width_on_stack() == (1+1) && t_topic->size() == 2);

  int offset = 0;
  auto next_slice = [&rvect, &offset](int width) -> std::vector<var_idx_t> {
    int start = offset;
    offset += width;
    return std::vector(rvect.begin() + start, rvect.begin() + start + width);
  };

  std::vector ir_dest   = next_slice(t_dest->get_width_on_stack());
  std::vector ir_body   = next_slice(bodyT->get_width_on_stack());
  tolk_assert(offset == static_cast<int>(rvect.size()));

  // field `dest` is `dest: address | builder | ExtOutLogBucket`;
  // struct ExtOutLogBucket { topic: uint248 | bits248; }
  std::vector ir_dest_is_address = pre_compile_is_type(code, t_dest, TypeDataAddress::create(), ir_dest, loc, "(is-address)");
  std::vector ir_dest_is_builder = pre_compile_is_type(code, t_dest, TypeDataBuilder::create(), ir_dest, loc, "(is-builder)");
  std::vector ir_dest_ExtOutLogBucket = transition_to_target_type(std::vector(ir_dest), code, t_dest, TypeDataStruct::create(s_ExtOutLogBucket), loc);
  // dest.topic (it's the only field in a struct)
  std::vector ir_dest_topic = std::move(ir_dest_ExtOutLogBucket);

  std::vector ir_options = create_default_PackOptions(code, loc);

  FunctionPtr f_beginCell = lookup_function("beginCell");
  FunctionPtr f_endCell = lookup_function("builder.endCell");

  // detect whether to store `body: (Either X ^X)` inline or as ref
  // if it's small (guaranteed to fit), store it inside the same builder, without creating a cell
  PackSize body_size = EstimateContext().estimate_any(bodyT);
  // if `body` is already `cell` / `Cell<T>`
  bool body_already_ref = bodyT == TypeDataCell::create() || is_type_cellT(bodyT);
  // if `body` is `UnsafeBodyNoRef<T>`
  bool body_force_no_ref = is_type_UnsafeBodyNoRef_T(bodyT);
  // max size of all fields before body = 622 (621 CommonMsgInfoRelaxed + 1 StateInit), so 400 bits will fit
  bool body_100p_fits_no_ref = body_size.max_bits < 400;
  // final decision: 1 (^X) or 0 (X)
  bool body_store_as_ref = body_already_ref || (!body_100p_fits_no_ref && !body_force_no_ref);

  // same as for createMessage: `body` field is the topmost at the stack, convert it to a cell before creating a builder
  if (body_store_as_ref && !body_already_ref) {
    std::vector ir_ref_builder = code.create_var(TypeDataBuilder::create(), loc, "refb");
    code.emplace_back(loc, Op::_Call, ir_ref_builder, std::vector<var_idx_t>{}, f_beginCell);
    PackContext ref_ctx(code, loc, ir_ref_builder, ir_options);
    ref_ctx.generate_pack_any(bodyT, std::move(ir_body));
    std::vector ir_ref_cell = code.create_tmp_var(TypeDataCell::create(), loc, "(ref-cell)");
    code.emplace_back(loc, Op::_Call, ir_ref_cell, std::move(ir_ref_builder), f_endCell);
    ir_body = std::move(ir_ref_cell);
  }

  std::vector ir_builder = code.create_var(TypeDataBuilder::create(), loc, "b");
  code.emplace_back(loc, Op::_Call, ir_builder, std::vector<var_idx_t>{}, f_beginCell);
  PackContext ctx(code, loc, ir_builder, ir_options);
  var_idx_t ir_zero = code.create_int(loc, 0, "(zero)");
  var_idx_t ir_one = code.create_int(loc, 1, "(one)");

  // '11' prefix ext_out_msg_info + '00' src
  ctx.storeUint(code.create_int(loc, 0b1100, "(out-prefix)"), 4);

  // fill `dest:MsgAddressExt` from p.dest (complex union)
  Op& if_address = code.emplace_back(loc, Op::_If, ir_dest_is_address);
  {
    // input is `dest: someAddress`
    code.push_set_cur(if_address.block0);
    std::vector ir_dest_address = transition_to_target_type(std::vector(ir_dest), code, t_dest, TypeDataAddress::create(), loc);
    ctx.storeAddress(ir_dest_address[0]);
    code.close_pop_cur(loc);
  }
  {
    code.push_set_cur(if_address.block1);
    Op& if_builder = code.emplace_back(loc, Op::_If, ir_dest_is_builder);
    {
      // input is `dest: someBuilder`
      code.push_set_cur(if_builder.block0);
      std::vector ir_dest_builder = transition_to_target_type(std::vector(ir_dest), code, t_dest, TypeDataBuilder::create(), loc);
      ctx.storeBuilder(ir_dest_builder[0]);
      code.close_pop_cur(loc);
    }
    {
      // input is `dest: ExtOutLogBucket`;
      // fill addr_extern$01 + 256 (len 9 bit) + 0x00 (prefix) + 248 bits
      code.push_set_cur(if_builder.block1);
      ctx.storeUint(ir_one, 2);  // addr_extern$01
      ctx.storeUint(code.create_int(loc, 256, "(addr-len)"), 9);  // len:(## 9) = 256
      ctx.storeOpcode(s_ExtOutLogBucket->opcode);
      std::vector ir_if_topic_uint = pre_compile_is_type(code, t_topic, t_topic->variants[0], ir_dest_topic, loc, "(topic-is-uint)");
      Op& if_topic_uint = code.emplace_back(loc, Op::_If, ir_if_topic_uint);
      {
        // input is `dest: ExtOutLogBucket { topic: uint248 }`
        code.push_set_cur(if_topic_uint.block0);
        ctx.storeUint(ir_dest_topic[0], 248);
        code.close_pop_cur(loc);
      }
      {
        // input is `dest: ExtOutLogBucket { topic: bits248 }`
        // for this field, generate runtime check to ensure its length
        code.push_set_cur(if_topic_uint.block1);
        ctx.generate_pack_any(t_topic->variants[1], std::vector{ir_dest_topic[0]});
        code.close_pop_cur(loc);
      }
      code.close_pop_cur(loc);
    }
    code.close_pop_cur(loc);
  }

  // tail of CommonMsgInfoRelaxed: 64*0 created_lt + 32*0 created_at
  // plus, StateInit is empty (0 maybe bit) for external messages
  ctx.storeUint(ir_zero, 64 + 32 + 1);

  // fill bit `body: (Either X ^X)` and store body
  if (body_size.max_bits == 0 && body_size.max_refs == 0) {
    // missing body of type `never`
    tolk_assert(ir_body.empty());
    ctx.storeUint(ir_zero, 1);
  } else if (body_store_as_ref) {
    tolk_assert(ir_body.size() == 1);
    ctx.storeUint(ir_one, 1);
    ctx.storeRef(ir_body[0]);
  } else {
    ctx.storeUint(ir_zero, 1);
    ctx.generate_pack_any(bodyT, std::move(ir_body));
  }

  std::vector ir_cell = code.create_tmp_var(TypeDataCell::create(), loc, "(msg-cell)");
  code.emplace_back(loc, Op::_Call, ir_cell, std::move(ir_builder), f_endCell);
  return ir_cell;
}

std::vector<var_idx_t> generate_address_buildInAnotherShard(CodeBlob& code, SrcLocation loc, std::vector<var_idx_t>&& ir_self_address, std::vector<var_idx_t>&& ir_shard_options) {
  tolk_assert(ir_shard_options.size() == 2);

  // example for fixedPrefixLength (shard depth) = 8:
  // | self (A)     | aaaaaaaaaaa...aaa |
  // | closeTo (B)  | 01010101bbb...bbb |   shardPrefix = 01010101 (depth 8)
  // | result       | 01010101aaa...aaa |   address of A in same shard as B

  // the most effective way is not to calculate shardPrefix, but to:
  // - take first 3+8+D bits of B: we'll have '100' (std addr no anycast) + workchainB + shardPrefix
  // - take last  256-D bits of A: we'll have "aa...a"
  // - concatenate: we'll result in '100' + workchainB + "bbaa...a"

  std::vector ir_offsetB = {code.create_int(loc, 3 + 8, "(offset-addrB)")};
  code.emplace_back(loc, Op::_Call, ir_offsetB, std::vector{ir_offsetB[0], ir_shard_options[0]}, lookup_function("_+_"));
  std::vector ir_headB = code.create_tmp_var(TypeDataSlice::create(), loc, "(headB)");
  code.emplace_back(loc, Op::_Call, ir_headB, std::vector{ir_shard_options[1], ir_offsetB[0]}, lookup_function("slice.getFirstBits"));

  std::vector ir_builder = code.create_var(TypeDataBuilder::create(), loc, "b");
  code.emplace_back(loc, Op::_Call, ir_builder, std::vector<var_idx_t>{}, lookup_function("beginCell"));
  code.emplace_back(loc, Op::_Call, ir_builder, std::vector{ir_builder[0], ir_headB[0]}, lookup_function("builder.storeSlice"));

  std::vector ir_restLenA = {code.create_int(loc, 256, "(last-addrA)")};
  code.emplace_back(loc, Op::_Call, ir_restLenA, std::vector{ir_restLenA[0], ir_shard_options[0]}, lookup_function("_-_"));
  std::vector ir_tailA = code.create_tmp_var(TypeDataSlice::create(), loc, "(tailA)");
  code.emplace_back(loc, Op::_Call, ir_tailA, std::vector{ir_self_address[0], ir_restLenA[0]}, lookup_function("slice.getLastBits"));
  code.emplace_back(loc, Op::_Call, ir_builder, std::vector{ir_builder[0], ir_tailA[0]}, lookup_function("builder.storeSlice"));

  return ir_builder;
}

} // namespace tolk
