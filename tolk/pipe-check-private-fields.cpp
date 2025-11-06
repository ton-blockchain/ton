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
#include "ast-visitor.h"
#include "platform-utils.h"
#include "type-system.h"

namespace tolk {

GNU_ATTRIBUTE_NORETURN GNU_ATTRIBUTE_COLD
static void fire_error_private_field_used_outside_method(FunctionPtr cur_f, SrcLocation loc, StructPtr struct_ref, StructFieldPtr field_ref) {
  throw ParseError(cur_f, loc, "field `" + struct_ref->as_human_readable() + "." + field_ref->name + "` is private");
}

static bool is_private_field_usage_allowed(FunctionPtr cur_f, StructPtr struct_ref) {
  // private fields are accessible only inside methods for that struct
  if (!cur_f->is_method()) {
    return false;
  }
  const TypeDataStruct* receiver_struct = cur_f->receiver_type->unwrap_alias()->try_as<TypeDataStruct>(); 
  if (receiver_struct && receiver_struct->struct_ref == struct_ref) {
    return true;
  }

  // probably it's generic, e.g. struct_ref = `Box<int32>` and receiver = `Box<T>`
  if (struct_ref->is_instantiation_of_generic_struct() && cur_f->is_instantiation_of_generic_function()) {
    const auto* receiver_Ts = cur_f->base_fun_ref->receiver_type->try_as<TypeDataGenericTypeWithTs>();
    return receiver_Ts && receiver_Ts->struct_ref == struct_ref->base_struct_ref;
  }
  
  return false;
}

class CheckPrivateFieldsUsageVisitor final : public ASTVisitorFunctionBody {
  FunctionPtr cur_f = nullptr;

protected:
  void visit(V<ast_dot_access> v) override {
    parent::visit(v);

    if (v->is_target_struct_field()) {
      StructFieldPtr field_ref = std::get<StructFieldPtr>(v->target);
      const TypeDataStruct* obj_type = v->get_obj()->inferred_type->unwrap_alias()->try_as<TypeDataStruct>();
      tolk_assert(obj_type);
      if (field_ref->is_private && !is_private_field_usage_allowed(cur_f, obj_type->struct_ref)) {
        fire_error_private_field_used_outside_method(cur_f, v->loc, obj_type->struct_ref, field_ref);
      }
    }
  }

  void visit(V<ast_object_literal> v) override {
    parent::visit(v);
    tolk_assert(v->struct_ref);

    for (int i = 0; i < v->get_body()->get_num_fields(); ++i) {
      auto v_field = v->get_body()->get_field(i);
      StructFieldPtr field_ref = v_field->field_ref;
      if (field_ref->is_private && !is_private_field_usage_allowed(cur_f, v->struct_ref)) {
        fire_error_private_field_used_outside_method(cur_f, v_field->loc, v->struct_ref, field_ref);
      }
    }
  }
  
 public:
  bool should_visit_function(FunctionPtr fun_ref) override {
    return fun_ref->is_code_function() && !fun_ref->is_generic_function();
  }

  void start_visiting_function(FunctionPtr fun_ref, V<ast_function_declaration> v_function) override {
    cur_f = fun_ref;
    parent::visit(v_function->get_body());
    cur_f = nullptr;
  }
};

void pipeline_check_private_fields_usage() {
  visit_ast_of_all_functions<CheckPrivateFieldsUsageVisitor>();
}

} // namespace tolk
