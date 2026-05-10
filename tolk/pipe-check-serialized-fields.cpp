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
#include "compiler-state.h"
#include "contract-directive.h"
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
    if (const TypeDataAlias* t_alias = child->try_as<TypeDataAlias>()) {
      check_mapKV_inside_type(range, t_alias->underlying_type);
    } else if (const TypeDataMapKV* t_map = child->try_as<TypeDataMapKV>(); t_map && !t_map->has_genericT_inside()) {
      check_map_TKey_TValue(range, t_map->TKey, t_map->TValue);
    }
    return child;
  });
}

static void check_mapKV_inside_type(AnyTypeV type_node) {
  if (type_node) {
    TypePtr checked_type = type_node->resolved_type->unwrap_alias();
    if (checked_type->has_mapKV_inside()) {
      check_mapKV_inside_type(type_node->range, checked_type);
    }
  }
}

// check `@abi.clientType` annotation over a struct field: it must be serializable
static void check_abi_client_type_can_be_serialized(StructFieldPtr field_ref) {
  std::string because_msg;
  if (!check_struct_can_be_packed_or_unpacked(field_ref->abi_client_type, true, &because_msg)) {
    err("invalid `@abi.clientType`: type `{}` can not be serialized\n{}", field_ref->abi_client_type, because_msg).collect(field_ref->abi_type_node);
  }
}

// validate a contract directive item (e.g., `incomingMessages: T`) to be serializable
static void check_contract_directive_item(AnyTypeV type_node, std::string_view prop_name) {
  if (type_node == nullptr) {
    return;
  }
  TypePtr root_type = type_node->resolved_type;
  if (root_type == nullptr || root_type == TypeDataNullLiteral::create()) {
    return;
  }

  std::string because_msg;
  if (!check_struct_can_be_packed_or_unpacked(root_type, true, &because_msg) ||
      !check_struct_can_be_packed_or_unpacked(root_type, false, &because_msg)) {
    err("`{}` can not be serialized: type `{}`\n{}", prop_name, root_type, because_msg).collect(type_node);
  }
}

// validate every type referenced by the `contract {...}` directive of the entrypoint file;
// prevent `storage: int` and other non-serializable types to leak into `out.abi.json`
static void check_contract_directive_serializability() {
  SrcFilePtr entrypoint_file = G.all_src_files.get_entrypoint_file();
  if (!entrypoint_file->has_contract_directive()) {
    return;
  }
  const ContractDirective* d = entrypoint_file->contract_directive;

  check_contract_directive_item(d->incomingMessages,    "incomingMessages");
  check_contract_directive_item(d->incomingExternal,    "incomingExternal");
  check_contract_directive_item(d->outgoingMessages,    "outgoingMessages");
  check_contract_directive_item(d->emittedEvents,       "emittedEvents");
  check_contract_directive_item(d->storage,             "storage");
  check_contract_directive_item(d->storageAtDeployment, "storageAtDeployment");
  // forceAbiExport is not required to be serialized (maybe, it's used only to refer to from TS)
  // thrownErrors is validated separately (must be an enum) in pipe-collect-abi.cpp
}

// given `enum Role: int8` check colon type (not struct/slice etc.)
static bool check_enum_colon_type_to_be_intN(EnumDefPtr enum_ref, AnyTypeV colon_type_node) {
  bool colon_valid = true;
  if (!colon_type_node->resolved_type->try_as<TypeDataIntN>() && !colon_type_node->resolved_type->try_as<TypeDataCoins>()) {
    err("serialization type of `enum` must be intN: `int8` / `uint32` / etc.").collect(colon_type_node);
    colon_valid = false;
  }
  // having `enum Some: int8` validate that all members fit int8
  if (const TypeDataIntN* t_intN = colon_type_node->resolved_type->try_as<TypeDataIntN>(); t_intN && !t_intN->is_variadic) {
    for (EnumMemberPtr member_ref : enum_ref->members) {
      if (!member_ref->computed_value->fits_bits(t_intN->n_bits, !t_intN->is_unsigned)) {
        err("member `{}` = {} does not fit into `{}`", member_ref->name, member_ref->computed_value->to_dec_string(), t_intN).collect(enum_ref->ident_anchor);
        colon_valid = false;
      }
    }
  }
  return colon_valid;
}


class CheckSerializedFieldsAndTypesVisitor final : public ASTVisitorFunctionBody {

  static void check_type_fits_cell_or_has_policy(TypePtr serialized_type) {
    if (serialized_type->try_as<TypeDataAlias>() && get_custom_pack_unpack_function(serialized_type)) {
      return;   // we can't analyze custom serializers, don't go deep
    }
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
    if (fun_ref && fun_ref->receiver_type) {
      check_mapKV_inside_type(v->range, fun_ref->receiver_type);
    }
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

  void visit(V<ast_reference> v) override {
    if (v->sym->try_as<FunctionPtr>()) {
      check_mapKV_inside_type(v->range, v->inferred_type);
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
    check_mapKV_inside_type(cur_f->return_type_node);
  }
};

void pipeline_check_serialized_fields() {
  bool all_enums_valid = true;
  for (EnumDefPtr enum_ref : get_all_declared_enums()) {
    if (enum_ref->colon_type_node) {
      all_enums_valid &= check_enum_colon_type_to_be_intN(enum_ref, enum_ref->colon_type_node);
    }
  }
  if (!all_enums_valid) {
    return;   // otherwise, we can't estimate size of serialized types that contain invalid enums
  }

  CheckSerializedFieldsAndTypesVisitor visitor;
  visit_ast_of_all_functions(visitor);

  for (StructPtr struct_ref : get_all_declared_structs()) {
    for (StructFieldPtr field_ref : struct_ref->fields) {
      check_mapKV_inside_type(field_ref->type_node);
      check_mapKV_inside_type(field_ref->abi_type_node);
      if (field_ref->abi_client_type) {
        check_abi_client_type_can_be_serialized(field_ref);
      }
    }
  }
  for (GlobalVarPtr glob_ref : get_all_declared_global_vars()) {
    check_mapKV_inside_type(glob_ref->type_node);
  }
  for (GlobalConstPtr const_ref : get_all_declared_constants()) {
    check_mapKV_inside_type(const_ref->type_node);
  }

  check_contract_directive_serializability();
}

} // namespace tolk
