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

static void calc_arg_ret_order_of_asm_function(V<ast_asm_body> v_body, V<ast_parameter_list> param_list, TypeExpr* ret_type,
                                   std::vector<int>& arg_order, std::vector<int>& ret_order) {
  int cnt = param_list->size();
  int width = ret_type->get_width();
  if (width < 0 || width > 16) {
    v_body->error("return type of an assembler built-in function must have a well-defined fixed width");
  }
  if (cnt > 16) {
    v_body->error("assembler built-in function must have at most 16 arguments");
  }
  std::vector<int> cum_arg_width;
  cum_arg_width.push_back(0);
  int tot_width = 0;
  for (int i = 0; i < cnt; ++i) {
    V<ast_parameter> v_param = param_list->get_param(i);
    int arg_width = v_param->declared_type->get_width();
    if (arg_width < 0 || arg_width > 16) {
      v_param->error("parameters of an assembler built-in function must have a well-defined fixed width");
    }
    cum_arg_width.push_back(tot_width += arg_width);
  }
  if (!v_body->arg_order.empty()) {
    if (static_cast<int>(v_body->arg_order.size()) != cnt) {
      v_body->error("arg_order of asm function must specify all parameters");
    }
    std::vector<bool> visited(cnt, false);
    for (int i = 0; i < cnt; ++i) {
      int j = v_body->arg_order[i];
      if (visited[j]) {
        v_body->error("arg_order of asm function contains duplicates");
      }
      visited[j] = true;
      int c1 = cum_arg_width[j], c2 = cum_arg_width[j + 1];
      while (c1 < c2) {
        arg_order.push_back(c1++);
      }
    }
    tolk_assert(arg_order.size() == (unsigned)tot_width);
  }
  if (!v_body->ret_order.empty()) {
    if (static_cast<int>(v_body->ret_order.size()) != width) {
      v_body->error("ret_order of this asm function expected to be width = " + std::to_string(width));
    }
    std::vector<bool> visited(width, false);
    for (int i = 0; i < width; ++i) {
      int j = v_body->ret_order[i];
      if (j < 0 || j >= width || visited[j]) {
        v_body->error("ret_order contains invalid integer, not in range 0 .. width-1");
      }
      visited[j] = true;
    }
    ret_order = v_body->ret_order;
  }
}

static void register_constant(V<ast_constant_declaration> v) {
  ConstantValue init_value = eval_const_init_value(v->get_init_value());
  GlobalConstData* c_sym = new GlobalConstData(static_cast<std::string>(v->get_identifier()->name), v->loc, std::move(init_value));

  if (v->declared_type && !v->declared_type->equals_to(c_sym->inferred_type)) {
    v->error("expression type does not match declared type");
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
    return {"", v->loc, idx, v->declared_type};
  }

  LocalVarData p_sym(static_cast<std::string>(v->param_name), v->loc, idx, v->declared_type);
  if (v->declared_as_mutate) {
    p_sym.flags |= LocalVarData::flagMutateParameter;
  }
  if (!v->declared_as_mutate && idx == 0 && v->param_name == "self") {
    p_sym.flags |= LocalVarData::flagImmutable;
  }
  return p_sym;
}

static void register_function(V<ast_function_declaration> v) {
  std::string_view func_name = v->get_identifier()->name;

  // calculate TypeExpr of a function: it's a map (params -> ret), probably surrounded by forall
  TypeExpr* params_tensor_type = nullptr;
  int n_params = v->get_num_params();
  int n_mutate_params = 0;
  std::vector<LocalVarData> parameters;
  if (n_params) {
    std::vector<TypeExpr*> param_tensor_items;
    param_tensor_items.reserve(n_params);
    parameters.reserve(n_params);
    for (int i = 0; i < n_params; ++i) {
      auto v_param = v->get_param(i);
      n_mutate_params += static_cast<int>(v_param->declared_as_mutate);
      param_tensor_items.emplace_back(v_param->declared_type);
      parameters.emplace_back(register_parameter(v_param, i));
    }
    params_tensor_type = TypeExpr::new_tensor(std::move(param_tensor_items));
  } else {
    params_tensor_type = TypeExpr::new_unit();
  }

  TypeExpr* function_type = TypeExpr::new_map(params_tensor_type, v->ret_type);
  if (v->genericsT_list) {
    std::vector<TypeExpr*> type_vars;
    type_vars.reserve(v->genericsT_list->size());
    for (int idx = 0; idx < v->genericsT_list->size(); ++idx) {
      type_vars.emplace_back(v->genericsT_list->get_item(idx)->created_type);
    }
    function_type = TypeExpr::new_forall(std::move(type_vars), function_type);
  }
  if (v->marked_as_builtin) {
    const Symbol* builtin_func = lookup_global_symbol(func_name);
    const FunctionData* func_val = builtin_func ? builtin_func->as<FunctionData>() : nullptr;
    if (!func_val || !func_val->is_builtin_function()) {
      v->error("`builtin` used for non-builtin function");
    }
#ifdef TOLK_DEBUG
    // in release, we don't need this check, since `builtin` is used only in stdlib, which is our responsibility
    if (!func_val->full_type->equals_to(function_type) || func_val->is_marked_as_pure() != v->marked_as_pure) {
      v->error("declaration for `builtin` function doesn't match an actual one");
    }
#endif
    return;
  }

  if (G.is_verbosity(1)) {
    std::cerr << "fun " << func_name << " : " << function_type << std::endl;
  }
  if (v->marked_as_pure && v->ret_type->get_width() == 0) {
    v->error("a pure function should return something, otherwise it will be optimized out anyway");
  }

  FunctionBody f_body = v->get_body()->type == ast_sequence ? static_cast<FunctionBody>(new FunctionBodyCode) : static_cast<FunctionBody>(new FunctionBodyAsm);
  FunctionData* f_sym = new FunctionData(static_cast<std::string>(func_name), v->loc, function_type, std::move(parameters), 0, f_body);

  if (const auto* v_asm = v->get_body()->try_as<ast_asm_body>()) {
    calc_arg_ret_order_of_asm_function(v_asm, v->get_param_list(), v->ret_type, f_sym->arg_order, f_sym->ret_order);
  }

  if (v->method_id) {
    if (v->method_id->intval.is_null() || !v->method_id->intval->signed_fits_bits(32)) {
      v->method_id->error("invalid integer constant");
    }
    f_sym->method_id = static_cast<int>(v->method_id->intval->to_long());
  } else if (v->marked_as_get_method) {
    f_sym->method_id = calculate_method_id_by_func_name(func_name);
    for (const FunctionData* other : G.all_get_methods) {
      if (other->method_id == f_sym->method_id) {
        v->error(PSTRING() << "GET methods hash collision: `" << other->name << "` and `" << f_sym->name << "` produce the same hash. Consider renaming one of these functions.");
      }
    }
  } else if (v->is_entrypoint) {
    f_sym->method_id = calculate_method_id_for_entrypoint(func_name);
  }
  if (v->marked_as_pure) {
    f_sym->flags |= FunctionData::flagMarkedAsPure;
  }
  if (v->marked_as_inline) {
    f_sym->flags |= FunctionData::flagInline;
  }
  if (v->marked_as_inline_ref) {
    f_sym->flags |= FunctionData::flagInlineRef;
  }
  if (v->marked_as_get_method) {
    f_sym->flags |= FunctionData::flagGetMethod;
  }
  if (v->is_entrypoint) {
    f_sym->flags |= FunctionData::flagIsEntrypoint;
  }
  if (n_mutate_params) {
    f_sym->flags |= FunctionData::flagHasMutateParams;
  }
  if (v->accepts_self) {
    f_sym->flags |= FunctionData::flagAcceptsSelf;
  }
  if (v->returns_self) {
    f_sym->flags |= FunctionData::flagReturnsSelf;
  }

  G.symtable.add_function(f_sym);
  if (f_sym->is_regular_function()) {
    G.all_code_functions.push_back(f_sym);
  }
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
      case ast_import_statement:
        // on `import "another-file.tolk"`, register symbols from that file at first
        // (for instance, it can calculate constants, which are used in init_val of constants in current file below import)
        iterate_through_file_symbols(v->as<ast_import_statement>()->file);
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

void pipeline_register_global_symbols(const AllSrcFiles& all_src_files) {
  for (const SrcFile* file : all_src_files) {
    iterate_through_file_symbols(file);
  }
}

} // namespace tolk
