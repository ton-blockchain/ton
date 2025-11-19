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
#include "src-file.h"
#include "compilation-errors.h"
#include "compiler-state.h"
#include "generics-helpers.h"
#include "pack-unpack-serializers.h"
#include "td/utils/crypto.h"
#include <unordered_set>

/*
 *   This pipe registers global symbols: functions, constants, global vars, etc.
 *   It happens just after all files have been parsed to AST.
 *
 *   "Registering" means adding symbols to a global symbol table.
 *   After this pass, any global symbol can be looked up.
 *   Note, that local variables are not analyzed here, it's a later step.
 * Before digging into locals, we need a global symtable to be filled, exactly done here.
 */

namespace tolk {

static int calculate_tvm_method_id_for_entrypoint(std::string_view func_name) {
  if (func_name == "main" || func_name == "onInternalMessage") {
    return 0;
  }
  if (func_name == "onExternalMessage") {
    return -1;
  }
  if (func_name == "onRunTickTock") {
    return -2;
  }
  if (func_name == "onSplitPrepare") {
    return -3;
  }
  if (func_name == "onSplitInstall") {
    return -4;
  }
  if (func_name == "onBouncedMessage") {
    return FunctionData::EMPTY_TVM_METHOD_ID;
  }
  tolk_assert(false);
}

static int calculate_tvm_method_id_by_func_name(std::string_view func_name) {
  unsigned int crc = td::crc16(static_cast<std::string>(func_name));
  return static_cast<int>(crc & 0xffff) | 0x10000;
}

static void validate_arg_ret_order_of_asm_function(V<ast_asm_body> v_body, int n_params) {
  if (n_params > 16) {
    err("asm function can have at most 16 parameters").fire(v_body);
  }

  // asm(param1 ... paramN), param names were previously mapped into indices
  if (!v_body->arg_order.empty()) {
    if (static_cast<int>(v_body->arg_order.size()) != n_params) {
      err("arg_order of asm function must specify all parameters").fire(v_body);
    }
    std::vector<bool> visited(v_body->arg_order.size(), false);
    for (int j : v_body->arg_order) {
      if (visited[j]) {
        err("arg_order of asm function contains duplicates").fire(v_body);
      }
      visited[j] = true;
    }
  }

  // asm(-> 0 2 1 3), check for a shuffled range 0...N
  // correctness of N (actual return width onto a stack) will be checked after type inferring and generics instantiation
  if (!v_body->ret_order.empty()) {
    std::vector<bool> visited(v_body->ret_order.size(), false);
    for (int j : v_body->ret_order) {
      if (j < 0 || j >= static_cast<int>(v_body->ret_order.size()) || visited[j]) {
        err("ret_order contains invalid integer, not in range 0 .. N").fire(v_body);
      }
      visited[j] = true;
    }
  }
}

static GlobalConstPtr register_constant(V<ast_constant_declaration> v) {
  V<ast_identifier> v_ident = v->get_identifier();
  GlobalConstData* c_sym = new GlobalConstData(static_cast<std::string>(v_ident->name), v_ident, v->type_node, v->get_init_value());

  G.symtable.add_global_symbol(c_sym);
  G.all_constants.push_back(c_sym);
  v->mutate()->assign_const_ref(c_sym);
  return c_sym;
}

static GlobalVarPtr register_global_var(V<ast_global_var_declaration> v) {
  V<ast_identifier> v_ident = v->get_identifier();
  GlobalVarData* g_sym = new GlobalVarData(static_cast<std::string>(v_ident->name), v_ident, v->type_node);

  G.symtable.add_global_symbol(g_sym);
  G.all_global_vars.push_back(g_sym);
  v->mutate()->assign_glob_ref(g_sym);
  return g_sym;
}

static AliasDefPtr register_type_alias(V<ast_type_alias_declaration> v, AliasDefPtr base_alias_ref = nullptr, std::string override_name = {}, const GenericsSubstitutions* substitutedTs = nullptr) {
  V<ast_identifier> v_ident = v->get_identifier();
  std::string name = std::move(override_name);
  if (name.empty()) {
    name = v_ident->name;
  }
  const GenericsDeclaration* genericTs = nullptr;   // at registering it's null; will be assigned after types resolving
  AliasDefData* a_sym = new AliasDefData(std::move(name), v_ident, v->underlying_type_node, genericTs, substitutedTs, v);
  a_sym->base_alias_ref = base_alias_ref;   // for `Response<int>`, here is `Response<T>`

  G.symtable.add_global_symbol(a_sym);
  v->mutate()->assign_alias_ref(a_sym);
  return a_sym;
}

static EnumDefPtr register_enum(V<ast_enum_declaration> v) {
  auto v_body = v->get_enum_body();

  std::vector<EnumMemberPtr> members;
  members.reserve(v_body->get_num_members());
  for (int i = 0; i < v_body->get_num_members(); ++i) {
    auto v_member = v_body->get_member(i);
    V<ast_identifier> v_ident = v_member->get_identifier();
    std::string member_name = static_cast<std::string>(v_ident->name);

    for (EnumMemberPtr prev : members) {
      if (prev->name == member_name) {
        err("redeclaration of member `{}`", member_name).fire(v_member);
      }
    }
    members.emplace_back(new EnumMemberData(std::move(member_name), v_ident, i, v_member->init_value));
  }

  V<ast_identifier> v_ident = v->get_identifier();
  EnumDefData* e_sym = new EnumDefData(static_cast<std::string>(v_ident->name), v_ident, v->colon_type, std::move(members));

  G.symtable.add_global_symbol(e_sym);
  G.all_enums.push_back(e_sym);
  v->mutate()->assign_enum_ref(e_sym);
  return e_sym;
}

static StructPtr register_struct(V<ast_struct_declaration> v, StructPtr base_struct_ref = nullptr, std::string override_name = {}, const GenericsSubstitutions* substitutedTs = nullptr) {
  auto v_body = v->get_struct_body();

  std::vector<StructFieldPtr> fields;
  fields.reserve(v_body->get_num_fields());
  for (int i = 0; i < v_body->get_num_fields(); ++i) {
    auto v_field = v_body->get_field(i);
    V<ast_identifier> v_ident = v_field->get_identifier();
    std::string field_name = static_cast<std::string>(v_ident->name);

    for (StructFieldPtr prev : fields) {
      if (prev->name == field_name) {
        err("redeclaration of field `{}`", field_name).fire(v_field);
      }
    }
    fields.emplace_back(new StructFieldData(std::move(field_name), v_ident, i, v_field->is_private, v_field->is_readonly, v_field->type_node, v_field->default_value));
  }

  PackOpcode opcode(0, 0);
  if (v->has_opcode()) {
    auto v_opcode = v->get_opcode()->as<ast_int_const>();
    if (v_opcode->intval < 0 || v_opcode->intval > (1ULL << 48)) {
      err("opcode must not exceed 2^48").fire(v);
    }
    opcode.pack_prefix = v_opcode->intval->to_long();

    std::string_view prefix_str = v_opcode->orig_str;
    if (prefix_str.starts_with("0x")) {
      opcode.prefix_len = static_cast<int>(prefix_str.size() - 2) * 4;
    } else if (prefix_str.starts_with("0b")) {
      opcode.prefix_len = static_cast<int>(prefix_str.size() - 2);
    } else {
      tolk_assert(false);
    }
  }

  V<ast_identifier> v_ident = v->get_identifier();
  std::string name = std::move(override_name);
  if (name.empty()) {
    name = v_ident->name;
  }
  const GenericsDeclaration* genericTs = nullptr;   // at registering it's null; will be assigned after types resolving
  StructData* s_sym = new StructData(std::move(name), v_ident, std::move(fields), opcode, v->overflow1023_policy, genericTs, substitutedTs, v);
  s_sym->base_struct_ref = base_struct_ref;   // for `Container<int>`, here is `Container<T>`

  G.symtable.add_global_symbol(s_sym);
  G.all_structs.push_back(s_sym);
  v->mutate()->assign_struct_ref(s_sym);
  return s_sym;
}

static LocalVarData register_parameter(V<ast_parameter> v, int idx) {
  V<ast_identifier> v_ident = v->get_identifier();
  if (v_ident->name == "_") {
    return LocalVarData("", v, v->type_node, v->default_value, 0, idx);
  }

  int flags = 0;
  if (v->declared_as_mutate) {
    flags |= LocalVarData::flagMutateParameter;
  }
  if (!v->declared_as_mutate && idx == 0 && v_ident->name == "self") {
    flags |= LocalVarData::flagImmutable;
  }
  return LocalVarData(static_cast<std::string>(v_ident->name), v_ident, v->type_node, v->default_value, flags, idx);
}

static FunctionPtr register_function(V<ast_function_declaration> v, FunctionPtr base_fun_ref = nullptr, std::string override_name = {}, const GenericsSubstitutions* substitutedTs = nullptr) {
  if (v->is_builtin_function()) {
    return nullptr;
  }

  V<ast_identifier> v_ident = v->get_identifier();
  std::string_view f_identifier = v_ident->name;   // function or method name

  std::vector<LocalVarData> parameters;
  int n_mutate_params = 0;
  parameters.reserve(v->get_num_params());
  for (int i = 0; i < v->get_num_params(); ++i) {
    auto v_param = v->get_param(i);
    parameters.emplace_back(register_parameter(v_param, i));
    n_mutate_params += static_cast<int>(v_param->declared_as_mutate);
  }

  std::string method_name;
  if (v->receiver_type_node) {
    method_name = f_identifier;
  }
  std::string name = std::move(override_name);
  if (name.empty()) {
    name = f_identifier;
  }

  const GenericsDeclaration* genericTs = nullptr;   // at registering it's null; will be assigned after types resolving
  FunctionBody f_body = v->get_body()->kind == ast_block_statement ? static_cast<FunctionBody>(new FunctionBodyCode) : static_cast<FunctionBody>(new FunctionBodyAsm);
  FunctionData* f_sym = new FunctionData(std::move(name), v_ident, std::move(method_name), v->receiver_type_node, v->return_type_node, std::move(parameters), 0, v->inline_mode, genericTs, substitutedTs, f_body, v);
  f_sym->base_fun_ref = base_fun_ref;   // for `f<int>`, here is `f<T>`; for a lambda, a containing function

  if (auto v_asm = v->get_body()->try_as<ast_asm_body>()) {
    if (!v->return_type_node) {
      err("asm function must declare return type (before asm instructions)").fire(v_asm);
    }
    validate_arg_ret_order_of_asm_function(v_asm, v->get_num_params());
    f_sym->arg_order = v_asm->arg_order;
    f_sym->ret_order = v_asm->ret_order;
  }

  if (v->tvm_method_id != FunctionData::EMPTY_TVM_METHOD_ID) {
    f_sym->tvm_method_id = v->tvm_method_id;
  } else if (v->flags & FunctionData::flagContractGetter) {
    f_sym->tvm_method_id = calculate_tvm_method_id_by_func_name(f_identifier);
    for (FunctionPtr other : G.all_contract_getters) {
      if (other->tvm_method_id == f_sym->tvm_method_id) {
        err("GET methods hash collision: `{}` and `{}` produce the same hash. Consider renaming one of these functions.", other, f_sym).fire(v);
      }
    }
  } else if (v->flags & FunctionData::flagIsEntrypoint) {
    f_sym->tvm_method_id = calculate_tvm_method_id_for_entrypoint(f_identifier);
  }
  f_sym->flags |= v->flags;
  if (n_mutate_params) {
    f_sym->flags |= FunctionData::flagHasMutateParams;
  }

  if (!f_sym->receiver_type_node) {
    G.symtable.add_function(f_sym);
  } else if (!substitutedTs) {
    G.all_methods.push_back(f_sym);
  }
  G.all_functions.push_back(f_sym);
  if (f_sym->is_contract_getter()) {
    G.all_contract_getters.push_back(f_sym);
  }
  v->mutate()->assign_fun_ref(f_sym);
  return f_sym;
}

static void iterate_through_file_symbols(const SrcFile* file) {
  static std::unordered_set<const SrcFile*> seen;
  if (!seen.insert(file).second) {
    return;
  }
  tolk_assert(file && file->ast);

  for (AnyV v : file->ast->as<ast_tolk_file>()->get_toplevel_declarations()) {
    switch (v->kind) {
      case ast_import_directive:
        // on `import "another-file.tolk"`, register symbols from that file at first
        // (for instance, it can calculate constants, which are used in init_val of constants in current file below import)
        iterate_through_file_symbols(v->as<ast_import_directive>()->file);
        break;

      case ast_constant_declaration:
        register_constant(v->as<ast_constant_declaration>());
        break;
      case ast_global_var_declaration:
        register_global_var(v->as<ast_global_var_declaration>());
        break;
      case ast_type_alias_declaration:
        register_type_alias(v->as<ast_type_alias_declaration>());
        break;
      case ast_enum_declaration:
        register_enum(v->as<ast_enum_declaration>());
        break;
      case ast_struct_declaration:
        register_struct(v->as<ast_struct_declaration>());
        break;
      case ast_function_declaration:
        register_function(v->as<ast_function_declaration>());
        break;
      default:
        break;
    }
  }
}

void pipeline_register_global_symbols() {
  for (const SrcFile* file : G.all_src_files) {
    iterate_through_file_symbols(file);
  }
}

FunctionPtr pipeline_register_instantiated_generic_function(FunctionPtr base_fun_ref, AnyV cloned_v, std::string&& name, const GenericsSubstitutions* substitutedTs) {
  auto v = cloned_v->as<ast_function_declaration>();
  return register_function(v, base_fun_ref, std::move(name), substitutedTs);
}

FunctionPtr pipeline_register_instantiated_lambda_function(FunctionPtr base_fun_ref, AnyV cloned_v, std::string&& name) {
  auto v = cloned_v->as<ast_function_declaration>();
  return register_function(v, base_fun_ref, std::move(name));
}

StructPtr pipeline_register_instantiated_generic_struct(StructPtr base_struct_ref, AnyV cloned_v, std::string&& name, const GenericsSubstitutions* substitutedTs) {
  auto v = cloned_v->as<ast_struct_declaration>();
  return register_struct(v, base_struct_ref, std::move(name), substitutedTs);
}

AliasDefPtr pipeline_register_instantiated_generic_alias(AliasDefPtr base_alias_ref, AnyV cloned_v, std::string&& name, const GenericsSubstitutions* substitutedTs) {
  auto v = cloned_v->as<ast_type_alias_declaration>();
  return register_type_alias(v, base_alias_ref, std::move(name), substitutedTs);
}

} // namespace tolk
