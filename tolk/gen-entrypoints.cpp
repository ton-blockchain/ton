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
#include "gen-entrypoints.h"
#include "type-system.h"

/*
 *   This module is responsible for `onInternalMessage` at IR generation.
 *   In FunC, `recv_internal()` entrypoint was declared in such a way:
 *   > () recv_internal(int my_balance, int msg_value, cell in_msg_full, slice in_msg_body)
 *   Whenever the user wanted to check whether the message is bounced, he had to parse the cell manually.
 *
 *   In Tolk:
 *   > fun onInternalMessage(in: InMessage)
 *   And to use `in.senderAddress`, `in.body`, `in.originalForwardFee`, etc. in the function.
 * Under the hood, `in.senderAddress` is transformed into `INMSG_SRC`, and so on.
 *
 *   Also, if `onBouncedMessage` exists, it's embedded directly, like
 *   > if (INMSG_BOUNCED) { onBouncedMessage(in.body); return; }
 */

namespace tolk {

// implemented in ast-from-legacy.cpp
std::vector<var_idx_t> gen_inline_fun_call_in_place(CodeBlob& code, TypePtr ret_type, AnyV origin, FunctionPtr f_inlined, AnyExprV self_obj, bool is_before_immediate_return, const std::vector<std::vector<var_idx_t>>& vars_per_arg);


// check for "modern" `fun onInternalMessage(in: InMessage)`,
// because a "FunC-style" (msgCell: cell, msgBody: slice) also works;
// after transformation `in.xxx` to TVM aux vertices, the last parameter is named `in.body`
static bool is_modern_onInternalMessage(FunctionPtr f_onInternalMessage) {
  return f_onInternalMessage->get_num_params() == 1 && f_onInternalMessage->get_param(0).name == "in.body";
}

std::vector<var_idx_t> AuxData_OnInternalMessage_getField::generate_get_InMessage_field(CodeBlob& code, AnyV origin) const {
  if (field_name == "body") {           // take `in.body` from a stack
    return f_onInternalMessage->find_param("in.body")->ir_idx;
  }
  if (field_name == "bouncedBody") {    // we're in onBouncedMessage()
    return f_onInternalMessage->find_param("in.body")->ir_idx;
  }

  int idx = -1;
  if      (field_name == "isBounced")          idx = 1;
  else if (field_name == "senderAddress")      idx = 2;
  else if (field_name == "originalForwardFee") idx = 3;
  else if (field_name == "createdLt")          idx = 4;
  else if (field_name == "createdAt")          idx = 5;
  else if (field_name == "valueCoins")         idx = 7;
  else if (field_name == "valueExtra")         idx = 8;
  tolk_assert(idx != -1);

  std::vector ir_msgparam = code.create_tmp_var(TypeDataInt::create(), origin, field_name.data());
  code.emplace_back(origin, Op::_Call, ir_msgparam, std::vector{code.create_int(origin, idx, "(param-idx)")}, lookup_function("__InMessage.getInMsgParam"));

  if (field_name == "originalForwardFee") {
    code.emplace_back(origin, Op::_Call, ir_msgparam, std::vector{ir_msgparam[0], code.create_int(origin, 0, "(basechain)")}, lookup_function("__InMessage.originalForwardFee"));
  }

  return ir_msgparam;
}

void handle_onInternalMessage_codegen_start(FunctionPtr f_onInternalMessage, const std::vector<var_idx_t>& rvect_params, CodeBlob& code, AnyV origin) {
  // ignore FunC-style `onInternalMessage(msgCell, msgBody)`
  if (!is_modern_onInternalMessage(f_onInternalMessage)) {
    return;
  }
  // ignore `@on_bounced_policy("manual")`, don't insert "if (isBounced) return"
  if (f_onInternalMessage->is_manual_on_bounce()) {
    return;
  }

  const Symbol* sym = lookup_global_symbol("onBouncedMessage");
  FunctionPtr f_onBouncedMessage = sym ? sym->try_as<FunctionPtr>() : nullptr;

  AuxData_OnInternalMessage_getField get_isBounced(f_onInternalMessage, "isBounced");
  std::vector ir_isBounced = get_isBounced.generate_get_InMessage_field(code, origin);

  if (f_onBouncedMessage) {
    // generate: `if (isBounced) { onBouncedMessage(); return; }
    tolk_assert(f_onBouncedMessage->inferred_return_type->get_width_on_stack() == 0);
    Op& if_isBounced = code.emplace_back(origin, Op::_If, ir_isBounced);
    {
      code.push_set_cur(if_isBounced.block0);
      std::vector ir_bodySlice(rvect_params.end() - 1, rvect_params.end());
      if (f_onBouncedMessage->is_inlined_in_place()) {
        gen_inline_fun_call_in_place(code, TypeDataVoid::create(), origin, f_onBouncedMessage, nullptr, true, {ir_bodySlice});
      } else {
        Op& op_call = code.emplace_back(origin, Op::_Call, std::vector<var_idx_t>{}, ir_bodySlice, f_onBouncedMessage);
        op_call.set_impure_flag();
      }
      code.emplace_back(origin, Op::_Return, std::vector<var_idx_t>{});
      code.close_pop_cur(origin);
    }
    {
      code.push_set_cur(if_isBounced.block1);
      code.close_pop_cur(origin);
    }
  } else {
    // generate: `if (isBounced) throw 0`
    std::vector args = { code.create_int(origin, 0, "(exit-0)"), ir_isBounced[0] };
    Op& op_throw0if = code.emplace_back(origin, Op::_Call, std::vector<var_idx_t>{}, std::move(args), lookup_function("__throw_if"));
    op_throw0if.set_impure_flag();
  }
}

} // namespace tolk
