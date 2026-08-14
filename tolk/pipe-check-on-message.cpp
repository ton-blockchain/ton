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
#include "ast.h"
#include "ast-visitor.h"
#include "type-system.h"

/*
 *   This pipe validates bodies of message entrypoints:
 *   > fun onInternalMessage(in: InMessage)
 *   > fun onBouncedMessage(in: InMessageBounced)
 *   > fun onExternalMessage([slice])
 *
 *   This is validation-only, no transformation is done.
 *   `in.field` is represented as special IR at lowering.
 */

namespace tolk {

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

static void validate_onExternalMessage(FunctionPtr f) {
  bool no_param_or_slice = f->get_num_params() == 0 ||
    (f->get_num_params() == 1 && f->get_param(0).declared_type == TypeDataSlice::create());
  if (!no_param_or_slice) {
    err("`onExternalMessage` should have one parameter `slice`").fire(f->ident_anchor, f);
  }
}

class CheckOnMessageVisitor final : public ASTVisitorFunctionBody {
  LocalVarPtr param_ref = nullptr;         // `in` for `fun onInternalMessage(in: InMessage)`

  void visit(V<ast_reference> v) override {
    // don't allow `var v = in` or passing `in` to another function (only `in.someField` is allowed)
    if (v->sym == param_ref) {
      err("using `{}` as an object is prohibited, because `InMessage` is a built-in struct, its fields are mapped to TVM instructions\n""hint: use `{}.senderAddress` and other fields directly", param_ref->name, param_ref->name).fire(v, cur_f);
    }
    parent::visit(v);
  }

  void visit(V<ast_dot_access> v) override {
    if (v->get_obj()->kind == ast_reference && v->get_obj()->as<ast_reference>()->sym == param_ref && v->is_target_struct_field()) {
      // `body` / `bouncedBody` may be used as lvalue; other fields are read-only
      if (v->is_lvalue && v->get_field_name() != "body" && v->get_field_name() != "bouncedBody") {
        err("modifying an immutable variable\n""hint: fields of InMessage can be used for reading only").fire(v, cur_f);
      }
      // do not visit the child `ast_reference`: `in.senderAddress` is valid
      return;
    }

    parent::visit(v);
  }

  void visit(V<ast_lambda_fun> v) override {
    for (LocalVarPtr captured_var_ref : v->captured_vars) {
      if (captured_var_ref == param_ref) {
        err("capturing `InMessage` in a lambda is prohibited").fire(v, cur_f);
      }
    }
    parent::visit(v);
  }

public:
  bool should_visit_function(FunctionPtr fun_ref) override {
    if (!fun_ref->is_entrypoint()) {    // quick false
      return false;
    }
    return fun_ref->is_onInternalMessage() || fun_ref->is_onExternalMessage() || fun_ref->is_onBouncedMessage();
  }

  void on_enter_function(V<ast_function_declaration> v_function) override {
    param_ref = nullptr;
    if (cur_f->is_onExternalMessage()) {
      validate_onExternalMessage(cur_f);
      return;
    }
    if (cur_f->is_onBouncedMessage()) {
      validate_onBouncedMessage(cur_f);
    }
    param_ref = &cur_f->parameters[0];
  }

  void on_exit_function(V<ast_function_declaration> v_function) override {
    if (cur_f->is_onInternalMessage() || cur_f->is_onBouncedMessage()) {
      // replace `in:InMessage` with `in.body:slice`, preserving parameters[0] pointer for ast_reference::sym
      tolk_assert(cur_f->get_num_params() == 1);
      cur_f->mutate()->parameters[0] = LocalVarData("in.body", cur_f->ident_anchor, TypeDataSlice::create(), nullptr, 0, 0);
    }
  }
};

void pipeline_check_onInternalMessage() {
  CheckOnMessageVisitor visitor;
  visit_ast_of_all_functions(visitor);
}

} // namespace tolk
