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

    In addition, as a special exception, the copyright holders give permission
    to link the code of portions of this program with the OpenSSL library.
    You must obey the GNU General Public License in all respects for all
    of the code used other than OpenSSL. If you modify file(s) with this
    exception, you may extend this exception to your version of the file(s),
    but you are not obligated to do so. If you do not wish to do so, delete this
    exception statement from your version. If you delete this exception statement
    from all source files in the program, then also delete it here.
*/
#include "tolk.h"
#include "platform-utils.h"
#include "src-file.h"
#include "ast.h"
#include "compiler-state.h"
#include "td/utils/crypto.h"
#include <unordered_set>

namespace tolk {

Expr* process_expr(AnyV v, CodeBlob& code);

GNU_ATTRIBUTE_NORETURN GNU_ATTRIBUTE_COLD
static void fire_error_redefinition_of_symbol(V<ast_identifier> v_ident, SymDef* existing) {
  if (existing->loc.is_stdlib()) {
    v_ident->error("redefinition of a symbol from stdlib");
  } else if (existing->loc.is_defined()) {
    v_ident->error("redefinition of symbol, previous was at: " + existing->loc.to_string());
  } else {
    v_ident->error("redefinition of built-in symbol");
  }
}

static int calc_sym_idx(std::string_view sym_name) {
  return G.symbols.lookup_add(sym_name);
}

static td::RefInt256 calculate_method_id_for_entrypoint(std::string_view func_name) {
  if (func_name == "main" || func_name == "onInternalMessage") {
    return td::make_refint(0);
  }
  if (func_name == "onExternalMessage") {
    return td::make_refint(-1);
  }
  if (func_name == "onRunTickTock") {
    return td::make_refint(-2);
  }
  if (func_name == "onSplitPrepare") {
    return td::make_refint(-3);
  }
  if (func_name == "onSplitInstall") {
    return td::make_refint(-4);
  }
  tolk_assert(false);
}

static td::RefInt256 calculate_method_id_by_func_name(std::string_view func_name) {
  unsigned int crc = td::crc16(static_cast<std::string>(func_name));
  return td::make_refint((crc & 0xffff) | 0x10000);
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
    int arg_width = v_param->param_type->get_width();
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
  AnyV init_value = v->get_init_value();
  SymDef* sym_def = define_global_symbol(calc_sym_idx(v->get_identifier()->name), v->loc);
  if (sym_def->value) {
    fire_error_redefinition_of_symbol(v->get_identifier(), sym_def);
  }

  // todo currently, constant value calculation is dirty and roughly: init_value is evaluated to fif code
  // and waited to be a single expression
  // although it works, of course it should be later rewritten using AST calculations, as well as lots of other parts
  CodeBlob code("tmp", v->loc, nullptr, nullptr);
  Expr* x = process_expr(init_value, code);
  if (!x->is_rvalue()) {
    v->get_init_value()->error("expression is not strictly Rvalue");
  }
  if (v->declared_type && !v->declared_type->equals_to(x->e_type)) {
    v->error("expression type does not match declared type");
  }
  SymValConst* sym_val = nullptr;
  if (x->cls == Expr::_Const) {  // Integer constant
    sym_val = new SymValConst(static_cast<int>(G.all_constants.size()), x->intval);
  } else if (x->cls == Expr::_SliceConst) {  // Slice constant (string)
    sym_val = new SymValConst(static_cast<int>(G.all_constants.size()), x->strval);
  } else if (x->cls == Expr::_Apply) {  // even "1 + 2" is Expr::_Apply (it applies `_+_`)
    code.emplace_back(v->loc, Op::_Import, std::vector<var_idx_t>());
    auto tmp_vars = x->pre_compile(code);
    code.emplace_back(v->loc, Op::_Return, std::move(tmp_vars));
    code.emplace_back(v->loc, Op::_Nop);
    // It is REQUIRED to execute "optimizations" as in tolk.cpp
    code.simplify_var_types();
    code.prune_unreachable_code();
    code.split_vars(true);
    for (int i = 0; i < 16; i++) {
      code.compute_used_code_vars();
      code.fwd_analyze();
      code.prune_unreachable_code();
    }
    code.mark_noreturn();
    AsmOpList out_list(0, &code.vars);
    code.generate_code(out_list);
    if (out_list.list_.size() != 1) {
      init_value->error("precompiled expression must result in single operation");
    }
    auto op = out_list.list_[0];
    if (!op.is_const()) {
      init_value->error("precompiled expression must result in compilation time constant");
    }
    if (op.origin.is_null() || !op.origin->is_valid()) {
      init_value->error("precompiled expression did not result in a valid integer constant");
    }
    sym_val = new SymValConst(static_cast<int>(G.all_constants.size()), op.origin);
  } else {
    init_value->error("integer or slice literal or constant expected");
  }

  sym_def->value = sym_val;
#ifdef TOLK_DEBUG
  sym_def->value->sym_name = v->get_identifier()->name;
#endif
  G.all_constants.push_back(sym_def);
}

static void register_global_var(V<ast_global_var_declaration> v) {
  SymDef* sym_def = define_global_symbol(calc_sym_idx(v->get_identifier()->name), v->loc);
  if (sym_def->value) {
    fire_error_redefinition_of_symbol(v->get_identifier(), sym_def);
  }

  sym_def->value = new SymValGlobVar(static_cast<int>(G.all_global_vars.size()), v->declared_type);
#ifdef TOLK_DEBUG
  sym_def->value->sym_name = v->get_identifier()->name;
#endif
  G.all_global_vars.push_back(sym_def);
}

static SymDef* register_parameter(V<ast_parameter> v, int idx) {
  if (v->is_underscore()) {
    return nullptr;
  }
  SymDef* sym_def = define_parameter(calc_sym_idx(v->get_identifier()->name), v->loc);
  if (sym_def->value) {
    // todo always false now, how to detect similar parameter names? (remember about underscore)
    v->error("redefined parameter");
  }

  SymValVariable* sym_val = new SymValVariable(idx, v->param_type);
  if (v->declared_as_mutate) {
    sym_val->flags |= SymValVariable::flagMutateParameter;
  }
  if (!v->declared_as_mutate && idx == 0 && v->get_identifier()->name == "self") {
    sym_val->flags |= SymValVariable::flagImmutable;
  }
  sym_def->value = sym_val;
#ifdef TOLK_DEBUG
  sym_def->value->sym_name = v->get_identifier()->name;
#endif
  return sym_def;
}

static void register_function(V<ast_function_declaration> v) {
  std::string_view func_name = v->get_identifier()->name;

  // calculate TypeExpr of a function: it's a map (params -> ret), probably surrounded by forall
  TypeExpr* params_tensor_type = nullptr;
  int n_params = v->get_num_params();
  int n_mutate_params = 0;
  std::vector<SymDef*> parameters_syms;
  if (n_params) {
    std::vector<TypeExpr*> param_tensor_items;
    param_tensor_items.reserve(n_params);
    parameters_syms.reserve(n_params);
    for (int i = 0; i < n_params; ++i) {
      auto v_param = v->get_param(i);
      n_mutate_params += static_cast<int>(v_param->declared_as_mutate);
      param_tensor_items.emplace_back(v_param->param_type);
      parameters_syms.emplace_back(register_parameter(v_param, i));
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
    const SymDef* builtin_func = lookup_symbol(G.symbols.lookup(func_name));
    const SymValFunc* func_val = builtin_func ? dynamic_cast<SymValFunc*>(builtin_func->value) : nullptr;
    if (!func_val || !func_val->is_builtin()) {
      v->error("`builtin` used for non-builtin function");
    }
#ifdef TOLK_DEBUG
    // in release, we don't need this check, since `builtin` is used only in stdlib, which is our responsibility
    if (!func_val->sym_type->equals_to(function_type) || func_val->is_marked_as_pure() != v->marked_as_pure) {
      v->error("declaration for `builtin` function doesn't match an actual one");
    }
#endif
    return;
  }

  SymDef* sym_def = define_global_symbol(calc_sym_idx(func_name), v->loc);
  if (sym_def->value) {
    fire_error_redefinition_of_symbol(v->get_identifier(), sym_def);
  }
  if (G.is_verbosity(1)) {
    std::cerr << "fun " << func_name << " : " << function_type << std::endl;
  }
  if (v->marked_as_pure && v->ret_type->get_width() == 0) {
    v->error("a pure function should return something, otherwise it will be optimized out anyway");
  }

  SymValFunc* sym_val = nullptr;
  if (const auto* v_seq = v->get_body()->try_as<ast_sequence>()) {
    sym_val = new SymValCodeFunc(std::move(parameters_syms), static_cast<int>(G.all_code_functions.size()), function_type);
  } else if (const auto* v_asm = v->get_body()->try_as<ast_asm_body>()) {
    std::vector<int> arg_order, ret_order;
    calc_arg_ret_order_of_asm_function(v_asm, v->get_param_list(), v->ret_type, arg_order, ret_order);
    sym_val = new SymValAsmFunc(std::move(parameters_syms), function_type, std::move(arg_order), std::move(ret_order), 0);
  } else {
    v->error("Unexpected function body statement");
  }

  if (v->method_id) {
    sym_val->method_id = td::string_to_int256(static_cast<std::string>(v->method_id->int_val));
    if (sym_val->method_id.is_null()) {
      v->method_id->error("invalid integer constant");
    }
  } else if (v->marked_as_get_method) {
    sym_val->method_id = calculate_method_id_by_func_name(func_name);
    for (const SymDef* other : G.all_get_methods) {
      if (!td::cmp(dynamic_cast<const SymValFunc*>(other->value)->method_id, sym_val->method_id)) {
        v->error(PSTRING() << "GET methods hash collision: `" << other->name() << "` and `" << static_cast<std::string>(func_name) << "` produce the same hash. Consider renaming one of these functions.");
      }
    }
  } else if (v->is_entrypoint) {
    sym_val->method_id = calculate_method_id_for_entrypoint(func_name);
  }
  if (v->marked_as_pure) {
    sym_val->flags |= SymValFunc::flagMarkedAsPure;
  }
  if (v->marked_as_inline) {
    sym_val->flags |= SymValFunc::flagInline;
  }
  if (v->marked_as_inline_ref) {
    sym_val->flags |= SymValFunc::flagInlineRef;
  }
  if (v->marked_as_get_method) {
    sym_val->flags |= SymValFunc::flagGetMethod;
  }
  if (v->is_entrypoint) {
    sym_val->flags |= SymValFunc::flagIsEntrypoint;
  }
  if (n_mutate_params) {
    sym_val->flags |= SymValFunc::flagHasMutateParams;
  }
  if (v->accepts_self) {
    sym_val->flags |= SymValFunc::flagAcceptsSelf;
  }
  if (v->returns_self) {
    sym_val->flags |= SymValFunc::flagReturnsSelf;
  }

  sym_def->value = sym_val;
#ifdef TOLK_DEBUG
  sym_def->value->sym_name = func_name;
#endif
  if (dynamic_cast<SymValCodeFunc*>(sym_val)) {
    G.all_code_functions.push_back(sym_def);
  }
  if (sym_val->is_get_method()) {
    G.all_get_methods.push_back(sym_def);
  }
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
