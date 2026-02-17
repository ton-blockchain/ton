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
#include "generics-helpers.h"
#include "type-system.h"

namespace tolk {

std::vector<var_idx_t> pre_compile_is_type(CodeBlob& code, TypePtr expr_type, TypePtr cmp_type, const std::vector<var_idx_t>& expr_ir_idx, AnyV origin, const char* debug_desc);
std::vector<var_idx_t> transition_to_target_type(std::vector<var_idx_t>&& rvect, CodeBlob& code, TypePtr original_type, TypePtr target_type, AnyV origin);

static bool is_type_UnsafeBodyNoRef_T(TypePtr bodyT) {
  if (const TypeDataStruct* t_struct = bodyT->unwrap_alias()->try_as<TypeDataStruct>()) {
    if (t_struct->struct_ref->is_instantiation_of_generic_struct() && t_struct->struct_ref->base_struct_ref->name == "UnsafeBodyNoRef") {
      return true;
    }
  }
  return false;
}

// calculate `addrHash &= mask` where mask = `(1 << (256 - SHARD_DEPTH)) - 1`
static void append_bitwise_and_shard_mask(CodeBlob& code, AnyV origin, var_idx_t ir_addr_hash, var_idx_t ir_shard_depth) {
  var_idx_t ir_one = code.create_int(origin, 1, "(one)");
  std::vector ir_mask = code.create_tmp_var(TypeDataInt::create(), origin, "(mask)");
  code.emplace_back(origin, Op::_Call, ir_mask, std::vector{code.create_int(origin, 256, ""), ir_shard_depth}, lookup_function("_-_"));
  code.emplace_back(origin, Op::_Call, ir_mask, std::vector{ir_one, ir_mask[0]}, lookup_function("_<<_"));
  code.emplace_back(origin, Op::_Call, ir_mask, std::vector{ir_mask[0], ir_one}, lookup_function("_-_"));
  code.emplace_back(origin, Op::_Call, std::vector{ir_addr_hash}, std::vector{ir_addr_hash, ir_mask[0]}, lookup_function("_&_"));
}

// struct AutoDeployAddress { workchain: int8; stateInit: ContractState | cell; toShard: AddressShardingOptions?; }
struct IR_AutoDeployAddress {
  std::vector<var_idx_t> ir_stateInitField,
                         ir_toShardField;
  const TypeDataUnion* t_stateInit;
  const TypeDataUnion* t_toShard;
  var_idx_t workchain,                                      // workchain
            stateInitCode, stateInitData, stateInitCell,    // stateInit
            ir_shardDepth, ir_closeTo;                      // toShard

  IR_AutoDeployAddress(CodeBlob& code, AnyV origin, const std::vector<var_idx_t>& ir_vars) {
    StructPtr s_AutoDeployAddress = lookup_global_symbol("AutoDeployAddress")->try_as<StructPtr>();
    t_stateInit = s_AutoDeployAddress->find_field("stateInit")->declared_type->try_as<TypeDataUnion>();
    t_toShard = s_AutoDeployAddress->find_field("toShard")->declared_type->try_as<TypeDataUnion>();
    tolk_assert(ir_vars.size() == 1 + 3 + 3);
    tolk_assert(t_stateInit && t_stateInit->get_width_on_stack() == (2+1) && t_stateInit->size() == 2);
    tolk_assert(t_toShard && t_toShard->get_width_on_stack() == (2+1) && t_toShard->or_null);

    workchain = ir_vars[0];

    ir_stateInitField = std::vector(ir_vars.begin() + 1, ir_vars.begin() + 1 + 3);
    std::vector ir_ContractState = transition_to_target_type(std::vector(ir_stateInitField), code, t_stateInit, t_stateInit->variants[0], origin);
    stateInitCode = ir_ContractState[0];
    stateInitData = ir_ContractState[1];
    stateInitCell = transition_to_target_type(std::vector(ir_stateInitField), code, t_stateInit, t_stateInit->variants[1], origin)[0];

    ir_toShardField = std::vector(ir_vars.begin() + 1 + 3, ir_vars.begin() + 1 + 3 + 3);
    std::vector ir_AddressSharding = transition_to_target_type(std::vector(ir_toShardField), code, t_toShard, t_toShard->or_null, origin);
    ir_shardDepth = ir_AddressSharding[0];
    ir_closeTo = ir_AddressSharding[1];
  }

  // generate IR vars "stateInit is ContractState"
  std::vector<var_idx_t> is_ContractState(CodeBlob& code, AnyV origin) const {
    return pre_compile_is_type(code, t_stateInit, t_stateInit->variants[0], ir_stateInitField, origin, "(is-ContractState)");
  }

  // generate IR vars "toShard is not null"
  std::vector<var_idx_t> is_AddressSharding(CodeBlob& code, AnyV origin) const {
    return pre_compile_is_type(code, t_toShard, t_toShard->or_null, ir_toShardField, origin, "(is-AddressSharding)");
  }
};

// fun createMessage<TBody>(options: CreateMessageOptions<TBody>): OutMessage
std::vector<var_idx_t> generate_createMessage(FunctionPtr called_f, CodeBlob& code, AnyV origin, const std::vector<std::vector<var_idx_t>>& ir_options) {
  TypePtr bodyT = called_f->substitutedTs->typeT_at(0);
  StructPtr s_Options = lookup_global_symbol("CreateMessageOptions")->try_as<StructPtr>();
  StructPtr s_AutoDeployAddress = lookup_global_symbol("AutoDeployAddress")->try_as<StructPtr>();

  const TypeDataUnion* t_bounce = s_Options->find_field("bounce")->declared_type->try_as<TypeDataUnion>();
  const TypeDataUnion* t_dest = s_Options->find_field("dest")->declared_type->try_as<TypeDataUnion>();
  const TypeDataUnion* t_value = s_Options->find_field("value")->declared_type->try_as<TypeDataUnion>();
  tolk_assert(t_bounce && t_bounce->get_width_on_stack() == (1+1) && t_bounce->size() == 2);
  tolk_assert(t_dest && t_dest->get_width_on_stack() == (1+3+3+1) && t_dest->size() == 4);
  tolk_assert(t_value && t_value->get_width_on_stack() == (2+1) && t_value->size() == 2);

  int offset = 0;
  std::vector rvect = ir_options[0];
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

  // since TVM 12, field `bounce` is a union: `bounce: BounceMode | bool`
  std::vector ir_bounce_is_bool = pre_compile_is_type(code, t_bounce, TypeDataBool::create(), ir_bounce, origin, "(bounce-is-bool)");
  
  // field `dest` is `dest: address | AutoDeployAddress | (int8, uint256) | builder`;
  // struct AutoDeployAddress { workchain: int8; stateInit: ContractState | cell; toShard: AddressShardingOptions?; }
  // struct ContractState { code: cell; data: cell; }
  // struct AddressShardingOptions { fixedPrefixLength: uint5; closeTo: address; }
  std::vector ir_dest_is_address = pre_compile_is_type(code, t_dest, TypeDataAddress::internal(), ir_dest, origin, "(is-address)");
  std::vector ir_dest_is_AutoDeploy = pre_compile_is_type(code, t_dest, TypeDataStruct::create(s_AutoDeployAddress), ir_dest, origin, "(is-auto)");
  std::vector ir_dest_is_builder = pre_compile_is_type(code, t_dest, TypeDataBuilder::create(), ir_dest, origin, "(is-builder)");
  std::vector ir_dest_AutoDeployAddress = transition_to_target_type(std::vector(ir_dest), code, t_dest, TypeDataStruct::create(s_AutoDeployAddress), origin);
  IR_AutoDeployAddress ir_dest_ad(code, origin, ir_dest_AutoDeployAddress);

  FunctionPtr f_beginCell = lookup_function("beginCell");
  FunctionPtr f_endCell = lookup_function("builder.endCell");

  // detect whether to store `body: (Either X ^X)` inline or as ref
  // if it's small (guaranteed to fit), store it inside the same builder, without creating a cell
  PackSize body_size = EstimateContext().estimate_any(bodyT);
  // if `body` is already `cell` / `Cell<T>`
  bool body_already_ref = bodyT == TypeDataCell::create() || is_type_cellT(bodyT);
  // if `body` is `UnsafeBodyNoRef<T>`
  bool body_force_no_ref = is_type_UnsafeBodyNoRef_T(bodyT);
  // max size of all fields before body = 522 (510 CommonMsgInfoRelaxed + 12 StateInit), so 500 bits will fit
  bool body_100p_fits_no_ref = body_size.max_bits <= 500 && body_size.max_refs < 2;
  // final decision: 1 (^X) or 0 (X)
  bool body_store_as_ref = body_already_ref || (!body_100p_fits_no_ref && !body_force_no_ref);

  // if we need to store body ref, convert it to a cell here, before creating a builder for the message;
  // it's more optimal, since the `body` field is the topmost at the stack
  if (body_store_as_ref && !body_already_ref) {
    std::vector ir_ref_builder = code.create_var(TypeDataBuilder::create(), origin, "refb");
    code.emplace_back(origin, Op::_Call, ir_ref_builder, std::vector<var_idx_t>{}, f_beginCell);
    PackContext ref_ctx(code, origin, ir_ref_builder, create_default_PackOptions(code, origin));
    ref_ctx.generate_pack_any(bodyT, std::move(ir_body));
    std::vector ir_ref_cell = code.create_tmp_var(TypeDataCell::create(), origin, "(ref-cell)");
    code.emplace_back(origin, Op::_Call, ir_ref_cell, std::move(ir_ref_builder), f_endCell);
    ir_body = std::move(ir_ref_cell);
  }

  std::vector ir_builder = code.create_var(TypeDataSlice::create(), origin, "b");
  code.emplace_back(origin, Op::_Call, ir_builder, std::vector<var_idx_t>{}, f_beginCell);
  PackContext ctx(code, origin, ir_builder, create_default_PackOptions(code, origin));
  var_idx_t ir_zero = code.create_int(origin, 0, "(zero)");
  var_idx_t ir_one = code.create_int(origin, 1, "(one)");

  // '0' prefix int_msg_info
  ctx.storeUint(ir_zero, 1);
  // fill `ihr_disabled:Bool` always 1
  ctx.storeUint(ir_one, 1);
  // fill `bounce:Bool` from p.bounce (if it's constant (most likely), it will be concatenated with prev and next)
  Op& if_oldBounceFormat = code.emplace_back(origin, Op::_If, ir_bounce_is_bool);
  {
    // input is `bounce: true` or false (old bounce mode), so 0-th slot is 0 or -1
    code.push_set_cur(if_oldBounceFormat.block0);
    ctx.storeBool(ir_bounce[0]);
    code.close_pop_cur(origin);
  }
  {
    // input is `bounce: BounceMode.*` (enum), then write 0 for `NoBounce=0` or 1 otherwise
    code.push_set_cur(if_oldBounceFormat.block1);
    std::vector ir_not_NoBounce = code.create_tmp_var(TypeDataInt::create(), origin, "(not-eq-NoBounce)");
    code.emplace_back(origin, Op::_Call, ir_not_NoBounce, std::vector{ir_bounce[0], ir_zero}, lookup_function("_!=_"));
    ctx.storeBool(ir_not_NoBounce[0]);
    code.close_pop_cur(origin);
  }  
  // fill `bounced:Bool` + `src:MsgAddress` 00
  ctx.storeUint(ir_zero, 1 + 2);

  // fill `dest:MsgAddressInt` from p.dest (complex union)
  Op& if_address = code.emplace_back(origin, Op::_If, ir_dest_is_address);
  {
    // input is `dest: someAddress`
    code.push_set_cur(if_address.block0);
    std::vector ir_dest_address = transition_to_target_type(std::vector(ir_dest), code, t_dest, TypeDataAddress::internal(), origin);
    ctx.storeAddressInt(ir_dest_address[0]);
    code.close_pop_cur(origin);
  }
  {
    code.push_set_cur(if_address.block1);
    Op& if_AutoDeploy = code.emplace_back(origin, Op::_If, ir_dest_is_AutoDeploy);
    {
      // input is `dest: { workchain, stateInit, [toShard] }`;
      // then calculate hash equal to StateInit cell would be and fill "addr_std$10 + 0 anycast + workchain + hash";
      // and, if toShard, take first D bits from dest.toShard.closeTo and mix with 256-D bits of hash
      code.push_set_cur(if_AutoDeploy.block0);
      ctx.storeUint(code.create_int(origin, 0b100, "(addr-prefix)"), 3);  // addr_std$10 + 0 anycast
      ctx.storeInt(ir_dest_ad.workchain, 8);
      std::vector ir_hash = code.create_tmp_var(TypeDataInt::create(), origin, "(addr-hash)");
      Op& if_ContractState = code.emplace_back(origin, Op::_If, ir_dest_ad.is_ContractState(code, origin));
      {
        // input is `dest: { ... stateInit: { code, data } }`
        code.push_set_cur(if_ContractState.block0);
        Op& if_sharded = code.emplace_back(origin, Op::_If, ir_dest_ad.is_AddressSharding(code, origin));
        {
          // input is `dest: { ... stateInit: { code, data }, toShard: { fixedPrefixLength, closeTo } };
          // then stateInitHash = (hash of StateInit = 0b1(depth)0110 (prefix + code + data))
          code.push_set_cur(if_sharded.block0);
          std::vector args = { ir_dest_ad.ir_shardDepth, ir_dest_ad.stateInitCode, ir_dest_ad.stateInitData };
          code.emplace_back(origin, Op::_Call, ir_hash, std::move(args), lookup_function("StateInit.calcHashPrefixCodeData"));
          code.close_pop_cur(origin);
        }
        {
          // input is: `dest: { ... stateInit: { code, data } }` (toShard is null);
          // then hash = (hash of StateInit = 0b00110 (only code + data))
          code.push_set_cur(if_sharded.block1);
          std::vector args = { ir_dest_ad.stateInitCode, ir_dest_ad.stateInitData };
          code.emplace_back(origin, Op::_Call, ir_hash, std::move(args), lookup_function("StateInit.calcHashCodeData"));
          code.close_pop_cur(origin);
        }
        code.close_pop_cur(origin);
      }
      {
        // input is `dest: { ... stateInit: cell }`
        code.push_set_cur(if_ContractState.block1);
        std::vector args = { ir_dest_ad.stateInitCell };
        code.emplace_back(origin, Op::_Call, ir_hash, std::move(args), lookup_function("cell.hash"));
        code.close_pop_cur(origin);
      }
      Op& if_sharded = code.emplace_back(origin, Op::_If, ir_dest_ad.is_AddressSharding(code, origin));
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
        append_bitwise_and_shard_mask(code, origin, ir_hash[0], ir_dest_ad.ir_shardDepth);
        std::vector ir_lowerD = code.create_tmp_var(TypeDataInt::create(), origin, "(lowerD)");
        code.emplace_back(origin, Op::_Call, ir_lowerD, std::vector{code.create_int(origin, 256, ""), ir_dest_ad.ir_shardDepth}, lookup_function("_-_"));
        std::vector ir_shardPrefix = code.create_tmp_var(TypeDataSlice::create(), origin, "(shardPrefix)");
        std::vector args_subslice = { ir_dest_ad.ir_closeTo, code.create_int(origin, 3+8, ""), ir_dest_ad.ir_shardDepth };
        code.emplace_back(origin, Op::_Call, ir_shardPrefix, std::move(args_subslice), lookup_function("slice.getMiddleBits"));
        ctx.storeSlice(ir_shardPrefix[0]);  // first 8 bits of closeTo hash
        ctx.storeUint_var(ir_hash[0], ir_lowerD[0]);  // 248 STU (stateInitHash & mask)
        code.close_pop_cur(origin);
      }
      {
        // input is `dest: { workchain, stateInit }` (toShard is null);
        // we already calculated stateInitHash: either cell.hash() or based on code+data
        code.push_set_cur(if_sharded.block1);
        ctx.storeUint(ir_hash[0], 256);
        code.close_pop_cur(origin);
      }
      code.close_pop_cur(origin);
    }
    {
      code.push_set_cur(if_AutoDeploy.block1);
      Op& if_builder = code.emplace_back(origin, Op::_If, ir_dest_is_builder);
      {
        // input is `dest: someBuilder`
        code.push_set_cur(if_builder.block0);
        std::vector ir_dest_builder = transition_to_target_type(std::vector(ir_dest), code, t_dest, TypeDataBuilder::create(), origin);
        ctx.storeBuilder(ir_dest_builder[0]);
        code.close_pop_cur(origin);
      }
      {
        // input is `dest: (workchain, hash)`
        code.push_set_cur(if_builder.block1);
        std::vector ir_dest_wc_hash = transition_to_target_type(std::vector(ir_dest), code, t_dest, t_dest->variants[2], origin);
        ctx.storeUint(code.create_int(origin, 0b100, "(addr-prefix)"), 3);
        ctx.storeInt(ir_dest_wc_hash[0], 8);    // most likely, it's 0 (basechain), will be merged with above
        ctx.storeUint(ir_dest_wc_hash[1], 256);
        code.close_pop_cur(origin);
      }
      code.close_pop_cur(origin);
    }
    code.close_pop_cur(origin);
  }

  // fill `value:CurrencyCollection` from p.value `coins | (coins, dict)`
  std::vector ir_is_coins = pre_compile_is_type(code, t_value, TypeDataCoins::create(), ir_value, origin, "(is-coins)");
  Op& if_coins = code.emplace_back(origin, Op::_If, ir_is_coins);
  {
    code.push_set_cur(if_coins.block0);
    std::vector ir_coins = transition_to_target_type(std::vector(ir_value), code, t_value, TypeDataCoins::create(), origin);
    ctx.storeCoins(ir_coins[0]);
    ctx.storeUint(ir_zero, 1);
    code.close_pop_cur(origin);
  }
  {
    code.push_set_cur(if_coins.block1);
    std::vector ir_coins_dict = transition_to_target_type(std::move(ir_value), code, t_value, t_value->variants[1], origin);
    ctx.storeCoins(ir_coins_dict[0]);
    ctx.storeMaybeRef(ir_coins_dict[1]);
    code.close_pop_cur(origin);
  }

  // fill `extra_flags:Grams` (formerly `ihr_fee` always 0, now renamed and used for "new bounce format" only)
  Op& if_oldBounceFormat2 = code.emplace_back(origin, Op::_If, ir_bounce_is_bool);
  {
    code.push_set_cur(if_oldBounceFormat2.block0);
    ctx.storeCoins(ir_zero);    // extra_flags = 0
    code.close_pop_cur(origin);
  }
  {
    code.push_set_cur(if_oldBounceFormat2.block1);
    std::vector ir_eq_RichBounce = code.create_tmp_var(TypeDataInt::create(), origin, "(eq-RichBounce)");
    code.emplace_back(origin, Op::_Call, ir_eq_RichBounce, std::vector{ir_bounce[0], code.create_int(origin, 2, "(enum-RichBounce)")}, lookup_function("_==_"));
    Op& if_RichBounce = code.emplace_back(origin, Op::_If, ir_eq_RichBounce);
    {
      code.push_set_cur(if_RichBounce.block0);
      ctx.storeCoins(code.create_int(origin, 3, "(extra-flags-3)"));
      code.close_pop_cur(origin);
    }
    {
      code.push_set_cur(if_RichBounce.block1);
      std::vector ir_eq_RichBounceRoot = code.create_tmp_var(TypeDataInt::create(), origin, "(eq-ir_eq_RichBounceRoot)");
      code.emplace_back(origin, Op::_Call, ir_eq_RichBounceRoot, std::vector{ir_bounce[0], code.create_int(origin, 3, "(enum-RichBounceRoot)")}, lookup_function("_==_"));
      Op& if_RichBounceRoot = code.emplace_back(origin, Op::_If, ir_eq_RichBounceRoot);
      {
        code.push_set_cur(if_RichBounceRoot.block0);
        ctx.storeCoins(code.create_int(origin, 1, "(extra-flags-1)"));
        code.close_pop_cur(origin);
      }
      {
        code.push_set_cur(if_RichBounceRoot.block1);
        ctx.storeCoins(ir_zero);    // extra_flags = 0
        code.close_pop_cur(origin);
      }
      code.close_pop_cur(origin);
    }
    code.close_pop_cur(origin);
  }

  // tail of CommonMsgInfoRelaxed: 4*0 fwd_fee + 64*0 created_lt + 32*0 created_at
  ctx.storeUint(ir_zero, 4 + 64 + 32);

  // fill `init: (Maybe (Either StateInit ^StateInit))`
  // it's present only if p.dest contains StateInit
  // also fill the either bit of `body: (Either X ^X)`
  Op& if_no_init = code.emplace_back(origin, Op::_If, ir_dest_is_AutoDeploy);
  {
    // when it's known at compile-time (always in practice), this `if` is simplified, and bits join with above
    code.push_set_cur(if_no_init.block1);
    ctx.storeUint(body_store_as_ref ? ir_one : ir_zero, 1 + 1);
    code.close_pop_cur(origin);
  }
  {
    code.push_set_cur(if_no_init.block0);
    Op& if_ContractState = code.emplace_back(origin, Op::_If, ir_dest_ad.is_ContractState(code, origin));
    {
      // input is `dest: { ... stateInit: { code, data } }` and need to compose TL/B StateInit;
      // it's either just code+data OR (if `toShard: { ... }` is set) fixedPrefixLength+code+data
      code.push_set_cur(if_ContractState.block0);
      Op& if_sharded = code.emplace_back(origin, Op::_If, ir_dest_ad.is_AddressSharding(code, origin));
      {
        // 1 (maybe true) + 0 (either left) + 1 (maybe true of StateInit) + fixedPrefixLength + 0110 + body ref or not
        code.push_set_cur(if_sharded.block0);
        ctx.storeUint(code.create_int(origin, 0b101, ""), 1 + 1 + 1);
        ctx.storeUint(ir_dest_ad.ir_shardDepth, 5);  // fixedPrefixLength (shard depth)
        ctx.storeUint(code.create_int(origin, 0b01100 + body_store_as_ref, ""), 4 + 1);
        code.close_pop_cur(origin);
        // also, we used dest.toShard to fill CommonMsgInfoRelaxed.dest.address (with a mask for stateInitHash, see above)
      }
      {
        // 1 (maybe true) + 0 (either left) + 00110 (only code and data from StateInit) + body ref or not
        code.push_set_cur(if_sharded.block1);
        var_idx_t ir_rest_bits = code.create_int(origin, 0b10001100 + body_store_as_ref, "(rest-bits)");
        ctx.storeUint(ir_rest_bits, 1 + 1 + 5 + 1);
        code.close_pop_cur(origin);
      }
      ctx.storeRef(ir_dest_ad.stateInitCode);
      ctx.storeRef(ir_dest_ad.stateInitData);
      code.close_pop_cur(origin);
    }
    {
      // so, we have `dest: { stateInit: someCell }`, store it as ref
      // 1 (maybe true) + 1 (either right) + body ref or not
      code.push_set_cur(if_ContractState.block1);
      var_idx_t ir_rest_bits = code.create_int(origin, 0b110 + body_store_as_ref, "(rest-bits)");
      ctx.storeUint(ir_rest_bits, 1 + 1 + 1);
      ctx.storeRef(ir_dest_ad.stateInitCell);
      code.close_pop_cur(origin);
    }
    code.close_pop_cur(origin);
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

  std::vector ir_cell = code.create_tmp_var(TypeDataCell::create(), origin, "(msg-cell)");
  code.emplace_back(origin, Op::_Call, ir_cell, std::move(ir_builder), f_endCell);
  return ir_cell;
}

// fun createExternalLogMessage<TBody>(options: CreateExternalLogMessageOptions<TBody>): OutMessage
std::vector<var_idx_t> generate_createExternalLogMessage(FunctionPtr called_f, CodeBlob& code, AnyV origin, const std::vector<std::vector<var_idx_t>>& args) {
  TypePtr bodyT = called_f->substitutedTs->typeT_at(0);
  StructPtr s_Options = lookup_global_symbol("CreateExternalLogMessageOptions")->try_as<StructPtr>();
  StructPtr s_ExtOutLogBucket = lookup_global_symbol("ExtOutLogBucket")->try_as<StructPtr>();

  const TypeDataUnion* t_dest = s_Options->find_field("dest")->declared_type->try_as<TypeDataUnion>();
  const TypeDataUnion* t_topic = s_ExtOutLogBucket->find_field("topic")->declared_type->try_as<TypeDataUnion>();
  tolk_assert(t_dest && t_dest->get_width_on_stack() == (2+1) && t_dest->size() == 3);
  tolk_assert(t_topic && t_topic->get_width_on_stack() == (1+1) && t_topic->size() == 2);

  int offset = 0;
  std::vector rvect = args[0];
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
  std::vector ir_dest_is_address = pre_compile_is_type(code, t_dest, TypeDataAddress::any(), ir_dest, origin, "(is-address)");
  std::vector ir_dest_is_builder = pre_compile_is_type(code, t_dest, TypeDataBuilder::create(), ir_dest, origin, "(is-builder)");
  std::vector ir_dest_ExtOutLogBucket = transition_to_target_type(std::vector(ir_dest), code, t_dest, TypeDataStruct::create(s_ExtOutLogBucket), origin);
  // dest.topic (it's the only field in a struct)
  std::vector ir_dest_topic = std::move(ir_dest_ExtOutLogBucket);

  std::vector ir_options = create_default_PackOptions(code, origin);

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
    std::vector ir_ref_builder = code.create_var(TypeDataBuilder::create(), origin, "refb");
    code.emplace_back(origin, Op::_Call, ir_ref_builder, std::vector<var_idx_t>{}, f_beginCell);
    PackContext ref_ctx(code, origin, ir_ref_builder, ir_options);
    ref_ctx.generate_pack_any(bodyT, std::move(ir_body));
    std::vector ir_ref_cell = code.create_tmp_var(TypeDataCell::create(), origin, "(ref-cell)");
    code.emplace_back(origin, Op::_Call, ir_ref_cell, std::move(ir_ref_builder), f_endCell);
    ir_body = std::move(ir_ref_cell);
  }

  std::vector ir_builder = code.create_var(TypeDataBuilder::create(), origin, "b");
  code.emplace_back(origin, Op::_Call, ir_builder, std::vector<var_idx_t>{}, f_beginCell);
  PackContext ctx(code, origin, ir_builder, ir_options);
  var_idx_t ir_zero = code.create_int(origin, 0, "(zero)");
  var_idx_t ir_one = code.create_int(origin, 1, "(one)");

  // '11' prefix ext_out_msg_info + '00' src
  ctx.storeUint(code.create_int(origin, 0b1100, "(out-prefix)"), 4);

  // fill `dest:MsgAddressExt` from p.dest (complex union)
  Op& if_address = code.emplace_back(origin, Op::_If, ir_dest_is_address);
  {
    // input is `dest: someAddress`
    code.push_set_cur(if_address.block0);
    std::vector ir_dest_address = transition_to_target_type(std::vector(ir_dest), code, t_dest, TypeDataAddress::any(), origin);
    ctx.storeAddressAny(ir_dest_address[0]);
    code.close_pop_cur(origin);
  }
  {
    code.push_set_cur(if_address.block1);
    Op& if_builder = code.emplace_back(origin, Op::_If, ir_dest_is_builder);
    {
      // input is `dest: someBuilder`
      code.push_set_cur(if_builder.block0);
      std::vector ir_dest_builder = transition_to_target_type(std::vector(ir_dest), code, t_dest, TypeDataBuilder::create(), origin);
      ctx.storeBuilder(ir_dest_builder[0]);
      code.close_pop_cur(origin);
    }
    {
      // input is `dest: ExtOutLogBucket`;
      // fill addr_extern$01 + 256 (len 9 bit) + 0x00 (prefix) + 248 bits
      code.push_set_cur(if_builder.block1);
      ctx.storeUint(ir_one, 2);  // addr_extern$01
      ctx.storeUint(code.create_int(origin, 256, "(addr-len)"), 9);  // len:(## 9) = 256
      ctx.storeOpcode(s_ExtOutLogBucket->opcode);
      std::vector ir_if_topic_uint = pre_compile_is_type(code, t_topic, t_topic->variants[0], ir_dest_topic, origin, "(topic-is-uint)");
      Op& if_topic_uint = code.emplace_back(origin, Op::_If, ir_if_topic_uint);
      {
        // input is `dest: ExtOutLogBucket { topic: uint248 }`
        code.push_set_cur(if_topic_uint.block0);
        ctx.storeUint(ir_dest_topic[0], 248);
        code.close_pop_cur(origin);
      }
      {
        // input is `dest: ExtOutLogBucket { topic: bits248 }`
        // for this field, generate runtime check to ensure its length
        code.push_set_cur(if_topic_uint.block1);
        ctx.generate_pack_any(t_topic->variants[1], std::vector{ir_dest_topic[0]});
        code.close_pop_cur(origin);
      }
      code.close_pop_cur(origin);
    }
    code.close_pop_cur(origin);
  }

  // tail of CommonMsgInfoRelaxed: 64*0 created_lt + 32*0 created_at
  // plus, StateInit is empty (0 maybe bit) for external messages
  ctx.storeUint(ir_zero, 64 + 32 + 1);

  // fill bit `body: (Either X ^X)` and store body
  if (body_size.max_bits == 0 && body_size.max_refs == 0) {
    // missing body of type `void`
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

  std::vector ir_cell = code.create_tmp_var(TypeDataCell::create(), origin, "(msg-cell)");
  code.emplace_back(origin, Op::_Call, ir_cell, std::move(ir_builder), f_endCell);
  return ir_cell;
}

// fun address.buildSameAddressInAnotherShard(self, options: AddressShardingOptions): builder
std::vector<var_idx_t> generate_address_buildInAnotherShard(FunctionPtr called_f, CodeBlob& code, AnyV origin, const std::vector<std::vector<var_idx_t>>& args) {
  std::vector ir_shard_options = args[1];
  tolk_assert(ir_shard_options.size() == 2);

  // example for fixedPrefixLength (shard depth) = 8:
  // | self (A)     | aaaaaaaaaaa...aaa |
  // | closeTo (B)  | 01010101bbb...bbb |   shardPrefix = 01010101 (depth 8)
  // | result       | 01010101aaa...aaa |   address of A in same shard as B

  // the most effective way is not to calculate shardPrefix, but to:
  // - take first 3+8+D bits of B: we'll have '100' (std addr no anycast) + workchainB + shardPrefix
  // - take last  256-D bits of A: we'll have "aa...a"
  // - concatenate: we'll result in '100' + workchainB + "bbaa...a"

  std::vector ir_offsetB = {code.create_int(origin, 3 + 8, "(offset-addrB)")};
  code.emplace_back(origin, Op::_Call, ir_offsetB, std::vector{ir_offsetB[0], ir_shard_options[0]}, lookup_function("_+_"));
  std::vector ir_headB = code.create_tmp_var(TypeDataSlice::create(), origin, "(headB)");
  code.emplace_back(origin, Op::_Call, ir_headB, std::vector{ir_shard_options[1], ir_offsetB[0]}, lookup_function("slice.getFirstBits"));

  std::vector ir_builder = code.create_var(TypeDataBuilder::create(), origin, "b");
  code.emplace_back(origin, Op::_Call, ir_builder, std::vector<var_idx_t>{}, lookup_function("beginCell"));
  code.emplace_back(origin, Op::_Call, ir_builder, std::vector{ir_builder[0], ir_headB[0]}, lookup_function("builder.storeSlice"));

  std::vector ir_restLenA = {code.create_int(origin, 256, "(last-addrA)")};
  code.emplace_back(origin, Op::_Call, ir_restLenA, std::vector{ir_restLenA[0], ir_shard_options[0]}, lookup_function("_-_"));
  std::vector ir_tailA = code.create_tmp_var(TypeDataSlice::create(), origin, "(tailA)");
  code.emplace_back(origin, Op::_Call, ir_tailA, std::vector{args[0][0], ir_restLenA[0]}, lookup_function("slice.getLastBits"));
  code.emplace_back(origin, Op::_Call, ir_builder, std::vector{ir_builder[0], ir_tailA[0]}, lookup_function("builder.storeSlice"));

  return ir_builder;
}

// fun address.calculateSameAddressInAnotherShard(self, options: AddressShardingOptions): address
std::vector<var_idx_t> generate_address_calculateInAnotherShard(FunctionPtr called_f, CodeBlob& code, AnyV origin, const std::vector<std::vector<var_idx_t>>& args) {
  // it's "build address" + BTOS (until a deprecated "build" function removed)
  std::vector ir_builder = generate_address_buildInAnotherShard(called_f, code, origin, args);
  code.emplace_back(origin, Op::_Call, ir_builder, ir_builder, lookup_function("builder.toSlice"));
  return ir_builder;
}

// fun AutoDeployAddress.buildAddress(self): builder
std::vector<var_idx_t> generate_AutoDeployAddress_buildAddress(FunctionPtr called_f, CodeBlob& code, AnyV origin, const std::vector<std::vector<var_idx_t>>& ir_options) {
  IR_AutoDeployAddress ir_self(code, origin, ir_options[0]);

  std::vector ir_builder = code.create_tmp_var(TypeDataSlice::create(), origin, "(addr-b)");
  // important! unlike `createMessage()`, we calculate hash and shard prefix BEFORE creating a cell
  // (for fewer stack manipulations)

  // calculate stateInitHash = (hash of StateInit cell would be, but without constructing a cell)
  std::vector ir_hash = code.create_tmp_var(TypeDataInt::create(), origin, "(addr-hash)");
  Op& if_ContractState = code.emplace_back(origin, Op::_If, ir_self.is_ContractState(code, origin));
  {
    // called `{ ... stateInit: { code, data } }`
    code.push_set_cur(if_ContractState.block0);
    Op& if_sharded = code.emplace_back(origin, Op::_If, ir_self.is_AddressSharding(code, origin));
    {
      // called `{ ... stateInit: { code, data }, toShard: { fixedPrefixLength, closeTo } }
      code.push_set_cur(if_sharded.block0);
      std::vector args = { ir_self.ir_shardDepth, ir_self.stateInitCode, ir_self.stateInitData };
      code.emplace_back(origin, Op::_Call, ir_hash, std::move(args), lookup_function("StateInit.calcHashPrefixCodeData"));
      code.close_pop_cur(origin);
    }
    {
      // called `{ ... stateInit: { code, data } }` (toShard is null)
      code.push_set_cur(if_sharded.block1);
      std::vector args = { ir_self.stateInitCode, ir_self.stateInitData };
      code.emplace_back(origin, Op::_Call, ir_hash, std::move(args), lookup_function("StateInit.calcHashCodeData"));
      code.close_pop_cur(origin);
    }
    code.close_pop_cur(origin);
  }
  {
    // called `{ ... stateInit: cell }`
    code.push_set_cur(if_ContractState.block1);
    std::vector args = { ir_self.stateInitCell };
    code.emplace_back(origin, Op::_Call, ir_hash, std::move(args), lookup_function("cell.hash"));
    code.close_pop_cur(origin);
  }

  // now, if toShard, perform bitwise calculations with hashes (order on a stack matters)
  Op& if_sharded = code.emplace_back(origin, Op::_If, ir_self.is_AddressSharding(code, origin));
  {
    // called `{ ... toShard: { fixedPrefixLength, closeTo } }`
    // we already calculated stateInitHash (ir_hash): either cell.hash() or based on prefix+code+data;
    // keep hash = (last 256-D bits from stateInitHash) = `hash & mask`
    code.push_set_cur(if_sharded.block0);
    append_bitwise_and_shard_mask(code, origin, ir_hash[0], ir_self.ir_shardDepth);
    std::vector ir_lowerD = code.create_tmp_var(TypeDataInt::create(), origin, "(lowerD)");
    code.emplace_back(origin, Op::_Call, ir_lowerD, std::vector{code.create_int(origin, 256, ""), ir_self.ir_shardDepth}, lookup_function("_-_"));

    // calculate shard_prefix = (first D bits from dest.toShard.closeTo)
    std::vector ir_shardPrefix = code.create_tmp_var(TypeDataSlice::create(), origin, "(shardPrefix)");
    std::vector args_subslice = { ir_self.ir_closeTo, code.create_int(origin, 3+8, ""), ir_self.ir_shardDepth };
    code.emplace_back(origin, Op::_Call, ir_shardPrefix, std::move(args_subslice), lookup_function("slice.getMiddleBits"));

    // on a stack: stateInitHash & mask; shard prefix; create a cell and store all
    code.emplace_back(origin, Op::_Call, ir_builder, std::vector<var_idx_t>{}, lookup_function("beginCell"));
    PackContext ctx(code, origin, ir_builder, create_default_PackOptions(code, origin));
    ctx.storeUint(code.create_int(origin, 0b100, "(addr-prefix)"), 3);  // addr_std$10 + 0 anycast
    ctx.storeInt(ir_self.workchain, 8);
    ctx.storeSlice(ir_shardPrefix[0]);  // first 8 bits of closeTo hash
    ctx.storeUint_var(ir_hash[0], ir_lowerD[0]);  // 248 STU (stateInitHash & mask)
    code.close_pop_cur(origin);
  }
  {
    // called `{ workchain, stateInit }` (toShard is null);
    // on a stack: hash (already calculated); create a cell and store all
    code.push_set_cur(if_sharded.block1);
    code.emplace_back(origin, Op::_Call, ir_builder, std::vector<var_idx_t>{}, lookup_function("beginCell"));
    PackContext ctx(code, origin, ir_builder, create_default_PackOptions(code, origin));
    ctx.storeUint(code.create_int(origin, 0b100, "(addr-prefix)"), 3);  // addr_std$10 + 0 anycast
    ctx.storeInt(ir_self.workchain, 8);
    ctx.storeUint(ir_hash[0], 256);
    code.close_pop_cur(origin);
  }

  return ir_builder;
}

// fun AutoDeployAddress.calculateAddress(self): address
std::vector<var_idx_t> generate_AutoDeployAddress_calculateAddress(FunctionPtr called_f, CodeBlob& code, AnyV origin, const std::vector<std::vector<var_idx_t>>& ir_options) {
  // it's "build address" + BTOS (until a deprecated "build" function removed)
  std::vector ir_builder = generate_AutoDeployAddress_buildAddress(called_f, code, origin, ir_options);
  code.emplace_back(origin, Op::_Call, ir_builder, ir_builder, lookup_function("builder.toSlice"));
  return ir_builder;
}

// fun AutoDeployAddress.addressMatches(self, addr: address): bool
std::vector<var_idx_t> generate_AutoDeployAddress_addressMatches(FunctionPtr called_f, CodeBlob& code, AnyV origin, const std::vector<std::vector<var_idx_t>>& ir_self_and_addr) {
  IR_AutoDeployAddress ir_self(code, origin, ir_self_and_addr[0]);

  // at first, calculate stateInitHash = (hash of StateInit cell would be, but without constructing a cell)
  std::vector ir_hash = code.create_tmp_var(TypeDataInt::create(), origin, "(addr-hash)");
  Op& if_ContractState = code.emplace_back(origin, Op::_If, ir_self.is_ContractState(code, origin));
  {
    // called `{ ... stateInit: { code, data } }`
    code.push_set_cur(if_ContractState.block0);
    Op& if_sharded = code.emplace_back(origin, Op::_If, ir_self.is_AddressSharding(code, origin));
    {
      // called `{ ... stateInit: { code, data }, toShard: { fixedPrefixLength, closeTo } }
      code.push_set_cur(if_sharded.block0);
      std::vector args = { ir_self.ir_shardDepth, ir_self.stateInitCode, ir_self.stateInitData };
      code.emplace_back(origin, Op::_Call, ir_hash, std::move(args), lookup_function("StateInit.calcHashPrefixCodeData"));
      code.close_pop_cur(origin);
    }
    {
      // called `{ ... stateInit: { code, data } }` (toShard is null)
      code.push_set_cur(if_sharded.block1);
      std::vector args = { ir_self.stateInitCode, ir_self.stateInitData };
      code.emplace_back(origin, Op::_Call, ir_hash, std::move(args), lookup_function("StateInit.calcHashCodeData"));
      code.close_pop_cur(origin);
    }
    code.close_pop_cur(origin);
  }
  {
    // called `{ ... stateInit: cell }`
    code.push_set_cur(if_ContractState.block1);
    std::vector args = { ir_self.stateInitCell };
    code.emplace_back(origin, Op::_Call, ir_hash, std::move(args), lookup_function("cell.hash"));
    code.close_pop_cur(origin);
  }

  // now calculate `stateInitHash &= mask` where mask = `(1 << (256 - SHARD_DEPTH)) - 1`
  Op& if_sharded1 = code.emplace_back(origin, Op::_If, ir_self.is_AddressSharding(code, origin));
  {
    code.push_set_cur(if_sharded1.block0);
    append_bitwise_and_shard_mask(code, origin, ir_hash[0], ir_self.ir_shardDepth);
    code.close_pop_cur(origin);
  }
  {
    code.push_set_cur(if_sharded1.block1);
    code.close_pop_cur(origin);
  }

  // now do `(wc, hash) = addr.getWorkchainAndHash()`
  std::vector ir_addr_wc_hash = code.create_tmp_var(TypeDataTensor::create({TypeDataInt::create(), TypeDataInt::create()}), origin, "(self-wc-hash)");
  code.emplace_back(origin, Op::_Call, ir_addr_wc_hash, ir_self_and_addr[1], lookup_function("address.getWorkchainAndHash"));

  // now calculate `hash &= mask` (the same as we did earlier for stateInitHash)
  Op& if_sharded2 = code.emplace_back(origin, Op::_If, ir_self.is_AddressSharding(code, origin));
  {
    code.push_set_cur(if_sharded2.block0);
    append_bitwise_and_shard_mask(code, origin, ir_addr_wc_hash[1], ir_self.ir_shardDepth);
    code.close_pop_cur(origin);
  }
  {
    code.push_set_cur(if_sharded2.block1);
    code.close_pop_cur(origin);
  }

  // finally, eval `(hash == stateInitHash) & (wc == workchain)`
  std::vector ir_eq_hash = code.create_tmp_var(TypeDataInt::create(), origin, "(eq-hash)");
  code.emplace_back(origin, Op::_Call, ir_eq_hash, std::vector{ir_addr_wc_hash[1], ir_hash[0]}, lookup_function("_==_"));
  std::vector ir_eq_wc = code.create_tmp_var(TypeDataInt::create(), origin, "(eq-wc)");
  code.emplace_back(origin, Op::_Call, ir_eq_wc, std::vector{ir_addr_wc_hash[0], ir_self.workchain}, lookup_function("_==_"));

  std::vector ir_bool_result = code.create_tmp_var(TypeDataBool::create(), origin, "(is-addr-result)");
  code.emplace_back(origin, Op::_Call, ir_bool_result, std::vector{ir_eq_hash[0], ir_eq_wc[0]}, lookup_function("_&_"));
  return ir_bool_result;
}

} // namespace tolk
