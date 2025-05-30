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
#include "pack-unpack-api.h"
#include "generics-helpers.h"
#include "type-system.h"

namespace tolk {

// fire an error on overflow 1023 bits
GNU_ATTRIBUTE_NORETURN GNU_ATTRIBUTE_COLD
static void fire_error_theoretical_overflow_1023(StructPtr struct_ref, PackSize size) {
  throw ParseError(struct_ref->ast_root->loc,
    "struct `" + struct_ref->as_human_readable() + "` can exceed 1023 bits in serialization (estimated size: " + std::to_string(size.min_bits) + ".." + std::to_string(size.max_bits) + " bits)\n\n"
                  "1) either suppress it by adding an annotation:\n"
                  ">     @overflow1023_policy(\"suppress\")\n"
                  ">     struct " + struct_ref->name + " {\n"
                  ">         ...\n"
                  ">     }\n"
                  "   then, if limit exceeds, it will fail at runtime: you've manually agreed to ignore this\n\n"
                  "2) or place some fields into a separate struct (e.g. ExtraFields), and create a ref:\n"
                  ">     struct " + struct_ref->name + " {\n"
                  ">         ...\n"
                  ">         more: Cell<ExtraFields>;\n"
                  ">     }\n"
  );
}


class CheckSerializedFieldsAndTypesVisitor final : public ASTVisitorFunctionBody {
  FunctionPtr cur_f = nullptr;

  static void check_type_fits_cell_or_has_policy(TypePtr serialized_type) {
    if (const TypeDataStruct* s_struct = serialized_type->unwrap_alias()->try_as<TypeDataStruct>()) {
      check_struct_fits_cell_or_has_policy(s_struct);
    } else if (const TypeDataUnion* s_union = serialized_type->unwrap_alias()->try_as<TypeDataUnion>()) {
      for (TypePtr variant : s_union->variants) {
        check_type_fits_cell_or_has_policy(variant);
      }
    }
  }

  static void check_struct_fits_cell_or_has_policy(const TypeDataStruct* t_struct) {
    StructPtr struct_ref = t_struct->struct_ref;
    PackSize size = estimate_serialization_size(t_struct);
    if (size.max_bits > 1023 && !size.is_unpredictable_infinity()) {
      if (struct_ref->overflow1023_policy == StructData::Overflow1023Policy::not_specified) {
        fire_error_theoretical_overflow_1023(struct_ref, size);
      }
    }
    for (StructFieldPtr field_ref : struct_ref->fields) {
      if (is_type_cellT(field_ref->declared_type)) {
        const TypeDataStruct* f_struct = field_ref->declared_type->try_as<TypeDataStruct>();
        check_type_fits_cell_or_has_policy(f_struct->struct_ref->substitutedTs->typeT_at(0));
      }
    }
  }

  void visit(V<ast_function_call> v) override {
    FunctionPtr fun_ref = v->fun_maybe;
    if (!fun_ref || !fun_ref->is_compile_time_special_gen() || !fun_ref->is_instantiation_of_generic_function()) {
      return;
    }

    std::string_view f_name = fun_ref->base_fun_ref->name;
    TypePtr serialized_type = nullptr;
    bool is_pack = false;
    if (f_name == "Cell<T>.load" || f_name == "T.fromSlice" || f_name == "T.fromCell" || f_name == "T.toCell" ||
        f_name == "T.loadAny" || f_name == "slice.skipAny" || f_name == "slice.storeAny" || f_name == "T.estimatePackSize") {
      serialized_type = fun_ref->substitutedTs->typeT_at(0);
      is_pack = f_name == "T.toCell" || f_name == "slice.storeAny" || f_name == "T.estimatePackSize";
    } else {
      return;   // not a serialization function
    }

    std::string because_msg;
    if (!check_struct_can_be_packed_or_unpacked(serialized_type, is_pack, because_msg)) {
      fire(cur_f, v->loc, "auto-serialization via " + fun_ref->method_name + "() is not available for type `" + serialized_type->as_human_readable() + "`\n" + because_msg);
    }

    check_type_fits_cell_or_has_policy(serialized_type);
  }

 public:
  bool should_visit_function(FunctionPtr fun_ref) override {
    return fun_ref->is_code_function() && !fun_ref->is_generic_function();
  }

  void start_visiting_function(FunctionPtr fun_ref, V<ast_function_declaration> v_function) override {
    cur_f = fun_ref;
    parent::visit(v_function->get_body());
  }
};

void pipeline_check_serialized_fields() {
    visit_ast_of_all_functions<CheckSerializedFieldsAndTypesVisitor>();
}

} // namespace tolk
