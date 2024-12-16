/*
    This file is part of TON Blockchain Library.

    TON Blockchain Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    TON Blockchain Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with TON Blockchain Library.  If not, see <http://www.gnu.org/licenses/>.
*/
#include "tolk.h"
#include "src-file.h"
#include "ast.h"
#include "compiler-state.h"
#include "common/refint.h"
#include "constant-evaluator.h"

/*
 *   This pipe is the last one operating AST: it transforms AST to IR.
 *   IR is described as "Op" struct. So, here AST is transformed to Ops, and then all the rest "legacy"
 * kernel (initially forked from FunC) comes into play.
 *   Up to this point, all types have been inferred, all validity checks have been passed, etc.
 * All properties in AST nodes are assigned and can be safely used (fun_ref, etc.).
 * So, if execution reaches this pass, the input is correct, and code generation should succeed.
 */

namespace tolk {

struct LValGlobs {
  std::vector<std::pair<const GlobalVarData*, var_idx_t>> globs;

  void add_modified_glob(const GlobalVarData* g_sym, var_idx_t local_ir_idx) {
    globs.emplace_back(g_sym, local_ir_idx);
  }

  void gen_ops_set_globs(CodeBlob& code, SrcLocation loc) const {
    for (const auto& [g_sym, ir_idx] : globs) {
      Op& op = code.emplace_back(loc, Op::_SetGlob, std::vector<var_idx_t>{}, std::vector<var_idx_t>{ ir_idx }, g_sym);
      op.set_impure_flag();
    }
  }
};

std::vector<var_idx_t> pre_compile_expr(AnyExprV v, CodeBlob& code, LValGlobs* lval_globs = nullptr);
void process_statement(AnyV v, CodeBlob& code);


static std::vector<std::vector<var_idx_t>> pre_compile_tensor_inner(CodeBlob& code, const std::vector<AnyExprV>& args,
                                          LValGlobs* lval_globs) {
  const int n = static_cast<int>(args.size());
  if (n == 0) {  // just `()`
    return {};
  }
  if (n == 1) {  // just `(x)`: even if x is modified (e.g. `f(x=x+2)`), there are no next arguments
    return {pre_compile_expr(args[0], code, lval_globs)};
  }

  // the purpose is to handle such cases: `return (x, x += y, x)`
  // without this, ops will be { _Call $2 = +($0_x, $1_y); _Return $0_x, $2, $0_x } - invalid
  // with this, ops will be { _Let $3 = $0_x; _Call $2 = ...; _Return $3, $2, $0_x } - valid, tmp var for x
  // how it works: for every arg, after transforming to ops, start tracking ir_idx inside it
  // on modification attempt, create Op::_Let to a tmp var and replace old ir_idx with tmp_idx in result
  struct WatchingVarList {
    std::vector<var_idx_t> watched_vars;
    std::vector<std::vector<var_idx_t>> res_lists;

    explicit WatchingVarList(int n_args) {
      res_lists.reserve(n_args);
    }

    bool is_watched(var_idx_t ir_idx) const {
      return std::find(watched_vars.begin(), watched_vars.end(), ir_idx) != watched_vars.end();
    }

    void add_and_watch_modifications(std::vector<var_idx_t>&& vars_of_ith_arg, CodeBlob& code) {
      for (var_idx_t ir_idx : vars_of_ith_arg) {
        if (code.vars[ir_idx].v_sym && !is_watched(ir_idx)) {
          watched_vars.emplace_back(ir_idx);
          code.vars[ir_idx].on_modification.emplace_back([this, &code, ir_idx](SrcLocation loc) {
            on_var_modified(ir_idx, loc, code);
          });
        }
      }
      res_lists.emplace_back(std::move(vars_of_ith_arg));
    }

    void on_var_modified(var_idx_t ir_idx, SrcLocation loc, CodeBlob& code) {
      tolk_assert(is_watched(ir_idx));
      var_idx_t tmp_idx = code.create_tmp_var(code.vars[ir_idx].v_type, loc);
      code.emplace_back(loc, Op::_Let, std::vector{tmp_idx}, std::vector{ir_idx});
      for (std::vector<var_idx_t>& prev_vars : res_lists) {
        std::replace(prev_vars.begin(), prev_vars.end(), ir_idx, tmp_idx);
      }
    }

    std::vector<std::vector<var_idx_t>> clear_and_stop_watching(CodeBlob& code) {
      for (var_idx_t ir_idx : watched_vars) {
        code.vars[ir_idx].on_modification.pop_back();
      }
      watched_vars.clear();
      return std::move(res_lists);
    }
  };

  WatchingVarList watched_vars(n);
  for (int arg_idx = 0; arg_idx < n; ++arg_idx) {
    std::vector<var_idx_t> vars_of_ith_arg = pre_compile_expr(args[arg_idx], code, lval_globs);
    watched_vars.add_and_watch_modifications(std::move(vars_of_ith_arg), code);
  }
  return watched_vars.clear_and_stop_watching(code);
}

static std::vector<var_idx_t> pre_compile_tensor(CodeBlob& code, const std::vector<AnyExprV>& args,
                                          LValGlobs* lval_globs = nullptr) {
  std::vector<std::vector<var_idx_t>> res_lists = pre_compile_tensor_inner(code, args, lval_globs);
  std::vector<var_idx_t> res;
  for (const std::vector<var_idx_t>& list : res_lists) {
    res.insert(res.end(), list.cbegin(), list.cend());
  }
  return res;
}

static std::vector<var_idx_t> pre_compile_let(CodeBlob& code, AnyExprV lhs, AnyExprV rhs, SrcLocation loc) {
  // [lhs] = [rhs]; since type checking is ok, it's the same as "lhs = rhs"
  if (lhs->type == ast_tensor_square && rhs->type == ast_tensor_square) {
    std::vector<var_idx_t> right = pre_compile_tensor(code, rhs->as<ast_tensor_square>()->get_items());
    LValGlobs globs;
    std::vector<var_idx_t> left = pre_compile_tensor(code, lhs->as<ast_tensor_square>()->get_items(), &globs);
    code.on_var_modification(left, loc);
    code.emplace_back(loc, Op::_Let, std::move(left), right);
    globs.gen_ops_set_globs(code, loc);
    return right;
  }
  // [lhs] = rhs; it's un-tuple to N left vars
  if (lhs->type == ast_tensor_square) {
    std::vector<var_idx_t> right = pre_compile_expr(rhs, code);
    TypeExpr* rhs_type = rhs->inferred_type;
    TypeExpr::remove_indirect(rhs_type);
    TypeExpr* unpacked_type = rhs_type->args.at(0);   // rhs->inferred_type is tuple<tensor<...>>
    std::vector<var_idx_t> rvect = {code.create_tmp_var(unpacked_type, rhs->loc)};
    code.emplace_back(lhs->loc, Op::_UnTuple, rvect, std::move(right));
    LValGlobs globs;
    std::vector<var_idx_t> left = pre_compile_tensor(code, lhs->as<ast_tensor_square>()->get_items(), &globs);
    code.on_var_modification(left, loc);
    code.emplace_back(loc, Op::_Let, std::move(left), rvect);
    globs.gen_ops_set_globs(code, loc);
    return rvect;
  }
  // lhs = rhs
  std::vector<var_idx_t> right = pre_compile_expr(rhs, code);
  LValGlobs globs;
  std::vector<var_idx_t> left = pre_compile_expr(lhs, code, &globs);
  code.on_var_modification(left, loc);
  code.emplace_back(loc, Op::_Let, std::move(left), right);
  globs.gen_ops_set_globs(code, loc);
  return right;
}

static std::vector<var_idx_t> gen_op_call(CodeBlob& code, TypeExpr* ret_type, SrcLocation here,
                                          std::vector<var_idx_t>&& args_vars, const FunctionData* fun_ref) {
  std::vector<var_idx_t> rvect = {code.create_tmp_var(ret_type, here)};
  Op& op = code.emplace_back(here, Op::_Call, rvect, std::move(args_vars), fun_ref);
  if (!fun_ref->is_marked_as_pure()) {
    op.set_impure_flag();
  }
  return rvect;
}


static std::vector<var_idx_t> process_binary_operator(V<ast_binary_operator> v, CodeBlob& code) {
  TokenType t = v->tok;
  std::string operator_name = static_cast<std::string>(v->operator_name);

  if (v->is_set_assign()) {
    std::string_view calc_operator = std::string_view{operator_name}.substr(0, operator_name.size() - 1);
    auto v_apply = createV<ast_binary_operator>(v->loc, calc_operator, static_cast<TokenType>(t - 1), v->get_lhs(), v->get_rhs());
    v_apply->assign_inferred_type(v->inferred_type);
    return pre_compile_let(code, v->get_lhs(), v_apply, v->loc);
  }
  if (v->is_assign()) {
    return pre_compile_let(code, v->get_lhs(), v->get_rhs(), v->loc);
  }
  if (t == tok_minus || t == tok_plus ||
      t == tok_bitwise_and || t == tok_bitwise_or || t == tok_bitwise_xor ||
      t == tok_eq || t == tok_lt || t == tok_gt || t == tok_leq || t == tok_geq || t == tok_neq || t == tok_spaceship ||
      t == tok_lshift || t == tok_rshift || t == tok_rshiftC || t == tok_rshiftR ||
      t == tok_mul || t == tok_div || t == tok_mod || t == tok_divC || t == tok_divR) {
    const FunctionData* fun_ref = lookup_global_symbol("_" + operator_name + "_")->as<FunctionData>();
    std::vector<var_idx_t> args_vars = pre_compile_tensor(code, {v->get_lhs(), v->get_rhs()});
    return gen_op_call(code, v->inferred_type, v->loc, std::move(args_vars), fun_ref);
  }
  if (t == tok_logical_and || t == tok_logical_or) {
    // do the following transformations:
    // a && b  ->  a ? (b != 0) : 0
    // a || b  ->  a ? 1 : (b != 0)
    AnyExprV v_0 = createV<ast_int_const>(v->loc, td::make_refint(0), "0");
    v_0->mutate()->assign_inferred_type(TypeExpr::new_atomic(TypeExpr::_Int));
    AnyExprV v_1 = createV<ast_int_const>(v->loc, td::make_refint(-1), "-1");
    v_1->mutate()->assign_inferred_type(TypeExpr::new_atomic(TypeExpr::_Int));
    AnyExprV v_b_ne_0 = createV<ast_binary_operator>(v->loc, "!=", tok_neq, v->get_rhs(), v_0);
    v_b_ne_0->mutate()->assign_inferred_type(TypeExpr::new_atomic(TypeExpr::_Int));
    std::vector<var_idx_t> cond = pre_compile_expr(v->get_lhs(), code);
    tolk_assert(cond.size() == 1);
    std::vector<var_idx_t> rvect = {code.create_tmp_var(v->inferred_type, v->loc)};
    Op& if_op = code.emplace_back(v->loc, Op::_If, cond);
    code.push_set_cur(if_op.block0);
    code.emplace_back(v->loc, Op::_Let, rvect, pre_compile_expr(t == tok_logical_and ? v_b_ne_0 : v_1, code));
    code.close_pop_cur(v->loc);
    code.push_set_cur(if_op.block1);
    code.emplace_back(v->loc, Op::_Let, rvect, pre_compile_expr(t == tok_logical_and ? v_0 : v_b_ne_0, code));
    code.close_pop_cur(v->loc);
    return rvect;
  }

  throw UnexpectedASTNodeType(v, "process_binary_operator");
}

static std::vector<var_idx_t> process_unary_operator(V<ast_unary_operator> v, CodeBlob& code) {
  const FunctionData* fun_ref = lookup_global_symbol(static_cast<std::string>(v->operator_name) + "_")->as<FunctionData>();
  std::vector<var_idx_t> args_vars = pre_compile_tensor(code, {v->get_rhs()});
  return gen_op_call(code, v->inferred_type, v->loc, std::move(args_vars), fun_ref);
}

static std::vector<var_idx_t> process_ternary_operator(V<ast_ternary_operator> v, CodeBlob& code) {
  std::vector<var_idx_t> cond = pre_compile_expr(v->get_cond(), code);
  tolk_assert(cond.size() == 1);
  std::vector<var_idx_t> rvect = {code.create_tmp_var(v->inferred_type, v->loc)};
  Op& if_op = code.emplace_back(v->loc, Op::_If, cond);
  code.push_set_cur(if_op.block0);
  code.emplace_back(v->get_when_true()->loc, Op::_Let, rvect, pre_compile_expr(v->get_when_true(), code));
  code.close_pop_cur(v->get_when_true()->loc);
  code.push_set_cur(if_op.block1);
  code.emplace_back(v->get_when_false()->loc, Op::_Let, rvect, pre_compile_expr(v->get_when_false(), code));
  code.close_pop_cur(v->get_when_false()->loc);
  return rvect;
}

static std::vector<var_idx_t> process_function_call(V<ast_function_call> v, CodeBlob& code) {
  // most likely it's a global function, but also may be `some_var(args)` or even `getF()(args)`
  const FunctionData* fun_ref = v->fun_maybe;
  if (!fun_ref) {
    std::vector<AnyExprV> args;
    args.reserve(v->get_num_args());
    for (int i = 0; i < v->get_num_args(); ++i) {
      args.push_back(v->get_arg(i)->get_expr());
    }
    std::vector<var_idx_t> args_vars = pre_compile_tensor(code, args);
    std::vector<var_idx_t> tfunc = pre_compile_expr(v->get_called_f(), code);
    tolk_assert(tfunc.size() == 1);
    args_vars.push_back(tfunc[0]);
    std::vector<var_idx_t> rvect = {code.create_tmp_var(v->inferred_type, v->loc)};
    Op& op = code.emplace_back(v->loc, Op::_CallInd, rvect, std::move(args_vars));
    op.set_impure_flag();
    return rvect;
  }

  std::vector<AnyExprV> args;
  args.reserve(v->get_num_args());
  for (int i = 0; i < v->get_num_args(); ++i) {
    args.push_back(v->get_arg(i)->get_expr());
  }
  std::vector<var_idx_t> args_vars = pre_compile_tensor(code, args);

  TypeExpr* op_call_type = v->inferred_type;
  if (fun_ref->has_mutate_params()) {
    std::vector<TypeExpr*> types_list;
    for (int i = 0; i < v->get_num_args(); ++i) {
      if (fun_ref->parameters[i].is_mutate_parameter()) {
        types_list.push_back(args[i]->inferred_type);
      }
    }
    types_list.push_back(v->inferred_type);
    op_call_type = TypeExpr::new_tensor(std::move(types_list));
  }

  std::vector<var_idx_t> rvect_apply = gen_op_call(code, op_call_type, v->loc, std::move(args_vars), fun_ref);

  if (fun_ref->has_mutate_params()) {
    LValGlobs local_globs;
    std::vector<var_idx_t> left;
    for (int i = 0; i < v->get_num_args(); ++i) {
      if (fun_ref->parameters[i].is_mutate_parameter()) {
        AnyExprV arg_i = v->get_arg(i)->get_expr();
        tolk_assert(arg_i->is_lvalue);
        std::vector<var_idx_t> ith_var_idx = pre_compile_expr(arg_i, code, &local_globs);
        left.insert(left.end(), ith_var_idx.begin(), ith_var_idx.end());
      }
    }
    std::vector<var_idx_t> rvect = {code.create_tmp_var(v->inferred_type, v->loc)};
    left.push_back(rvect[0]);
    code.on_var_modification(left, v->loc);
    code.emplace_back(v->loc, Op::_Let, std::move(left), rvect_apply);
    local_globs.gen_ops_set_globs(code, v->loc);
    return rvect;
  }

  return rvect_apply;
}

static std::vector<var_idx_t> process_dot_method_call(V<ast_dot_method_call> v, CodeBlob& code) {
  std::vector<AnyExprV> args;
  args.reserve(1 + v->get_num_args());
  args.push_back(v->get_obj());
  for (int i = 0; i < v->get_num_args(); ++i) {
    args.push_back(v->get_arg(i)->get_expr());
  }
  std::vector<std::vector<var_idx_t>> vars_per_arg = pre_compile_tensor_inner(code, args, nullptr);

  TypeExpr* op_call_type = v->inferred_type;
  TypeExpr* real_ret_type = v->inferred_type;
  if (v->fun_ref->does_return_self()) {
    real_ret_type = TypeExpr::new_unit();
    if (!v->fun_ref->parameters[0].is_mutate_parameter()) {
      op_call_type = TypeExpr::new_unit();
    }
  }
  if (v->fun_ref->has_mutate_params()) {
    std::vector<TypeExpr*> types_list;
    for (int i = 0; i < 1 + v->get_num_args(); ++i) {
      if (v->fun_ref->parameters[i].is_mutate_parameter()) {
        types_list.push_back(args[i]->inferred_type);
      }
    }
    types_list.push_back(real_ret_type);
    op_call_type = TypeExpr::new_tensor(std::move(types_list));
  }

  std::vector<var_idx_t> args_vars;
  for (const std::vector<var_idx_t>& list : vars_per_arg) {
    args_vars.insert(args_vars.end(), list.cbegin(), list.cend());
  }
  std::vector<var_idx_t> rvect_apply = gen_op_call(code, op_call_type, v->loc, std::move(args_vars), v->fun_ref);

  AnyExprV obj_leftmost = args[0];
  while (obj_leftmost->type == ast_dot_method_call && obj_leftmost->as<ast_dot_method_call>()->fun_ref->does_return_self()) {
    obj_leftmost = obj_leftmost->as<ast_dot_method_call>()->get_obj();
  }

  if (v->fun_ref->has_mutate_params()) {
    LValGlobs local_globs;
    std::vector<var_idx_t> left;
    for (int i = 0; i < 1 + v->get_num_args(); ++i) {
      if (v->fun_ref->parameters[i].is_mutate_parameter()) {
        AnyExprV arg_i = i == 0 ? obj_leftmost : args[i];
        tolk_assert (arg_i->is_lvalue || i == 0);
        if (arg_i->is_lvalue) {
          std::vector<var_idx_t> ith_var_idx = pre_compile_expr(arg_i, code, &local_globs);
          left.insert(left.end(), ith_var_idx.begin(), ith_var_idx.end());
        } else {
          left.insert(left.end(), vars_per_arg[0].begin(), vars_per_arg[0].end());
        }
      }
    }
    std::vector<var_idx_t> rvect = {code.create_tmp_var(real_ret_type, v->loc)};
    left.push_back(rvect[0]);
    code.on_var_modification(left, v->loc);
    code.emplace_back(v->loc, Op::_Let, std::move(left), rvect_apply);
    local_globs.gen_ops_set_globs(code, v->loc);
    rvect_apply = rvect;
  }

  if (v->fun_ref->does_return_self()) {
    if (obj_leftmost->is_lvalue) {    // to handle if obj is global var, potentially re-assigned inside a chain
      rvect_apply = pre_compile_expr(obj_leftmost, code);
    } else {                          // temporary object, not lvalue, pre_compile_expr
      rvect_apply = vars_per_arg[0];
    }
  }

  return rvect_apply;
}

static std::vector<var_idx_t> process_tensor(V<ast_tensor> v, CodeBlob& code, LValGlobs* lval_globs) {
  return pre_compile_tensor(code, v->get_items(), lval_globs);
}

static std::vector<var_idx_t> process_tensor_square(V<ast_tensor_square> v, CodeBlob& code, LValGlobs* lval_globs) {
  if (lval_globs) {       // todo some time, make "var (a, [b,c]) = (1, [2,3])" work
    v->error("[...] can not be used as lvalue here");
  }
  std::vector<var_idx_t> left = std::vector{code.create_tmp_var(v->inferred_type, v->loc)};
  std::vector<var_idx_t> right = pre_compile_tensor(code, v->get_items());
  code.emplace_back(v->loc, Op::_Tuple, left, std::move(right));
  return left;
}

static std::vector<var_idx_t> process_int_const(V<ast_int_const> v, CodeBlob& code) {
  std::vector<var_idx_t> rvect = {code.create_tmp_var(v->inferred_type, v->loc)};
  code.emplace_back(v->loc, Op::_IntConst, rvect, v->intval);
  return rvect;
}

static std::vector<var_idx_t> process_string_const(V<ast_string_const> v, CodeBlob& code) {
  ConstantValue value = eval_const_init_value(v);
  std::vector<var_idx_t> rvect = {code.create_tmp_var(v->inferred_type, v->loc)};
  if (value.is_int()) {
    code.emplace_back(v->loc, Op::_IntConst, rvect, value.as_int());
  } else {
    code.emplace_back(v->loc, Op::_SliceConst, rvect, value.as_slice());
  }
  return rvect;
}

static std::vector<var_idx_t> process_bool_const(V<ast_bool_const> v, CodeBlob& code) {
  const FunctionData* builtin_sym = lookup_global_symbol(v->bool_val ? "__true" : "__false")->as<FunctionData>();
  return gen_op_call(code, v->inferred_type, v->loc, {}, builtin_sym);
}

static std::vector<var_idx_t> process_null_keyword(V<ast_null_keyword> v, CodeBlob& code) {
  const FunctionData* builtin_sym = lookup_global_symbol("__null")->as<FunctionData>();
  return gen_op_call(code, v->inferred_type, v->loc, {}, builtin_sym);
}

static std::vector<var_idx_t> process_self_keyword(V<ast_self_keyword> v, CodeBlob& code) {
  tolk_assert(code.fun_ref->does_accept_self() && v->param_ref);
  tolk_assert(v->param_ref->idx == 0);
  return {0};
}

static std::vector<var_idx_t> process_identifier(V<ast_identifier> v, CodeBlob& code, LValGlobs* lval_globs) {
  const Symbol* sym = v->sym;
  if (const auto* glob_ref = sym->try_as<GlobalVarData>()) {
    std::vector<var_idx_t> rvect = {code.create_tmp_var(v->inferred_type, v->loc)};
    if (lval_globs) {
      lval_globs->add_modified_glob(glob_ref, rvect[0]);
      return rvect;
    } else {
      code.emplace_back(v->loc, Op::_GlobVar, rvect, std::vector<var_idx_t>{}, glob_ref);
      return rvect;
    }
  }
  if (const auto* const_ref = sym->try_as<GlobalConstData>()) {
    std::vector<var_idx_t> rvect = {code.create_tmp_var(v->inferred_type, v->loc)};
    if (const_ref->is_int_const()) {
      code.emplace_back(v->loc, Op::_IntConst, rvect, const_ref->as_int_const());
    } else {
      code.emplace_back(v->loc, Op::_SliceConst, rvect, const_ref->as_slice_const());
    }
    return rvect;
  }
  if (const auto* fun_ref = sym->try_as<FunctionData>()) {
    std::vector<var_idx_t> rvect = {code.create_tmp_var(v->inferred_type, v->loc)};
    code.emplace_back(v->loc, Op::_GlobVar, rvect, std::vector<var_idx_t>{}, fun_ref);
    return rvect;
  }
  if (const auto* var_ref = sym->try_as<LocalVarData>()) {
#ifdef TOLK_DEBUG
    tolk_assert(var_ref->idx != -1);
#endif
    return {var_ref->idx};
  }
  throw UnexpectedASTNodeType(v, "process_identifier");
}

static std::vector<var_idx_t> process_local_var(V<ast_local_var> v, CodeBlob& code, LValGlobs* lval_globs) {
  if (v->marked_as_redef) {
    return process_identifier(v->get_identifier()->as<ast_identifier>(), code, lval_globs);
  }
  if (v->get_identifier()->try_as<ast_identifier>()) {
    const LocalVarData* var_ref = v->var_maybe->as<LocalVarData>();
    tolk_assert(var_ref->idx == -1);
    var_ref->mutate()->assign_idx(code.create_var(v->inferred_type, var_ref, v->loc));
    return {var_ref->idx};
  }
  return {code.create_tmp_var(v->inferred_type, v->loc)};  // underscore
}

static std::vector<var_idx_t> process_underscore(V<ast_underscore> v, CodeBlob& code) {
  return {code.create_tmp_var(v->inferred_type, v->loc)};
}

std::vector<var_idx_t> pre_compile_expr(AnyExprV v, CodeBlob& code, LValGlobs* lval_globs) {
  switch (v->type) {
    case ast_binary_operator:
      return process_binary_operator(v->as<ast_binary_operator>(), code);
    case ast_unary_operator:
      return process_unary_operator(v->as<ast_unary_operator>(), code);
    case ast_ternary_operator:
      return process_ternary_operator(v->as<ast_ternary_operator>(), code);
    case ast_function_call:
      return process_function_call(v->as<ast_function_call>(), code);
    case ast_dot_method_call:
      return process_dot_method_call(v->as<ast_dot_method_call>(), code);
    case ast_parenthesized_expression:
      return pre_compile_expr(v->as<ast_parenthesized_expression>()->get_expr(), code, lval_globs);
    case ast_tensor:
      return process_tensor(v->as<ast_tensor>(), code, lval_globs);
    case ast_tensor_square:
      return process_tensor_square(v->as<ast_tensor_square>(), code, lval_globs);
    case ast_int_const:
      return process_int_const(v->as<ast_int_const>(), code);
    case ast_string_const:
      return process_string_const(v->as<ast_string_const>(), code);
    case ast_bool_const:
      return process_bool_const(v->as<ast_bool_const>(), code);
    case ast_null_keyword:
      return process_null_keyword(v->as<ast_null_keyword>(), code);
    case ast_self_keyword:
      return process_self_keyword(v->as<ast_self_keyword>(), code);
    case ast_identifier:
      return process_identifier(v->as<ast_identifier>(), code, lval_globs);
    case ast_local_var:
      return process_local_var(v->as<ast_local_var>(), code, lval_globs);
    case ast_underscore:
      return process_underscore(v->as<ast_underscore>(), code);
    default:
      throw UnexpectedASTNodeType(v, "pre_compile_expr");
  }
}


static void process_local_vars_declaration(V<ast_local_vars_declaration> v, CodeBlob& code) {
  pre_compile_let(code, v->get_lhs(), v->get_assigned_val(), v->loc);
}

static void process_sequence(V<ast_sequence> v, CodeBlob& code) {
  for (AnyV item : v->get_items()) {
    process_statement(item, code);
  }
}


static void process_assert_statement(V<ast_assert_statement> v, CodeBlob& code) {
  std::vector<AnyExprV> args(3);
  if (auto v_not = v->get_cond()->try_as<ast_unary_operator>(); v_not && v_not->tok == tok_logical_not) {
    args[0] = v->get_thrown_code();
    args[1] = v->get_cond()->as<ast_unary_operator>()->get_rhs();
    args[2] = createV<ast_bool_const>(v->loc, true);
    args[2]->mutate()->assign_inferred_type(TypeExpr::new_atomic(TypeExpr::_Int));
  } else {
    args[0] = v->get_thrown_code();
    args[1] = v->get_cond();
    args[2] = createV<ast_bool_const>(v->loc, false);
    args[2]->mutate()->assign_inferred_type(TypeExpr::new_atomic(TypeExpr::_Int));
  }

  const FunctionData* builtin_sym = lookup_global_symbol("__throw_if_unless")->as<FunctionData>();
  std::vector<var_idx_t> args_vars = pre_compile_tensor(code, args);
  gen_op_call(code, TypeExpr::new_unit(), v->loc, std::move(args_vars), builtin_sym);
}

static void process_catch_variable(AnyExprV v_catch_var, CodeBlob& code) {
  if (auto v_ident = v_catch_var->try_as<ast_identifier>()) {
    const LocalVarData* var_ref = v_ident->sym->as<LocalVarData>();
    tolk_assert(var_ref->idx == -1);
    var_ref->mutate()->assign_idx(code.create_var(v_catch_var->inferred_type, var_ref, v_catch_var->loc));
  }
}

static void process_try_catch_statement(V<ast_try_catch_statement> v, CodeBlob& code) {
  code.require_callxargs = true;
  Op& try_catch_op = code.emplace_back(v->loc, Op::_TryCatch);
  code.push_set_cur(try_catch_op.block0);
  process_statement(v->get_try_body(), code);
  code.close_pop_cur(v->get_try_body()->loc_end);
  code.push_set_cur(try_catch_op.block1);

  // transform catch (excNo, arg) into TVM-catch (arg, excNo), where arg is untyped and thus almost useless now
  const std::vector<AnyExprV>& catch_vars = v->get_catch_expr()->get_items();
  tolk_assert(catch_vars.size() == 2);
  process_catch_variable(catch_vars[0], code);
  process_catch_variable(catch_vars[1], code);
  try_catch_op.left = pre_compile_tensor(code, {catch_vars[1], catch_vars[0]});
  process_statement(v->get_catch_body(), code);
  code.close_pop_cur(v->get_catch_body()->loc_end);
}

static void process_repeat_statement(V<ast_repeat_statement> v, CodeBlob& code) {
  std::vector<var_idx_t> tmp_vars = pre_compile_expr(v->get_cond(), code);
  Op& repeat_op = code.emplace_back(v->loc, Op::_Repeat, tmp_vars);
  code.push_set_cur(repeat_op.block0);
  process_statement(v->get_body(), code);
  code.close_pop_cur(v->get_body()->loc_end);
}

static void process_if_statement(V<ast_if_statement> v, CodeBlob& code) {
  std::vector<var_idx_t> tmp_vars = pre_compile_expr(v->get_cond(), code);
  Op& if_op = code.emplace_back(v->loc, Op::_If, std::move(tmp_vars));
  code.push_set_cur(if_op.block0);
  process_statement(v->get_if_body(), code);
  code.close_pop_cur(v->get_if_body()->loc_end);
  code.push_set_cur(if_op.block1);
  process_statement(v->get_else_body(), code);
  code.close_pop_cur(v->get_else_body()->loc_end);
  if (v->is_ifnot) {
    std::swap(if_op.block0, if_op.block1);
  }
}

static void process_do_while_statement(V<ast_do_while_statement> v, CodeBlob& code) {
  Op& until_op = code.emplace_back(v->loc, Op::_Until);
  code.push_set_cur(until_op.block0);
  process_statement(v->get_body(), code);

  // in TVM, there is only "do until", but in Tolk, we want "do while"
  // here we negate condition to pass it forward to legacy to Op::_Until
  // also, handle common situations as a hardcoded "optimization": replace (a<0) with (a>=0) and so on
  // todo these hardcoded conditions should be removed from this place in the future
  AnyExprV cond = v->get_cond();
  AnyExprV until_cond;
  if (auto v_not = cond->try_as<ast_unary_operator>(); v_not && v_not->tok == tok_logical_not) {
    until_cond = v_not->get_rhs();
  } else if (auto v_eq = cond->try_as<ast_binary_operator>(); v_eq && v_eq->tok == tok_eq) {
    until_cond = createV<ast_binary_operator>(cond->loc, "!=", tok_neq, v_eq->get_lhs(), v_eq->get_rhs());
  } else if (auto v_neq = cond->try_as<ast_binary_operator>(); v_neq && v_neq->tok == tok_neq) {
    until_cond = createV<ast_binary_operator>(cond->loc, "==", tok_eq, v_neq->get_lhs(), v_neq->get_rhs());
  } else if (auto v_leq = cond->try_as<ast_binary_operator>(); v_leq && v_leq->tok == tok_leq) {
    until_cond = createV<ast_binary_operator>(cond->loc, ">", tok_gt, v_leq->get_lhs(), v_leq->get_rhs());
  } else if (auto v_lt = cond->try_as<ast_binary_operator>(); v_lt && v_lt->tok == tok_lt) {
    until_cond = createV<ast_binary_operator>(cond->loc, ">=", tok_geq, v_lt->get_lhs(), v_lt->get_rhs());
  } else if (auto v_geq = cond->try_as<ast_binary_operator>(); v_geq && v_geq->tok == tok_geq) {
    until_cond = createV<ast_binary_operator>(cond->loc, "<", tok_lt, v_geq->get_lhs(), v_geq->get_rhs());
  } else if (auto v_gt = cond->try_as<ast_binary_operator>(); v_gt && v_gt->tok == tok_gt) {
    until_cond = createV<ast_binary_operator>(cond->loc, "<=", tok_geq, v_gt->get_lhs(), v_gt->get_rhs());
  } else {
    until_cond = createV<ast_unary_operator>(cond->loc, "!", tok_logical_not, cond);
  }
  until_cond->mutate()->assign_inferred_type(TypeExpr::new_atomic(TypeExpr::_Int));

  until_op.left = pre_compile_expr(until_cond, code);
  code.close_pop_cur(v->get_body()->loc_end);
}

static void process_while_statement(V<ast_while_statement> v, CodeBlob& code) {
  Op& while_op = code.emplace_back(v->loc, Op::_While);
  code.push_set_cur(while_op.block0);
  while_op.left = pre_compile_expr(v->get_cond(), code);
  code.close_pop_cur(v->get_body()->loc);
  code.push_set_cur(while_op.block1);
  process_statement(v->get_body(), code);
  code.close_pop_cur(v->get_body()->loc_end);
}

static void process_throw_statement(V<ast_throw_statement> v, CodeBlob& code) {
  if (v->has_thrown_arg()) {
    const FunctionData* builtin_sym = lookup_global_symbol("__throw_arg")->as<FunctionData>();
    std::vector<var_idx_t> args_vars = pre_compile_tensor(code, {v->get_thrown_arg(), v->get_thrown_code()});
    gen_op_call(code, TypeExpr::new_unit(), v->loc, std::move(args_vars), builtin_sym);
  } else {
    const FunctionData* builtin_sym = lookup_global_symbol("__throw")->as<FunctionData>();
    std::vector<var_idx_t> args_vars = pre_compile_tensor(code, {v->get_thrown_code()});
    gen_op_call(code, TypeExpr::new_unit(), v->loc, std::move(args_vars), builtin_sym);
  }
}

static void process_return_statement(V<ast_return_statement> v, CodeBlob& code) {
  std::vector<var_idx_t> return_vars = pre_compile_expr(v->get_return_value(), code);
  if (code.fun_ref->does_return_self()) {
    tolk_assert(return_vars.size() == 1);
    return_vars = {};
  }
  if (code.fun_ref->has_mutate_params()) {
    std::vector<var_idx_t> mutated_vars;
    for (const LocalVarData& p_sym: code.fun_ref->parameters) {
      if (p_sym.is_mutate_parameter()) {
        mutated_vars.push_back(p_sym.idx);
      }
    }
    return_vars.insert(return_vars.begin(), mutated_vars.begin(), mutated_vars.end());
  }
  code.emplace_back(v->loc, Op::_Return, std::move(return_vars));
}

static void append_implicit_return_statement(SrcLocation loc_end, CodeBlob& code) {
  std::vector<var_idx_t> mutated_vars;
  if (code.fun_ref->has_mutate_params()) {
    for (const LocalVarData& p_sym: code.fun_ref->parameters) {
      if (p_sym.is_mutate_parameter()) {
        mutated_vars.push_back(p_sym.idx);
      }
    }
  }
  code.emplace_back(loc_end, Op::_Return, std::move(mutated_vars));
}


void process_statement(AnyV v, CodeBlob& code) {
  switch (v->type) {
    case ast_local_vars_declaration:
      return process_local_vars_declaration(v->as<ast_local_vars_declaration>(), code);
    case ast_sequence:
      return process_sequence(v->as<ast_sequence>(), code);
    case ast_return_statement:
      return process_return_statement(v->as<ast_return_statement>(), code);
    case ast_repeat_statement:
      return process_repeat_statement(v->as<ast_repeat_statement>(), code);
    case ast_if_statement:
      return process_if_statement(v->as<ast_if_statement>(), code);
    case ast_do_while_statement:
      return process_do_while_statement(v->as<ast_do_while_statement>(), code);
    case ast_while_statement:
      return process_while_statement(v->as<ast_while_statement>(), code);
    case ast_throw_statement:
      return process_throw_statement(v->as<ast_throw_statement>(), code);
    case ast_assert_statement:
      return process_assert_statement(v->as<ast_assert_statement>(), code);
    case ast_try_catch_statement:
      return process_try_catch_statement(v->as<ast_try_catch_statement>(), code);
    case ast_empty_statement:
      return;
    default:
      pre_compile_expr(reinterpret_cast<AnyExprV>(v), code);
  }
}

static void convert_function_body_to_CodeBlob(V<ast_function_declaration> v, V<ast_sequence> v_body) {
  CodeBlob* blob = new CodeBlob{static_cast<std::string>(v->get_identifier()->name), v->loc, v->fun_ref, v->ret_type};
  FormalArgList legacy_arg_list;
  for (int i = 0; i < v->get_num_params(); ++i) {
    legacy_arg_list.emplace_back(v->get_param(i)->declared_type, &v->fun_ref->parameters[i], v->loc);
  }
  blob->import_params(std::move(legacy_arg_list));

  for (AnyV item : v_body->get_items()) {
    process_statement(item, *blob);
  }
  if (v->fun_ref->is_implicit_return()) {
    append_implicit_return_statement(v_body->loc_end, *blob);
  }

  blob->close_blk(v_body->loc_end);
  std::get<FunctionBodyCode*>(v->fun_ref->body)->set_code(blob);
}

static void convert_asm_body_to_AsmOp(V<ast_function_declaration> v, V<ast_asm_body> v_body) {
  int cnt = v->get_num_params();
  int width = v->ret_type->get_width();
  std::vector<AsmOp> asm_ops;
  for (AnyV v_child : v_body->get_asm_commands()) {
    std::string_view ops = v_child->as<ast_string_const>()->str_val; // <op>\n<op>\n...
    std::string op;
    for (char c : ops) {
      if (c == '\n' || c == '\r') {
        if (!op.empty()) {
          asm_ops.push_back(AsmOp::Parse(op, cnt, width));
          if (asm_ops.back().is_custom()) {
            cnt = width;
          }
          op.clear();
        }
      } else {
        op.push_back(c);
      }
    }
    if (!op.empty()) {
      asm_ops.push_back(AsmOp::Parse(op, cnt, width));
      if (asm_ops.back().is_custom()) {
        cnt = width;
      }
    }
  }

  std::get<FunctionBodyAsm*>(v->fun_ref->body)->set_code(std::move(asm_ops));
}

void pipeline_convert_ast_to_legacy_Expr_Op(const AllSrcFiles& all_src_files) {
  for (const SrcFile* file : all_src_files) {
    for (AnyV v : file->ast->as<ast_tolk_file>()->get_toplevel_declarations()) {
      if (auto v_func = v->try_as<ast_function_declaration>()) {
        if (v_func->is_asm_function()) {
          convert_asm_body_to_AsmOp(v_func, v_func->get_body()->as<ast_asm_body>());
        } else if (!v_func->marked_as_builtin) {
          convert_function_body_to_CodeBlob(v_func, v_func->get_body()->as<ast_sequence>());
        }
      }
    }
  }
}

} // namespace tolk
