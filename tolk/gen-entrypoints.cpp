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
 * Under the hood, `in.senderAddress` is lowered into `INMSG_SRC`, and so on.
 *
 *   Also, if `onBouncedMessage` exists, it's embedded directly, like
 *   > if (INMSG_BOUNCED) { onBouncedMessage(in.body); return; }
 */

namespace tolk {

// implemented in ast-from-legacy.cpp
std::vector<var_idx_t> gen_inline_fun_call_in_place(CodeBlob& code, TypePtr ret_type, AnyV origin, FunctionPtr f_inlined, AnyExprV self_obj, bool is_before_immediate_return, const std::vector<std::vector<var_idx_t>>& vars_per_arg);


std::vector<var_idx_t> generate_get_InMessage_field(CodeBlob& code, AnyV origin, std::string_view field_name, LocalVarPtr param_in_body) {
  // `in.body` and `in.bouncedBody` are actually `slice` from a stack;
  // beforehand, `onInternalMessage` was transformed from `in:InMessage` to `in.body:slice`
  if (field_name == "body" || field_name == "bouncedBody") {
    tolk_assert(param_in_body->ir_idx.size() == 1);
    return param_in_body->ir_idx;
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

  std::vector ir_msgparam = code.create_tmp_var(TypeDataInt::create(), origin, "(inmsg-field)");
  code.add_call(origin, ir_msgparam, {code.create_int(origin, idx, "(param-idx)")}, lookup_function("__InMessage.getInMsgParam"));

  if (field_name == "originalForwardFee") {
    code.add_call(origin, ir_msgparam, {ir_msgparam[0], code.create_int(origin, 0, "(basechain)")}, lookup_function("__InMessage.originalForwardFee"));
  }

  return ir_msgparam;
}

void handle_onInternalMessage_codegen_start(FunctionPtr f_onInternalMessage, const std::vector<var_idx_t>& ir_body_slice, CodeBlob& code, AnyV origin) {
  // parameter `in:InMessage` was transformed to `in.body:slice`
  tolk_assert(f_onInternalMessage->is_onInternalMessage() && ir_body_slice.size() == 1);

  // ignore `@on_bounced_policy("manual")`, don't insert "if (isBounced) return"
  if (f_onInternalMessage->is_manual_on_bounce()) {
    return;
  }

  const Symbol* sym = lookup_global_symbol("onBouncedMessage");
  FunctionPtr f_onBouncedMessage = sym ? sym->try_as<FunctionPtr>() : nullptr;

  std::vector ir_isBounced = generate_get_InMessage_field(code, origin, "isBounced", nullptr);

  if (f_onBouncedMessage) {
    // generate: `if (isBounced) { onBouncedMessage(); return; }
    tolk_assert(f_onBouncedMessage->inferred_return_type->get_width_on_stack() == 0);
    Op& if_isBounced = code.add_if_else(origin, ir_isBounced);
    {
      code.push_set_cur(if_isBounced.block0);
      if (f_onBouncedMessage->is_inlined_in_place()) {
        gen_inline_fun_call_in_place(code, TypeDataVoid::create(), origin, f_onBouncedMessage, nullptr, true, {ir_body_slice});
      } else {
        code.add_call(origin, {}, ir_body_slice, f_onBouncedMessage);
      }
      code.add_return(origin, {}, f_onInternalMessage);
      code.close_pop_cur(origin);
    }
    {
      code.push_set_cur(if_isBounced.block1);
      code.close_pop_cur(origin);
    }
  } else {
    // generate: `if (isBounced) throw 0`
    std::vector args = { code.create_int(origin, 0, "(exit-0)"), ir_isBounced[0] };
    code.add_call(origin, {}, std::move(args), lookup_function("__throw_if"));
  }
}

} // namespace tolk
