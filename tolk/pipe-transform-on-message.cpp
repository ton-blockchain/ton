/*
    This file is part of TON Blockchain source code.

    TON Blockchain is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation; either version 2
    of the License, or (at your option) any later version.

    TON Blockchain is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with TON Blockchain.  If not, see <http://www.gnu.org/licenses/>.
*/
#include "tolk.h"
#include "ast.h"
#include "ast-aux-data.h"
#include "ast-replacer.h"
#include "type-system.h"

/*
 *   This pipe analyzes the body of
 *   > fun onInternalMessage(in: InMessage)
 *   and replaces `in.senderAddress` / etc. with aux AST vertices (handled specially at IR generation).
 *
 *   This function is transformed to
 *   > fun onInternalMessage(in.body)
 *   so,
 *   - accessing `in.body` actually will take an element from a stack
 *   - accessing `in.senderAddress` will emit `INMSG_SRC` TVM instruction.
 */

namespace tolk {

// handle all functions having a prototype `fun f(var: InMessage)` (for testing purposes)
static bool is_onInternalMessage(FunctionPtr fun_ref) {
  if (fun_ref->is_entrypoint() || fun_ref->has_tvm_method_id()) {
    if (fun_ref->get_num_params() == 1) {
      const auto* t_param = fun_ref->get_param(0).declared_type->try_as<TypeDataStruct>();
      return t_param && t_param->struct_ref->name == "InMessage";
    }
  }
  return false;
}

// `onBouncedMessage` is only one, it's automatically embedded into `onInternalMessage` if exists
static bool is_onBouncedMessage(FunctionPtr fun_ref) {
  return fun_ref->is_entrypoint() && fun_ref->name == "onBouncedMessage";
}


class TransformOnInternalMessageReplacer final : public ASTReplacerInFunctionBody {
  LocalVarPtr param_ref = nullptr;         // `in` for `fun onInternalMessage(in: InMessage)`

  static void validate_onBouncedMessage(FunctionPtr f) {
    if (f->inferred_return_type != TypeDataVoid::create() && f->inferred_return_type != TypeDataNever::create()) {
      err("`onBouncedMessage` should return `void`").fire(f->ident_anchor, f);
    }
    if (f->get_num_params() != 1) {
      err("`onBouncedMessage` should have one parameter `InMessageBounced`").fire(f->ident_anchor, f);
    }
    const auto* t_struct = f->get_param(0).declared_type->try_as<TypeDataStruct>();
    if (!t_struct || t_struct->struct_ref->name != "InMessageBounced") {
      err("`onBouncedMessage` should have one parameter `InMessageBounced`").fire(f->ident_anchor, f);
    }
  }

  AnyExprV replace(V<ast_reference> v) override {
    // don't allow `var v = in` or passing `in` to another function (only `in.someField` is allowed)
    if (v->sym == param_ref) {
      err("using `{}` as an object is prohibited, because `InMessage` is a built-in struct, its fields are mapped to TVM instructions\n""hint: use `{}.senderAddress` and other fields directly", param_ref->name, param_ref->name).fire(v, cur_f);
    }
    return parent::replace(v);
  }

  AnyExprV replace(V<ast_dot_access> v) override {
    // replace `in.senderAddress` / `in.valueCoins` with an aux vertex
    if (v->get_obj()->kind == ast_reference && v->get_obj()->as<ast_reference>()->sym == param_ref && v->is_target_struct_field()) {
      if (v->is_lvalue && v->get_field_name() != "body" && v->get_field_name() != "bouncedBody") {
        err("modifying an immutable variable\n""hint: fields of InMessage can be used for reading only").fire(v, cur_f);
      }

      ASTAuxData* aux_getField = new AuxData_OnInternalMessage_getField(cur_f, v->get_field_name());
      return createV<ast_artificial_aux_vertex>(v, aux_getField, v->inferred_type);
    }

    return parent::replace(v);
  }

public:
  bool should_visit_function(FunctionPtr fun_ref) override {
    return is_onInternalMessage(fun_ref) || is_onBouncedMessage(fun_ref);
  }

  void on_enter_function(V<ast_function_declaration> v_function) override {
    if (cur_f->name == "onBouncedMessage") {
      validate_onBouncedMessage(cur_f);
    }
    param_ref = &cur_f->parameters[0];
  }

  void on_exit_function(V<ast_function_declaration> v_function) override {
    std::vector<LocalVarData> new_parameters;
    new_parameters.emplace_back("in.body", cur_f->ident_anchor, TypeDataSlice::create(), nullptr, 0, 0);
    cur_f->mutate()->parameters = std::move(new_parameters);
  }
};

void pipeline_transform_onInternalMessage() {
  replace_ast_of_all_functions<TransformOnInternalMessageReplacer>();
}

} // namespace tolk
