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
#include "platform-utils.h"
#include "src-file.h"
#include "ast.h"
#include "compiler-state.h"
#include "generics-helpers.h"
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
  tolk_assert(false);
}

static int calculate_tvm_method_id_by_func_name(std::string_view func_name) {
  unsigned int crc = td::crc16(static_cast<std::string>(func_name));
  return static_cast<int>(crc & 0xffff) | 0x10000;
}

static void validate_arg_ret_order_of_asm_function(V<ast_asm_body> v_body, int n_params) {
  if (n_params > 16) {
    v_body->error("asm function can have at most 16 parameters");
  }

  // asm(param1 ... paramN), param names were previously mapped into indices
  if (!v_body->arg_order.empty()) {
    if (static_cast<int>(v_body->arg_order.size()) != n_params) {
      v_body->error("arg_order of asm function must specify all parameters");
    }
    std::vector<bool> visited(v_body->arg_order.size(), false);
    for (int j : v_body->arg_order) {
      if (visited[j]) {
        v_body->error("arg_order of asm function contains duplicates");
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
        v_body->error("ret_order contains invalid integer, not in range 0 .. N");
      }
      visited[j] = true;
    }
  }
}

static GlobalConstPtr register_constant(V<ast_constant_declaration> v) {
  GlobalConstData* c_sym = new GlobalConstData(static_cast<std::string>(v->get_identifier()->name), v->loc, v->type_node, v->get_init_value());

  G.symtable.add_global_const(c_sym);
  G.all_constants.push_back(c_sym);
  v->mutate()->assign_const_ref(c_sym);
  return c_sym;
}

static GlobalVarPtr register_global_var(V<ast_global_var_declaration> v) {
  GlobalVarData* g_sym = new GlobalVarData(static_cast<std::string>(v->get_identifier()->name), v->loc, v->type_node);

  G.symtable.add_global_var(g_sym);
  G.all_global_vars.push_back(g_sym);
  v->mutate()->assign_glob_ref(g_sym);
  return g_sym;
}

static AliasDefPtr register_type_alias(V<ast_type_alias_declaration> v, AliasDefPtr base_alias_ref = nullptr, std::string override_name = {}, const GenericsSubstitutions* substitutedTs = nullptr) {
  std::string name = std::move(override_name);
  if (name.empty()) {
    name = v->get_identifier()->name;
  }
  const GenericsDeclaration* genericTs = nullptr;   // at registering it's null; will be assigned after types resolving
  AliasDefData* a_sym = new AliasDefData(std::move(name), v->loc, v->underlying_type_node, genericTs, substitutedTs, v);
  a_sym->base_alias_ref = base_alias_ref;   // for `Response<int>`, here is `Response<T>`

  G.symtable.add_type_alias(a_sym);
  v->mutate()->assign_alias_ref(a_sym);
  return a_sym;
}

static StructPtr register_struct(V<ast_struct_declaration> v, StructPtr base_struct_ref = nullptr, std::string override_name = {}, const GenericsSubstitutions* substitutedTs = nullptr) {
  auto v_body = v->get_struct_body();

  std::vector<StructFieldPtr> fields;
  fields.reserve(v_body->get_num_fields());
  for (int i = 0; i < v_body->get_num_fields(); ++i) {
    auto v_field = v_body->get_field(i);
    std::string field_name = static_cast<std::string>(v_field->get_identifier()->name);
    AnyExprV default_value = v_field->has_default_value() ? v_field->get_default_value() : nullptr;

    for (StructFieldPtr prev : fields) {
      if (UNLIKELY(prev->name == field_name)) {
        v_field->error("redeclaration of field `" + field_name + "`");
      }
    }
    fields.emplace_back(new StructFieldData(static_cast<std::string>(v_field->get_identifier()->name), v_field->loc, i, v_field->type_node, default_value));
  }

  std::string name = std::move(override_name);
  if (name.empty()) {
    name = v->get_identifier()->name;
  }
  const GenericsDeclaration* genericTs = nullptr;   // at registering it's null; will be assigned after types resolving
  StructData* s_sym = new StructData(std::move(name), v->loc, std::move(fields), genericTs, substitutedTs, v);
  s_sym->base_struct_ref = base_struct_ref;   // for `Container<int>`, here is `Container<T>`

  G.symtable.add_struct(s_sym);
  G.all_structs.push_back(s_sym);
  v->mutate()->assign_struct_ref(s_sym);
  return s_sym;
}

static LocalVarData register_parameter(V<ast_parameter> v, int idx) {
  if (v->is_underscore()) {
    return LocalVarData{"", v->loc, v->type_node, 0, idx};
  }

  int flags = 0;
  if (v->declared_as_mutate) {
    flags |= LocalVarData::flagMutateParameter;
  }
  if (!v->declared_as_mutate && idx == 0 && v->param_name == "self") {
    flags |= LocalVarData::flagImmutable;
  }
  return LocalVarData(static_cast<std::string>(v->param_name), v->loc, v->type_node, flags, idx);
}

static FunctionPtr register_function(V<ast_function_declaration> v, FunctionPtr base_fun_ref = nullptr, std::string override_name = {}, const GenericsSubstitutions* substitutedTs = nullptr) {
  if (v->is_builtin_function()) {
    return nullptr;
  }

  std::string_view f_identifier = v->get_identifier()->name;   // function or method name

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
  FunctionData* f_sym = new FunctionData(std::move(name), v->loc, std::move(method_name), v->receiver_type_node, v->return_type_node, std::move(parameters), 0, genericTs, substitutedTs, f_body, v);
  f_sym->base_fun_ref = base_fun_ref;   // for `f<int>`, here is `f<T>`

  if (auto v_asm = v->get_body()->try_as<ast_asm_body>()) {
    if (!v->return_type_node) {
      v_asm->error("asm function must declare return type (before asm instructions)");
    }
    validate_arg_ret_order_of_asm_function(v_asm, v->get_num_params());
    f_sym->arg_order = v_asm->arg_order;
    f_sym->ret_order = v_asm->ret_order;
  }

  if (v->tvm_method_id.not_null()) {
    f_sym->tvm_method_id = static_cast<int>(v->tvm_method_id->to_long());
  } else if (v->flags & FunctionData::flagContractGetter) {
    f_sym->tvm_method_id = calculate_tvm_method_id_by_func_name(f_identifier);
    for (FunctionPtr other : G.all_contract_getters) {
      if (other->tvm_method_id == f_sym->tvm_method_id) {
        v->error(PSTRING() << "GET methods hash collision: `" << other->name << "` and `" << f_sym->name << "` produce the same hash. Consider renaming one of these functions.");
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

StructPtr pipeline_register_instantiated_generic_struct(StructPtr base_struct_ref, AnyV cloned_v, std::string&& name, const GenericsSubstitutions* substitutedTs) {
  auto v = cloned_v->as<ast_struct_declaration>();
  return register_struct(v, base_struct_ref, std::move(name), substitutedTs);
}

AliasDefPtr pipeline_register_instantiated_generic_alias(AliasDefPtr base_alias_ref, AnyV cloned_v, std::string&& name, const GenericsSubstitutions* substitutedTs) {
  auto v = cloned_v->as<ast_type_alias_declaration>();
  return register_type_alias(v, base_alias_ref, std::move(name), substitutedTs);
}

} // namespace tolk
