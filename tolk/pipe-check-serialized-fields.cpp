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
#include "compilation-errors.h"
#include "pack-unpack-api.h"
#include "maps-kv-api.h"
#include "generics-helpers.h"
#include "type-system.h"

namespace tolk {

// make an error on overflow 1023 bits or 4 cells
static Error err_theoretical_overflow_1023(StructPtr struct_ref, PackSize size) {
  bool exceeds_in_bits = size.max_bits > 1023;
  return err("struct `{}` can exceed {} {} in serialization (estimated size: {}..{} {})\n\n"
                  "1) either suppress it by adding an annotation:\n"
                  ">     @overflow1023_policy(\"suppress\")\n"
                  ">     struct {} {\n"
                  ">         ...\n"
                  ">     }\n"
                  "   then, if limit exceeds, it will fail at runtime: you've manually agreed to ignore this\n\n"
                  "2) or place some fields into a separate struct (e.g. ExtraFields), and create a ref:\n"
                  ">     struct {} {\n"
                  ">         ...\n"
                  ">         more: Cell<ExtraFields>;\n"
                  ">     }\n",
                  struct_ref,
                  exceeds_in_bits ? 1023 : 4,
                  exceeds_in_bits ? "bits" : "refs",
                  exceeds_in_bits ? size.min_bits : size.min_refs,
                  exceeds_in_bits ? size.max_bits : size.max_refs,
                  exceeds_in_bits ? "bits" : "refs",
                  struct_ref, struct_ref);
}

GNU_ATTRIBUTE_NOINLINE
static void check_map_TKey_TValue(SrcRange range, TypePtr TKey, TypePtr TValue) {
  std::string because_msg;
  if (!check_mapKV_TKey_is_valid(TKey, because_msg)) {
    err("invalid `map`: type `{}` can not be used as a key\n{}", TKey, because_msg).collect(range);
  } 
  if (!check_mapKV_TValue_is_valid(TValue, because_msg)) {
    err("invalid `map`: type `{}` can not be used as a value\n{}", TValue, because_msg).collect(range);
  } 
}

GNU_ATTRIBUTE_NOINLINE
static void check_mapKV_inside_type(SrcRange range, TypePtr any_type) {
  any_type->replace_children_custom([range](TypePtr child) {
    if (const TypeDataMapKV* t_map = child->try_as<TypeDataMapKV>()) {
      check_map_TKey_TValue(range, t_map->TKey, t_map->TValue);
    }
    return child;
  });
}

static void check_mapKV_inside_type(AnyTypeV type_node) {
  if (type_node && type_node->resolved_type->has_mapKV_inside()) {
    check_mapKV_inside_type(type_node->range, type_node->resolved_type);
  }
}

// given `enum Role: int8` check colon type (not struct/slice etc.)
static void check_enum_colon_type_to_be_intN(AnyTypeV colon_type_node) {
  if (!colon_type_node->resolved_type->try_as<TypeDataIntN>() && !colon_type_node->resolved_type->try_as<TypeDataCoins>()) {
    err("serialization type of `enum` must be intN: `int8` / `uint32` / etc.").collect(colon_type_node);
  }
}


class CheckSerializedFieldsAndTypesVisitor final : public ASTVisitorFunctionBody {

  static void check_type_fits_cell_or_has_policy(TypePtr serialized_type) {
    if (const TypeDataStruct* s_struct = serialized_type->unwrap_alias()->try_as<TypeDataStruct>()) {
      check_struct_fits_cell_or_has_policy(s_struct);
    } else if (const TypeDataUnion* s_union = serialized_type->unwrap_alias()->try_as<TypeDataUnion>()) {
      for (TypePtr variant : s_union->variants) {
        check_type_fits_cell_or_has_policy(variant);
      }
    } else if (const TypeDataArray* s_array = serialized_type->unwrap_alias()->try_as<TypeDataArray>()) {
      check_type_fits_cell_or_has_policy(s_array->innerT);
    }
  }

  static void check_struct_fits_cell_or_has_policy(const TypeDataStruct* t_struct) {
    StructPtr struct_ref = t_struct->struct_ref;
    bool avoid_check = struct_ref->is_instantiation_of_UnsafeBodyNoRef();
    if (avoid_check) {
      return;
    }

    PackSize size = estimate_serialization_size(t_struct);
    if ((size.max_bits > 1023 || size.max_refs > 4) && !size.is_unpredictable_infinity()) {
      if (struct_ref->overflow1023_policy == StructData::Overflow1023Policy::not_specified) {
        err_theoretical_overflow_1023(struct_ref, size).collect(struct_ref->ident_anchor);
      }
    }
    // don't check Cell<T> fields for overflow of T: it would be checked on load() or other interaction with T
  }

  void visit(V<ast_function_call> v) override {
    parent::visit(v);

    FunctionPtr fun_ref = v->fun_maybe;
    if (!fun_ref || !fun_ref->is_builtin() || !fun_ref->is_instantiation_of_generic_function()) {
      return;
    }

    if (fun_ref->base_fun_ref->name == "createEmptyMap" || fun_ref->base_fun_ref->name == "createMapFromLowLevelDict") {
      check_map_TKey_TValue(v->range, fun_ref->substitutedTs->typeT_at(0), fun_ref->substitutedTs->typeT_at(1));
      return;
    }
    
    TypePtr serialized_type = nullptr;
    bool is_pack = false;
    if (!is_serialization_builtin_function(fun_ref, &serialized_type, &is_pack)) {
      return;
    }

    std::string because_msg;
    if (!check_struct_can_be_packed_or_unpacked(serialized_type, is_pack, &because_msg)) {
      std::string via_name = fun_ref->is_method() ? fun_ref->method_name : fun_ref->base_fun_ref->name;
      err("auto-serialization via {}() is not available for type `{}`\n{}", via_name, serialized_type, because_msg).collect(v, cur_f);
      return;  // don't check overflow if serialization is not available
    }

    if (!fun_ref->name.starts_with("reflect.")) {
      check_type_fits_cell_or_has_policy(serialized_type);
    }
  }

  void visit(V<ast_local_var_lhs> v) override {
    check_mapKV_inside_type(v->type_node);
  }

  void visit(V<ast_is_type_operator> v) override {
    check_mapKV_inside_type(v->type_node);
    parent::visit(v);
  }

  void visit(V<ast_cast_as_operator> v) override {
    check_mapKV_inside_type(v->type_node);
    parent::visit(v);
  }

  void visit(V<ast_square_brackets> v) override {
    check_mapKV_inside_type(v->type_node);
    parent::visit(v);
  }

public:
  bool should_visit_function(FunctionPtr fun_ref) override {
    return fun_ref->is_code_function() && !fun_ref->is_generic_function();
  }

  void on_enter_function(V<ast_function_declaration> v_function) override {
    for (int i = 0; i < cur_f->get_num_params(); ++i) {
      check_mapKV_inside_type(cur_f->get_param(i).type_node);
    }
  }
};

void pipeline_check_serialized_fields() {
  CheckSerializedFieldsAndTypesVisitor visitor;
  visit_ast_of_all_functions(visitor);

  for (StructPtr struct_ref : get_all_declared_structs()) {
    for (StructFieldPtr field_ref : struct_ref->fields) {
      check_mapKV_inside_type(field_ref->type_node);
    }
  }
  for (GlobalVarPtr glob_ref : get_all_declared_global_vars()) {
    check_mapKV_inside_type(glob_ref->type_node);
  }
  for (EnumDefPtr enum_ref : get_all_declared_enums()) {
    if (enum_ref->colon_type_node) {
      check_enum_colon_type_to_be_intN(enum_ref->colon_type_node);
    }
  }
}

} // namespace tolk
