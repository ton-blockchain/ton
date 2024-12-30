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
#include "constant-evaluator.h"
#include "generics-helpers.h"
#include "td/utils/crypto.h"
#include "type-system.h"
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

static int calculate_method_id_for_entrypoint(std::string_view func_name) {
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

static int calculate_method_id_by_func_name(std::string_view func_name) {
  unsigned int crc = td::crc16(static_cast<std::string>(func_name));
  return static_cast<int>(crc & 0xffff) | 0x10000;
}

static void validate_arg_ret_order_of_asm_function(V<ast_asm_body> v_body, int n_params, TypePtr ret_type) {
  if (!ret_type) {
    v_body->error("asm function must declare return type (before asm instructions)");
  }
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

static const GenericsDeclaration* construct_genericTs(V<ast_genericsT_list> v_list) {
  std::vector<GenericsDeclaration::GenericsItem> itemsT;
  itemsT.reserve(v_list->size());

  for (int i = 0; i < v_list->size(); ++i) {
    auto v_item = v_list->get_item(i);
    auto it_existing = std::find_if(itemsT.begin(), itemsT.end(), [v_item](const GenericsDeclaration::GenericsItem& prev) {
      return prev.nameT == v_item->nameT;
    });
    if (it_existing != itemsT.end()) {
      v_item->error("duplicate generic parameter `" + static_cast<std::string>(v_item->nameT) + "`");
    }
    itemsT.emplace_back(v_item->nameT);
  }

  return new GenericsDeclaration(std::move(itemsT));
}

static void register_constant(V<ast_constant_declaration> v) {
  ConstantValue init_value = eval_const_init_value(v->get_init_value());
  GlobalConstData* c_sym = new GlobalConstData(static_cast<std::string>(v->get_identifier()->name), v->loc, v->declared_type, std::move(init_value));

  if (v->declared_type) {
    bool ok = (c_sym->is_int_const() && (v->declared_type == TypeDataInt::create()))
           || (c_sym->is_slice_const() && (v->declared_type == TypeDataSlice::create()));
    if (!ok) {
      v->error("expression type does not match declared type");
    }
  }

  G.symtable.add_global_const(c_sym);
  G.all_constants.push_back(c_sym);
  v->mutate()->assign_const_ref(c_sym);
}

static void register_global_var(V<ast_global_var_declaration> v) {
  GlobalVarData* g_sym = new GlobalVarData(static_cast<std::string>(v->get_identifier()->name), v->loc, v->declared_type);

  G.symtable.add_global_var(g_sym);
  G.all_global_vars.push_back(g_sym);
  v->mutate()->assign_var_ref(g_sym);
}

static LocalVarData register_parameter(V<ast_parameter> v, int idx) {
  if (v->is_underscore()) {
    return {"", v->loc, v->declared_type, 0, idx};
  }

  int flags = 0;
  if (v->declared_as_mutate) {
    flags |= LocalVarData::flagMutateParameter;
  }
  if (!v->declared_as_mutate && idx == 0 && v->param_name == "self") {
    flags |= LocalVarData::flagImmutable;
  }
  return LocalVarData(static_cast<std::string>(v->param_name), v->loc, v->declared_type, flags, idx);
}

static void register_function(V<ast_function_declaration> v) {
  std::string_view func_name = v->get_identifier()->name;

  // calculate TypeData of a function
  std::vector<TypePtr> arg_types;
  std::vector<LocalVarData> parameters;
  int n_params = v->get_num_params();
  int n_mutate_params = 0;
  arg_types.reserve(n_params);
  parameters.reserve(n_params);
  for (int i = 0; i < n_params; ++i) {
    auto v_param = v->get_param(i);
    arg_types.emplace_back(v_param->declared_type);
    parameters.emplace_back(register_parameter(v_param, i));
    n_mutate_params += static_cast<int>(v_param->declared_as_mutate);
  }

  const GenericsDeclaration* genericTs = nullptr;
  if (v->genericsT_list) {
    genericTs = construct_genericTs(v->genericsT_list);
  }
  if (v->is_builtin_function()) {
    const Symbol* builtin_func = lookup_global_symbol(func_name);
    const FunctionData* fun_ref = builtin_func ? builtin_func->as<FunctionData>() : nullptr;
    if (!fun_ref || !fun_ref->is_builtin_function()) {
      v->error("`builtin` used for non-builtin function");
    }
    v->mutate()->assign_fun_ref(fun_ref);
    return;
  }

  if (G.is_verbosity(1) && v->is_code_function()) {
    std::cerr << "fun " << func_name << " : " << v->declared_return_type << std::endl;
  }

  FunctionBody f_body = v->get_body()->type == ast_sequence ? static_cast<FunctionBody>(new FunctionBodyCode) : static_cast<FunctionBody>(new FunctionBodyAsm);
  FunctionData* f_sym = new FunctionData(static_cast<std::string>(func_name), v->loc, v->declared_return_type, std::move(parameters), 0, genericTs, nullptr, f_body, v);

  if (const auto* v_asm = v->get_body()->try_as<ast_asm_body>()) {
    validate_arg_ret_order_of_asm_function(v_asm, v->get_num_params(), v->declared_return_type);
    f_sym->arg_order = v_asm->arg_order;
    f_sym->ret_order = v_asm->ret_order;
  }

  if (v->method_id.not_null()) {
    f_sym->method_id = static_cast<int>(v->method_id->to_long());
  } else if (v->flags & FunctionData::flagGetMethod) {
    f_sym->method_id = calculate_method_id_by_func_name(func_name);
    for (const FunctionData* other : G.all_get_methods) {
      if (other->method_id == f_sym->method_id) {
        v->error(PSTRING() << "GET methods hash collision: `" << other->name << "` and `" << f_sym->name << "` produce the same hash. Consider renaming one of these functions.");
      }
    }
  } else if (v->flags & FunctionData::flagIsEntrypoint) {
    f_sym->method_id = calculate_method_id_for_entrypoint(func_name);
  }
  f_sym->flags |= v->flags;
  if (n_mutate_params) {
    f_sym->flags |= FunctionData::flagHasMutateParams;
  }

  G.symtable.add_function(f_sym);
  G.all_functions.push_back(f_sym);
  if (f_sym->is_get_method()) {
    G.all_get_methods.push_back(f_sym);
  }
  v->mutate()->assign_fun_ref(f_sym);
}

static void iterate_through_file_symbols(const SrcFile* file) {
  static std::unordered_set<const SrcFile*> seen;
  if (!seen.insert(file).second) {
    return;
  }
  tolk_assert(file && file->ast);

  for (AnyV v : file->ast->as<ast_tolk_file>()->get_toplevel_declarations()) {
    switch (v->type) {
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

} // namespace tolk
