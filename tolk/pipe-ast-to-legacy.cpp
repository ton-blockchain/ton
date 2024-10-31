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
#include "openssl/digest.hpp"
#include "crypto/common/util.h"
#include "td/utils/crypto.h"
#include "ton/ton-types.h"

/*
 *   In this module, we convert modern AST representation to legacy representation
 * (global state, Expr, CodeBlob, etc.) to make the rest of compiling process remain unchanged for now.
 *   Since time goes, I'll gradually get rid of legacy, since most of the code analysis
 * should be done at AST level.
 */

namespace tolk {

static int calc_sym_idx(std::string_view sym_name) {
  return G.symbols.lookup(sym_name);
}

void Expr::fire_error_rvalue_expected() const {
  // generally, almost all vertices are rvalue, that's why code leading to "not rvalue"
  // should be very strange, like `var x = _`
  throw ParseError(here, "rvalue expected");
}

void Expr::fire_error_lvalue_expected(const std::string& details) const {
  // "lvalue expected" is when a user modifies something unmodifiable
  // example: `f() = 32`
  // example: `loadUint(c.beginParse(), 32)` (since `loadUint()` mutates the first argument)
  throw ParseError(here, "lvalue expected (" + details + ")");
}

void Expr::fire_error_modifying_immutable(const std::string& details) const {
  // "modifying immutable variable" is when a user assigns to a variable declared `val`
  // example: `immutable_val = 32`
  // example: `(regular_var, immutable_val) = f()`
  // for better error message, try to print out variable name if possible
  std::string variable_name;
  if (cls == _Var || cls == _Const) {
    variable_name = sym->name();
  } else if (cls == _Tensor || cls == _MkTuple) {
    for (const Expr* arg : (cls == _Tensor ? args : args[0]->args)) {
      if (arg->is_immutable() && (arg->cls == _Var || arg->cls == _Const)) {
        variable_name = arg->sym->name();
        break;
      }
    }
  }

  if (variable_name == "self") {
    throw ParseError(here, "modifying `self` (" + details + "), which is immutable by default; probably, you want to declare `mutate self`");
  } else if (!variable_name.empty()) {
    throw ParseError(here, "modifying an immutable variable `" + variable_name + "` (" + details + ")");
  } else {
    throw ParseError(here, "modifying an immutable variable (" + details + ")");
  }
}

GNU_ATTRIBUTE_COLD GNU_ATTRIBUTE_NORETURN
static void fire_error_invalid_mutate_arg_passed(SrcLocation loc, const SymDef* func_sym, const SymDef* param_sym, bool called_as_method, bool arg_passed_as_mutate, AnyV arg_expr) {
  std::string func_name = func_sym->name();
  std::string arg_str(arg_expr->type == ast_identifier ? arg_expr->as<ast_identifier>()->name : "obj");
  const SymValFunc* func_val = dynamic_cast<const SymValFunc*>(func_sym->value);
  const SymValVariable* param_val = dynamic_cast<const SymValVariable*>(param_sym->value);

  // case: `loadInt(cs, 32)`; suggest: `cs.loadInt(32)`
  if (param_val->is_mutate_parameter() && !arg_passed_as_mutate && !called_as_method && param_val->idx == 0 && func_val->does_accept_self()) {
    throw ParseError(loc, "`" + func_name + "` is a mutating method; consider calling `" + arg_str + "." + func_name + "()`, not `" + func_name + "(" + arg_str + ")`");
  }
  // case: `cs.mutating_function()`; suggest: `mutating_function(mutate cs)` or make it a method
  if (param_val->is_mutate_parameter() && called_as_method && param_val->idx == 0 && !func_val->does_accept_self()) {
    throw ParseError(loc, "function `" + func_name + "` mutates parameter `" + param_sym->name() + "`; consider calling `" + func_name + "(mutate " + arg_str + ")`, not `" + arg_str + "." + func_name + "`(); alternatively, rename parameter to `self` to make it a method");
  }
  // case: `mutating_function(arg)`; suggest: `mutate arg`
  if (param_val->is_mutate_parameter() && !arg_passed_as_mutate) {
    throw ParseError(loc, "function `" + func_name + "` mutates parameter `" + param_sym->name() + "`; you need to specify `mutate` when passing an argument, like `mutate " + arg_str + "`");
  }
  // case: `usual_function(mutate arg)`
  if (!param_val->is_mutate_parameter() && arg_passed_as_mutate) {
    throw ParseError(loc, "incorrect `mutate`, since `" + func_name + "` does not mutate this parameter");
  }
  throw Fatal("unreachable");
}

// parse address like "EQCRDM9h4k3UJdOePPuyX40mCgA4vxge5Dc5vjBR8djbEKC5"
// based on unpack_std_smc_addr() from block.cpp
// (which is not included to avoid linking with ton_crypto)
static bool parse_friendly_address(const char packed[48], ton::WorkchainId& workchain, ton::StdSmcAddress& addr) {
  unsigned char buffer[36];
  if (!td::buff_base64_decode(td::MutableSlice{buffer, 36}, td::Slice{packed, 48}, true)) {
    return false;
  }
  td::uint16 crc = td::crc16(td::Slice{buffer, 34});
  if (buffer[34] != (crc >> 8) || buffer[35] != (crc & 0xff) || (buffer[0] & 0x3f) != 0x11) {
    return false;
  }
  workchain = (td::int8)buffer[1];
  std::memcpy(addr.data(), buffer + 2, 32);
  return true;
}

// parse address like "0:527964d55cfa6eb731f4bfc07e9d025098097ef8505519e853986279bd8400d8"
// based on StdAddress::parse_addr() from block.cpp
// (which is not included to avoid linking with ton_crypto)
static bool parse_raw_address(const std::string& acc_string, int& workchain, ton::StdSmcAddress& addr) {
  size_t pos = acc_string.find(':');
  if (pos != std::string::npos) {
    td::Result<int> r_wc = td::to_integer_safe<ton::WorkchainId>(acc_string.substr(0, pos));
    if (r_wc.is_error()) {
      return false;
    }
    workchain = r_wc.move_as_ok();
    pos++;
  } else {
    pos = 0;
  }
  if (acc_string.size() != pos + 64) {
    return false;
  }

  for (int i = 0; i < 64; ++i) {    // loop through each hex digit
    char c = acc_string[pos + i];
    int x;
    if (c >= '0' && c <= '9') {
      x = c - '0';
    } else if (c >= 'a' && c <= 'z') {
      x = c - 'a' + 10;
    } else if (c >= 'A' && c <= 'Z') {
      x = c - 'A' + 10;
    } else {
      return false;
    }

    if ((i & 1) == 0) {
      addr.data()[i >> 1] = static_cast<unsigned char>((addr.data()[i >> 1] & 0x0F) | (x << 4));
    } else {
      addr.data()[i >> 1] = static_cast<unsigned char>((addr.data()[i >> 1] & 0xF0) | x);
    }
  }
  return true;
}

static Expr* create_expr_apply(SrcLocation loc, SymDef* sym, std::vector<Expr*>&& args) {
  Expr* apply = new Expr(Expr::_Apply, sym, std::move(args));
  apply->here = loc;
  apply->flags = Expr::_IsRvalue;
  apply->deduce_type();
  return apply;
}

static Expr* create_expr_int_const(SrcLocation loc, int int_val) {
  Expr* int_const = new Expr(Expr::_Const, loc);
  int_const->intval = td::make_refint(int_val);
  int_const->flags = Expr::_IsRvalue;
  int_const->e_type = TypeExpr::new_atomic(TypeExpr::_Int);
  return int_const;
}

namespace blk_fl {
enum { end = 1, ret = 2, empty = 4 };
typedef int val;
constexpr val init = end | empty;
void combine(val& x, const val y) {
  x |= y & ret;
  x &= y | ~(end | empty);
}
void combine_parallel(val& x, const val y) {
  x &= y | ~(ret | empty);
  x |= y & end;
}
} // namespace blk_fl

Expr* process_expr(AnyV v, CodeBlob& code);
blk_fl::val process_statement(AnyV v, CodeBlob& code);

static void check_global_func(SrcLocation loc, sym_idx_t func_name) {
  SymDef* sym_def = lookup_symbol(func_name);
  if (!sym_def) {
    throw ParseError(loc, "undefined symbol `" + G.symbols.get_name(func_name) + "`");
  }
}

static void check_import_exists_when_using_sym(AnyV v_usage, const SymDef* used_sym) {
  if (!v_usage->loc.is_symbol_from_same_or_builtin_file(used_sym->loc)) {
    const SrcFile* declared_in = used_sym->loc.get_src_file();
    bool has_import = false;
    for (const SrcFile::ImportStatement& import_stmt : v_usage->loc.get_src_file()->imports) {
      if (import_stmt.imported_file == declared_in) {
        has_import = true;
      }
    }
    if (!has_import) {
      v_usage->error("Using a non-imported symbol `" + used_sym->name() + "`. Forgot to import \"" + declared_in->rel_filename + "\"?");
    }
  }
}

static Expr* create_new_local_variable(SrcLocation loc, std::string_view var_name, TypeExpr* var_type, bool is_immutable) {
  SymDef* sym = lookup_symbol(calc_sym_idx(var_name));
  if (sym) { // creating a new variable, but something found in symtable
    if (sym->level != G.scope_level) {
      sym = nullptr;  // declaring a new variable with the same name, but in another scope
    } else {
      throw ParseError(loc, "redeclaration of local variable `" + static_cast<std::string>(var_name) + "`");
    }
  }
  Expr* x = new Expr{Expr::_Var, loc};
  x->val = ~calc_sym_idx(var_name);
  x->e_type = var_type;
  x->flags = Expr::_IsLvalue | (is_immutable ? Expr::_IsImmutable : 0);
  return x;
}

static Expr* create_new_underscore_variable(SrcLocation loc, TypeExpr* var_type) {
  Expr* x = new Expr{Expr::_Hole, loc};
  x->val = -1;
  x->flags = Expr::_IsLvalue;
  x->e_type = var_type;
  return x;
}

static Expr* process_expr(V<ast_binary_operator> v, CodeBlob& code) {
  TokenType t = v->tok;
  std::string operator_name = static_cast<std::string>(v->operator_name);

  if (t == tok_set_plus || t == tok_set_minus || t == tok_set_mul || t == tok_set_div ||
      t == tok_set_mod || t == tok_set_lshift || t == tok_set_rshift ||
      t == tok_set_bitwise_and || t == tok_set_bitwise_or || t == tok_set_bitwise_xor) {
    Expr* x = process_expr(v->get_lhs(), code);
    x->chk_rvalue();
    if (!x->is_lvalue()) {
      x->fire_error_lvalue_expected("left side of assignment");
    }
    if (x->is_immutable()) {
      x->fire_error_modifying_immutable("left side of assignment");
    }
    SymDef* sym = lookup_symbol(calc_sym_idx("^_" + operator_name + "_"));
    Expr* y = process_expr(v->get_rhs(), code);
    y->chk_rvalue();
    Expr* z = create_expr_apply(v->loc, sym, {x, y});
    Expr* res = new Expr{Expr::_Letop, {x->copy(), z}};
    res->here = v->loc;
    res->flags = x->flags | Expr::_IsRvalue;
    res->deduce_type();
    return res;
  }
  if (t == tok_assign) {
    Expr* x = process_expr(v->get_lhs(), code);
    if (!x->is_lvalue()) {
      x->fire_error_lvalue_expected("left side of assignment");
    }
    if (x->is_immutable()) {
      x->fire_error_modifying_immutable("left side of assignment");
    }
    Expr* y = process_expr(v->get_rhs(), code);
    y->chk_rvalue();
    x->predefine_vars();
    x->define_new_vars(code);
    Expr* res = new Expr{Expr::_Letop, {x, y}};
    res->here = v->loc;
    res->flags = x->flags | Expr::_IsRvalue;
    res->deduce_type();
    return res;
  }
  if (t == tok_minus || t == tok_plus ||
      t == tok_bitwise_and || t == tok_bitwise_or || t == tok_bitwise_xor ||
      t == tok_eq || t == tok_lt || t == tok_gt || t == tok_leq || t == tok_geq || t == tok_neq || t == tok_spaceship ||
      t == tok_lshift || t == tok_rshift || t == tok_rshiftC || t == tok_rshiftR ||
      t == tok_mul || t == tok_div || t == tok_mod || t == tok_divC || t == tok_divR) {
    Expr* res = process_expr(v->get_lhs(), code);
    res->chk_rvalue();
    SymDef* sym = lookup_symbol(calc_sym_idx("_" + operator_name + "_"));
    Expr* x = process_expr(v->get_rhs(), code);
    x->chk_rvalue();
    res = create_expr_apply(v->loc, sym, {res, x});
    return res;
  }
  if (t == tok_logical_and || t == tok_logical_or) {
    // do the following transformations:
    // a && b  ->  a ? (b != 0) : 0
    // a || b  ->  a ? 1 : (b != 0)
    SymDef* sym_neq = lookup_symbol(calc_sym_idx("_!=_"));
    Expr* lhs = process_expr(v->get_lhs(), code);
    Expr* rhs = process_expr(v->get_rhs(), code);
    Expr* e_neq0 = create_expr_apply(v->loc, sym_neq, {rhs, create_expr_int_const(v->loc, 0)});
    Expr* e_when_true = t == tok_logical_and ? e_neq0 : create_expr_int_const(v->loc, -1);
    Expr* e_when_false = t == tok_logical_and ? create_expr_int_const(v->loc, 0) : e_neq0;
    Expr* e_ternary = new Expr(Expr::_CondExpr, {lhs, e_when_true, e_when_false});
    e_ternary->here = v->loc;
    e_ternary->flags = Expr::_IsRvalue;
    e_ternary->deduce_type();
    return e_ternary;
  }

  v->error("unsupported binary operator");
}

static Expr* process_expr(V<ast_unary_operator> v, CodeBlob& code) {
  TokenType t = v->tok;
  SymDef* sym = lookup_symbol(calc_sym_idx(static_cast<std::string>(v->operator_name) + "_"));
  Expr* x = process_expr(v->get_rhs(), code);
  x->chk_rvalue();

  // here's an optimization to convert "-1" (tok_minus tok_int_const) to a const -1, not to Expr::Apply(-,1)
  // without this, everything still works, but Tolk looses some vars/stack knowledge for now (to be fixed later)
  // in FunC, it was:
  // `var fst = -1;`   // is constantly 1
  // `var snd = - 1;`  // is Expr::Apply(-), a comment "snd=1" is lost in stack layout comments, and so on
  // hence, when after grammar modification tok_minus became a true unary operator (not a part of a number),
  // and thus to preserve existing behavior until compiler parts are completely rewritten, handle this case here
  if (t == tok_minus && x->cls == Expr::_Const) {
    x->intval = -x->intval;
    if (!x->intval->signed_fits_bits(257)) {
      v->error("integer overflow");
    }
    return x;
  }
  if (t == tok_plus && x->cls == Expr::_Const) {
    return x;
  }

  return create_expr_apply(v->loc, sym, {x});
}

static Expr* process_expr(V<ast_ternary_operator> v, CodeBlob& code) {
  Expr* cond = process_expr(v->get_cond(), code);
  cond->chk_rvalue();
  Expr* x = process_expr(v->get_when_true(), code);
  x->chk_rvalue();
  Expr* y = process_expr(v->get_when_false(), code);
  y->chk_rvalue();
  Expr* res = new Expr{Expr::_CondExpr, {cond, x, y}};
  res->here = v->loc;
  res->flags = Expr::_IsRvalue;
  res->deduce_type();
  return res;
}

static Expr* process_function_arguments(SymDef* func_sym, V<ast_argument_list> v, Expr* lhs_of_dot_call, CodeBlob& code) {
  SymValFunc* func_val = dynamic_cast<SymValFunc*>(func_sym->value);
  int delta_self = lhs_of_dot_call ? 1 : 0;
  int n_arguments = static_cast<int>(v->get_arguments().size()) + delta_self;
  int n_parameters = static_cast<int>(func_val->parameters.size());

  // Tolk doesn't have optional parameters currently, so just compare counts
  if (n_parameters < n_arguments) {
    v->error("too many arguments in call to `" + func_sym->name() + "`, expected " + std::to_string(n_parameters - delta_self) + ", have " + std::to_string(n_arguments - delta_self));
  }
  if (n_arguments < n_parameters) {
    v->error("too few arguments in call to `" + func_sym->name() + "`, expected " + std::to_string(n_parameters - delta_self) + ", have " + std::to_string(n_arguments - delta_self));
  }

  std::vector<Expr*> apply_args;
  apply_args.reserve(n_arguments);
  if (lhs_of_dot_call) {
    apply_args.push_back(lhs_of_dot_call);
  }
  for (int i = delta_self; i < n_arguments; ++i) {
    auto v_arg = v->get_arg(i - delta_self);
    if (SymDef* param_sym = func_val->parameters[i]) {   // can be null (for underscore parameter)
      SymValVariable* param_val = dynamic_cast<SymValVariable*>(param_sym->value);
      if (param_val->is_mutate_parameter() != v_arg->passed_as_mutate) {
        fire_error_invalid_mutate_arg_passed(v_arg->loc, func_sym, param_sym, false, v_arg->passed_as_mutate, v_arg->get_expr());
      }
    }

    Expr* arg = process_expr(v_arg->get_expr(), code);
    arg->chk_rvalue();
    apply_args.push_back(arg);
  }

  Expr* apply = new Expr{Expr::_Apply, func_sym, std::move(apply_args)};
  apply->flags = Expr::_IsRvalue | (!func_val->is_marked_as_pure() * Expr::_IsImpure);
  apply->here = v->loc;
  apply->deduce_type();

  return apply;
}

static Expr* process_function_call(V<ast_function_call> v, CodeBlob& code) {
  // special error for "null()" which is a FunC syntax
  if (v->get_called_f()->type == ast_null_keyword) {
    v->error("null is not a function: use `null`, not `null()`");
  }

  // most likely it's a global function, but also may be `some_var(args)` or even `getF()(args)`
  Expr* lhs = process_expr(v->get_called_f(), code);
  if (lhs->cls != Expr::_GlobFunc) {
    Expr* tensor_arg = new Expr(Expr::_Tensor, v->loc);
    std::vector<TypeExpr*> type_list;
    type_list.reserve(v->get_num_args());
    for (int i = 0; i < v->get_num_args(); ++i) {
      auto v_arg = v->get_arg(i);
      if (v_arg->passed_as_mutate) {
        v_arg->error("`mutate` used for non-mutate argument");
      }
      Expr* arg = process_expr(v_arg->get_expr(), code);
      arg->chk_rvalue();
      tensor_arg->pb_arg(arg);
      type_list.push_back(arg->e_type);
    }
    tensor_arg->flags = Expr::_IsRvalue;
    tensor_arg->e_type = TypeExpr::new_tensor(std::move(type_list));

    Expr* var_apply = new Expr{Expr::_VarApply, {lhs, tensor_arg}};
    var_apply->here = v->loc;
    var_apply->flags = Expr::_IsRvalue;
    var_apply->deduce_type();
    return var_apply;
  }

  Expr* apply = process_function_arguments(lhs->sym, v->get_arg_list(), nullptr, code);

  if (dynamic_cast<SymValFunc*>(apply->sym->value)->has_mutate_params()) {
    const std::vector<Expr*>& args = apply->args;
    SymValFunc* func_val = dynamic_cast<SymValFunc*>(apply->sym->value);
    tolk_assert(func_val->parameters.size() == args.size());
    Expr* grabbed_vars = new Expr(Expr::_Tensor, v->loc);
    std::vector<TypeExpr*> type_list;
    for (int i = 0; i < static_cast<int>(args.size()); ++i) {
      SymDef* param_def = func_val->parameters[i];
      if (param_def && dynamic_cast<SymValVariable*>(param_def->value)->is_mutate_parameter()) {
        if (!args[i]->is_lvalue()) {
          args[i]->fire_error_lvalue_expected("call a mutating function");
        }
        if (args[i]->is_immutable()) {
          args[i]->fire_error_modifying_immutable("call a mutating function");
        }
        grabbed_vars->pb_arg(args[i]->copy());
        type_list.emplace_back(args[i]->e_type);
      }
    }
    grabbed_vars->flags = Expr::_IsRvalue;
    Expr* grab_mutate = new Expr(Expr::_GrabMutatedVars, apply->sym, {apply, grabbed_vars});
    grab_mutate->here = v->loc;
    grab_mutate->flags = apply->flags;
    grab_mutate->deduce_type();
    return grab_mutate;
  }

  return apply;
}

static Expr* process_dot_method_call(V<ast_dot_method_call> v, CodeBlob& code) {
  sym_idx_t name_idx = calc_sym_idx(v->method_name);
  check_global_func(v->loc, name_idx);
  SymDef* func_sym = lookup_symbol(name_idx);
  SymValFunc* func_val = dynamic_cast<SymValFunc*>(func_sym->value);
  tolk_assert(func_val != nullptr);

  Expr* obj = process_expr(v->get_obj(), code);
  obj->chk_rvalue();

  if (func_val->parameters.empty()) {
    v->error("`" + func_sym->name() + "` has no parameters and can not be called as method");
  }
  if (!func_val->does_accept_self() && func_val->parameters[0] && dynamic_cast<SymValVariable*>(func_val->parameters[0]->value)->is_mutate_parameter()) {
    fire_error_invalid_mutate_arg_passed(v->loc, func_sym, func_val->parameters[0], true, false, v->get_obj());
  }

  Expr* apply = process_function_arguments(func_sym, v->get_arg_list(), obj, code);

  Expr* obj_lval = apply->args[0];
  if (!obj_lval->is_lvalue()) {
    if (obj_lval->cls == Expr::_ReturnSelf) {
      obj_lval = obj_lval->args[1];
    } else {
      Expr* tmp_var = create_new_underscore_variable(v->loc, obj_lval->e_type);
      tmp_var->define_new_vars(code);
      Expr* assign_to_tmp_var = new Expr(Expr::_Letop, {tmp_var, obj_lval});
      assign_to_tmp_var->here = v->loc;
      assign_to_tmp_var->flags = Expr::_IsRvalue;
      assign_to_tmp_var->deduce_type();
      apply->args[0] = assign_to_tmp_var;
      obj_lval = tmp_var;
    }
  }

  if (func_val->has_mutate_params()) {
    tolk_assert(func_val->parameters.size() == apply->args.size());
    Expr* grabbed_vars = new Expr(Expr::_Tensor, v->loc);
    std::vector<TypeExpr*> type_list;
    for (int i = 0; i < static_cast<int>(apply->args.size()); ++i) {
      SymDef* param_sym = func_val->parameters[i];
      if (param_sym && dynamic_cast<SymValVariable*>(param_sym->value)->is_mutate_parameter()) {
        Expr* ith_arg = apply->args[i];
        if (ith_arg->is_immutable()) {
          ith_arg->fire_error_modifying_immutable("call a mutating method");
        }

        Expr* var_to_mutate = nullptr;
        if (ith_arg->is_lvalue()) {
          var_to_mutate = ith_arg->copy();
        } else if (i == 0) {
          var_to_mutate = obj_lval;
        } else {
          ith_arg->fire_error_lvalue_expected("call a mutating method");
        }
        tolk_assert(var_to_mutate->is_lvalue() && !var_to_mutate->is_immutable());
        grabbed_vars->pb_arg(var_to_mutate);
        type_list.emplace_back(var_to_mutate->e_type);
      }
    }
    grabbed_vars->flags = Expr::_IsRvalue;

    Expr* grab_mutate = new Expr(Expr::_GrabMutatedVars, func_sym, {apply, grabbed_vars});
    grab_mutate->here = v->loc;
    grab_mutate->flags = apply->flags;
    grab_mutate->deduce_type();

    apply = grab_mutate;
  }

  if (func_val->does_return_self()) {
    Expr* self_arg = obj_lval;
    tolk_assert(self_arg->is_lvalue());

    Expr* return_self = new Expr(Expr::_ReturnSelf, func_sym, {apply, self_arg});
    return_self->here = v->loc;
    return_self->flags = Expr::_IsRvalue;
    return_self->deduce_type();

    apply = return_self;
  }

  return apply;
}

static Expr* process_expr(V<ast_tensor> v, CodeBlob& code) {
  if (v->empty()) {
    Expr* res = new Expr{Expr::_Tensor, {}};
    res->flags = Expr::_IsRvalue;
    res->here = v->loc;
    res->e_type = TypeExpr::new_unit();
    return res;
  }

  Expr* res = process_expr(v->get_item(0), code);
  std::vector<TypeExpr*> type_list;
  type_list.push_back(res->e_type);
  int f = res->flags;
  res = new Expr{Expr::_Tensor, {res}};
  for (int i = 1; i < v->size(); ++i) {
    Expr* x = process_expr(v->get_item(i), code);
    res->pb_arg(x);
    f &= (x->flags | Expr::_IsImmutable);
    f |= (x->flags & Expr::_IsImmutable);
    type_list.push_back(x->e_type);
  }
  res->here = v->loc;
  res->flags = f;
  res->e_type = TypeExpr::new_tensor(std::move(type_list));
  return res;
}

static Expr* process_expr(V<ast_tensor_square> v, CodeBlob& code) {
  if (v->empty()) {
    Expr* res = new Expr{Expr::_Tensor, {}};
    res->flags = Expr::_IsRvalue;
    res->here = v->loc;
    res->e_type = TypeExpr::new_unit();
    res = new Expr{Expr::_MkTuple, {res}};
    res->flags = Expr::_IsRvalue;
    res->here = v->loc;
    res->e_type = TypeExpr::new_tuple(res->args.at(0)->e_type);
    return res;
  }

  Expr* res = process_expr(v->get_item(0), code);
  std::vector<TypeExpr*> type_list;
  type_list.push_back(res->e_type);
  int f = res->flags;
  res = new Expr{Expr::_Tensor, {res}};
  for (int i = 1; i < v->size(); ++i) {
    Expr* x = process_expr(v->get_item(i), code);
    res->pb_arg(x);
    f &= (x->flags | Expr::_IsImmutable);
    f |= (x->flags & Expr::_IsImmutable);
    type_list.push_back(x->e_type);
  }
  res->here = v->loc;
  res->flags = f;
  res->e_type = TypeExpr::new_tensor(std::move(type_list), false);
  res = new Expr{Expr::_MkTuple, {res}};
  res->flags = f;
  res->here = v->loc;
  res->e_type = TypeExpr::new_tuple(res->args.at(0)->e_type);
  return res;
}

static Expr* process_expr(V<ast_int_const> v) {
  Expr* res = new Expr{Expr::_Const, v->loc};
  res->flags = Expr::_IsRvalue;
  res->intval = td::string_to_int256(static_cast<std::string>(v->int_val));
  if (res->intval.is_null() || !res->intval->signed_fits_bits(257)) {
    v->error("invalid integer constant");
  }
  res->e_type = TypeExpr::new_atomic(TypeExpr::_Int);
  return res;
}

static Expr* process_expr(V<ast_string_const> v) {
  std::string str = static_cast<std::string>(v->str_val);
  Expr* res;
  switch (v->modifier) {
    case 0:
    case 's':
    case 'a':
      res = new Expr{Expr::_SliceConst, v->loc};
      res->e_type = TypeExpr::new_atomic(TypeExpr::_Slice);
      break;
    case 'u':
    case 'h':
    case 'H':
    case 'c':
      res = new Expr{Expr::_Const, v->loc};
      res->e_type = TypeExpr::new_atomic(TypeExpr::_Int);
      break;
    default:
      v->error("invalid string modifier '" + std::string(1, v->modifier) + "'");
  }
  res->flags = Expr::_IsRvalue;
  switch (v->modifier) {
    case 0: {
      res->strval = td::hex_encode(str);
      break;
    }
    case 's': {
      res->strval = str;
      unsigned char buff[128];
      int bits = (int)td::bitstring::parse_bitstring_hex_literal(buff, sizeof(buff), str.data(), str.data() + str.size());
      if (bits < 0) {
        v->error("invalid hex bitstring constant '" + str + "'");
      }
      break;
    }
    case 'a': {  // MsgAddress
      int workchain;
      ton::StdSmcAddress addr;
      bool correct = (str.size() == 48 && parse_friendly_address(str.data(), workchain, addr)) ||
                     (str.size() != 48 && parse_raw_address(str, workchain, addr));
      if (!correct) {
        v->error("invalid standard address '" + str + "'");
      }
      if (workchain < -128 || workchain >= 128) {
        v->error("anycast addresses not supported");
      }

      unsigned char data[3 + 8 + 256];  // addr_std$10 anycast:(Maybe Anycast) workchain_id:int8 address:bits256 = MsgAddressInt;
      td::bitstring::bits_store_long_top(data, 0, static_cast<uint64_t>(4) << (64 - 3), 3);
      td::bitstring::bits_store_long_top(data, 3, static_cast<uint64_t>(workchain) << (64 - 8), 8);
      td::bitstring::bits_memcpy(data, 3 + 8, addr.bits().ptr, 0, addr.size());
      res->strval = td::BitSlice{data, sizeof(data)}.to_hex();
      break;
    }
    case 'u': {
      res->intval = td::hex_string_to_int256(td::hex_encode(str));
      if (str.empty()) {
        v->error("empty integer ascii-constant");
      }
      if (res->intval.is_null()) {
        v->error("too long integer ascii-constant");
      }
      break;
    }
    case 'h':
    case 'H': {
      unsigned char hash[32];
      digest::hash_str<digest::SHA256>(hash, str.data(), str.size());
      res->intval = td::bits_to_refint(hash, (v->modifier == 'h') ? 32 : 256, false);
      break;
    }
    case 'c': {
      res->intval = td::make_refint(td::crc32(td::Slice{str}));
      break;
    }
    default:
      tolk_assert(false);
  }
  return res;
}

static Expr* process_expr(V<ast_bool_const> v) {
  SymDef* builtin_sym = lookup_symbol(calc_sym_idx(v->bool_val ? "__true" : "__false"));
  return create_expr_apply(v->loc, builtin_sym, {});
}

static Expr* process_expr(V<ast_null_keyword> v) {
  SymDef* builtin_sym = lookup_symbol(calc_sym_idx("__null"));
  return create_expr_apply(v->loc, builtin_sym, {});
}

static Expr* process_expr(V<ast_self_keyword> v, CodeBlob& code) {
  if (!code.func_val->does_accept_self()) {
    v->error("using `self` in a non-member function (it does not accept the first `self` parameter)");
  }
  SymDef* sym = lookup_symbol(calc_sym_idx("self"));
  tolk_assert(sym);
  SymValVariable* sym_val = dynamic_cast<SymValVariable*>(sym->value);
  Expr* res = new Expr(Expr::_Var, v->loc);
  res->sym = sym;
  res->val = sym_val->idx;
  res->flags = Expr::_IsLvalue | Expr::_IsRvalue | (sym_val->is_immutable() ? Expr::_IsImmutable : 0);
  res->e_type = sym_val->get_type();
  return res;
}

static Expr* process_identifier(V<ast_identifier> v) {
  SymDef* sym = lookup_symbol(calc_sym_idx(v->name));
  if (sym && dynamic_cast<SymValGlobVar*>(sym->value)) {
    check_import_exists_when_using_sym(v, sym);
    Expr* res = new Expr{Expr::_GlobVar, v->loc};
    res->e_type = sym->value->get_type();
    res->sym = sym;
    res->flags = Expr::_IsLvalue | Expr::_IsRvalue | Expr::_IsImpure;
    return res;
  }
  if (sym && dynamic_cast<SymValConst*>(sym->value)) {
    check_import_exists_when_using_sym(v, sym);
    auto val = dynamic_cast<SymValConst*>(sym->value);
    Expr* res = nullptr;
    if (val->get_kind() == SymValConst::IntConst) {
      res = new Expr{Expr::_Const, v->loc};
      res->intval = val->get_int_value();
      res->e_type = TypeExpr::new_atomic(TypeExpr::_Int);
    } else if (val->get_kind() == SymValConst::SliceConst) {
      res = new Expr{Expr::_SliceConst, v->loc};
      res->strval = val->get_str_value();
      res->e_type = TypeExpr::new_atomic(TypeExpr::_Slice);
    } else {
      v->error("invalid symbolic constant type");
    }
    res->flags = Expr::_IsLvalue | Expr::_IsRvalue | Expr::_IsImmutable;
    res->sym = sym;
    return res;
  }
  if (sym && dynamic_cast<SymValFunc*>(sym->value)) {
    check_import_exists_when_using_sym(v, sym);
  }
  Expr* res = new Expr{Expr::_Var, v->loc};
  if (!sym) {
    check_global_func(v->loc, calc_sym_idx(v->name));
    sym = lookup_symbol(calc_sym_idx(v->name));
    tolk_assert(sym);
  }
  res->sym = sym;
  bool impure = false;
  bool immutable = false;
  if (const SymValFunc* func_val = dynamic_cast<SymValFunc*>(sym->value)) {
    res->e_type = func_val->get_type();
    res->cls = Expr::_GlobFunc;
    impure = !func_val->is_marked_as_pure();
  } else if (const SymValVariable* var_val = dynamic_cast<SymValVariable*>(sym->value)) {
    tolk_assert(var_val->idx >= 0)
    res->val = var_val->idx;
    res->e_type = var_val->get_type();
    immutable = var_val->is_immutable();
    // std::cerr << "accessing variable " << lex.cur().str << " : " << res->e_type << std::endl;
  } else {
    v->error("undefined identifier '" + static_cast<std::string>(v->name) + "'");
  }
  // std::cerr << "accessing symbol " << lex.cur().str << " : " << res->e_type << (val->impure ? " (impure)" : " (pure)") << std::endl;
  res->flags = Expr::_IsLvalue | Expr::_IsRvalue | (impure ? Expr::_IsImpure : 0) | (immutable ? Expr::_IsImmutable : 0);
  res->deduce_type();
  return res;
}

Expr* process_expr(AnyV v, CodeBlob& code) {
  switch (v->type) {
    case ast_binary_operator:
      return process_expr(v->as<ast_binary_operator>(), code);
    case ast_unary_operator:
      return process_expr(v->as<ast_unary_operator>(), code);
    case ast_ternary_operator:
      return process_expr(v->as<ast_ternary_operator>(), code);
    case ast_function_call:
      return process_function_call(v->as<ast_function_call>(), code);
    case ast_dot_method_call:
      return process_dot_method_call(v->as<ast_dot_method_call>(), code);
    case ast_parenthesized_expr:
      return process_expr(v->as<ast_parenthesized_expr>()->get_expr(), code);
    case ast_tensor:
      return process_expr(v->as<ast_tensor>(), code);
    case ast_tensor_square:
      return process_expr(v->as<ast_tensor_square>(), code);
    case ast_int_const:
      return process_expr(v->as<ast_int_const>());
    case ast_string_const:
      return process_expr(v->as<ast_string_const>());
    case ast_bool_const:
      return process_expr(v->as<ast_bool_const>());
    case ast_null_keyword:
      return process_expr(v->as<ast_null_keyword>());
    case ast_self_keyword:
      return process_expr(v->as<ast_self_keyword>(), code);
    case ast_identifier:
      return process_identifier(v->as<ast_identifier>());
    case ast_underscore:
      return create_new_underscore_variable(v->loc, TypeExpr::new_hole());
    default:
      throw UnexpectedASTNodeType(v, "process_expr");
  }
}

static Expr* process_local_vars_lhs(AnyV v, CodeBlob& code) {
  switch (v->type) {
    case ast_local_var: {
      auto v_var = v->as<ast_local_var>();
      if (v_var->marked_as_redef) {
        Expr* redef_var = process_identifier(v_var->get_identifier()->as<ast_identifier>());
        if (redef_var->is_immutable()) {
          redef_var->fire_error_modifying_immutable("left side of assignment");
        }
        return redef_var;
      }
      TypeExpr* var_type = v_var->declared_type ? v_var->declared_type : TypeExpr::new_hole();
      if (auto v_ident = v->as<ast_local_var>()->get_identifier()->try_as<ast_identifier>()) {
        return create_new_local_variable(v->loc, v_ident->name, var_type, v_var->is_immutable);
      } else {
        return create_new_underscore_variable(v->loc, var_type);
      }
    }
    case ast_parenthesized_expr:
      return process_local_vars_lhs(v->as<ast_parenthesized_expr>()->get_expr(), code);
    case ast_tensor: {
      std::vector<TypeExpr*> type_list;
      Expr* res = new Expr{Expr::_Tensor, v->loc};
      for (AnyV item : v->as<ast_tensor>()->get_items()) {
        Expr* x = process_local_vars_lhs(item, code);
        res->pb_arg(x);
        res->flags |= x->flags;
        type_list.push_back(x->e_type);
      }
      res->e_type = TypeExpr::new_tensor(std::move(type_list));
      return res;
    }
    case ast_tensor_square: {
      std::vector<TypeExpr*> type_list;
      Expr* res = new Expr{Expr::_Tensor, v->loc};
      for (AnyV item : v->as<ast_tensor_square>()->get_items()) {
        Expr* x = process_local_vars_lhs(item, code);
        res->pb_arg(x);
        res->flags |= x->flags;
        type_list.push_back(x->e_type);
      }
      res->e_type = TypeExpr::new_tensor(std::move(type_list));
      res = new Expr{Expr::_MkTuple, {res}};
      res->flags = res->args.at(0)->flags;
      res->here = v->loc;
      res->e_type = TypeExpr::new_tuple(res->args.at(0)->e_type);
      return res;
    }
    default:
      throw UnexpectedASTNodeType(v, "process_local_vars_lhs");
  }
}

static blk_fl::val process_vertex(V<ast_local_vars_declaration> v, CodeBlob& code) {
  Expr* x = process_local_vars_lhs(v->get_lhs(), code);
  Expr* y = process_expr(v->get_assigned_val(), code);
  y->chk_rvalue();
  x->predefine_vars();
  x->define_new_vars(code);
  Expr* res = new Expr{Expr::_Letop, {x, y}};
  res->here = v->loc;
  res->flags = x->flags | Expr::_IsRvalue;
  res->deduce_type();
  res->chk_rvalue();
  res->pre_compile(code);
  return blk_fl::end;
}

static bool is_expr_valid_as_return_self(Expr* return_expr) {
  // `return self`
  if (return_expr->cls == Expr::_Var && return_expr->val == 0) {
    return true;
  }
  if (return_expr->cls == Expr::_ReturnSelf) {
    return is_expr_valid_as_return_self(return_expr->args[1]);
  }
  if (return_expr->cls == Expr::_CondExpr) {
    return is_expr_valid_as_return_self(return_expr->args[1]) && is_expr_valid_as_return_self(return_expr->args[2]);
  }
  return false;
}

// for mutating functions, having `return expr`, transform it to `return (modify_var1, ..., expr)`
static Expr* wrap_return_value_with_mutate_params(SrcLocation loc, CodeBlob& code, Expr* return_expr) {
  Expr* tmp_var;
  if (return_expr->cls != Expr::_Var) {
    // `return complex_expr` - extract this into temporary variable (eval it before return)
    // this is mandatory if it assigns to one of modified vars
    tmp_var = create_new_underscore_variable(loc, return_expr->e_type);
    tmp_var->predefine_vars();
    tmp_var->define_new_vars(code);
    Expr* assign_to_tmp_var = new Expr(Expr::_Letop, {tmp_var, return_expr});
    assign_to_tmp_var->here = loc;
    assign_to_tmp_var->flags = tmp_var->flags | Expr::_IsRvalue;
    assign_to_tmp_var->deduce_type();
    assign_to_tmp_var->pre_compile(code);
  } else {
    tmp_var = return_expr;
  }

  Expr* ret_tensor = new Expr(Expr::_Tensor, loc);
  std::vector<TypeExpr*> type_list;
  for (SymDef* p_sym: code.func_val->parameters) {
    if (p_sym && dynamic_cast<SymValVariable*>(p_sym->value)->is_mutate_parameter()) {
      Expr* p_expr = new Expr{Expr::_Var, p_sym->loc};
      p_expr->sym = p_sym;
      p_expr->val = p_sym->value->idx;
      p_expr->flags = Expr::_IsRvalue;
      p_expr->e_type = p_sym->value->get_type();
      ret_tensor->pb_arg(p_expr);
      type_list.emplace_back(p_expr->e_type);
    }
  }
  ret_tensor->pb_arg(tmp_var);
  type_list.emplace_back(tmp_var->e_type);
  ret_tensor->flags = Expr::_IsRvalue;
  ret_tensor->e_type = TypeExpr::new_tensor(std::move(type_list));
  return ret_tensor;
}

static blk_fl::val process_vertex(V<ast_return_statement> v, CodeBlob& code) {
  Expr* expr = process_expr(v->get_return_value(), code);
  if (code.func_val->does_return_self()) {
    if (!is_expr_valid_as_return_self(expr)) {
      v->error("invalid return from `self` function");
    }
    Expr* var_self = new Expr(Expr::_Var, v->loc);
    var_self->flags = Expr::_IsRvalue | Expr::_IsLvalue;
    var_self->e_type = code.func_val->parameters[0]->value->get_type();
    Expr* assign_to_self = new Expr(Expr::_Letop, {var_self, expr});
    assign_to_self->here = v->loc;
    assign_to_self->flags = Expr::_IsRvalue;
    assign_to_self->deduce_type();
    assign_to_self->pre_compile(code);
    Expr* empty_tensor = new Expr(Expr::_Tensor, {});
    empty_tensor->here = v->loc;
    empty_tensor->flags = Expr::_IsRvalue;
    empty_tensor->e_type = TypeExpr::new_tensor({});
    expr = empty_tensor;
  }
  if (code.func_val->has_mutate_params()) {
    expr = wrap_return_value_with_mutate_params(v->loc, code, expr);
  }
  expr->chk_rvalue();
  try {
    unify(expr->e_type, code.ret_type);
  } catch (UnifyError& ue) {
    std::ostringstream os;
    os << "previous function return type " << code.ret_type
       << " cannot be unified with return statement expression type " << expr->e_type << ": " << ue;
    v->error(os.str());
  }
  std::vector<var_idx_t> tmp_vars = expr->pre_compile(code);
  code.emplace_back(v->loc, Op::_Return, std::move(tmp_vars));
  return blk_fl::ret;
}

static void append_implicit_ret_stmt(SrcLocation loc_end, CodeBlob& code) {
  Expr* expr = new Expr{Expr::_Tensor, {}};
  expr->flags = Expr::_IsRvalue;
  expr->here = loc_end;
  expr->e_type = TypeExpr::new_unit();
  if (code.func_val->does_return_self()) {
    throw ParseError(loc_end, "missing return; forgot `return self`?");
  }
  if (code.func_val->has_mutate_params()) {
    expr = wrap_return_value_with_mutate_params(loc_end, code, expr);
  }
  try {
    unify(expr->e_type, code.ret_type);
  } catch (UnifyError& ue) {
    std::ostringstream os;
    os << "previous function return type " << code.ret_type
       << " cannot be unified with implicit end-of-block return type " << expr->e_type << ": " << ue;
    throw ParseError(loc_end, os.str());
  }
  std::vector<var_idx_t> tmp_vars = expr->pre_compile(code);
  code.emplace_back(loc_end, Op::_Return, std::move(tmp_vars));
}

static blk_fl::val process_vertex(V<ast_sequence> v, CodeBlob& code, bool no_new_scope = false) {
  if (!no_new_scope) {
    open_scope(v->loc);
  }
  blk_fl::val res = blk_fl::init;
  bool warned = false;
  for (AnyV item : v->get_items()) {
    if (!(res & blk_fl::end) && !warned) {
      item->loc.show_warning("unreachable code");
      warned = true;
    }
    blk_fl::combine(res, process_statement(item, code));
  }
  if (!no_new_scope) {
    close_scope();
  }
  return res;
}

static blk_fl::val process_vertex(V<ast_repeat_statement> v, CodeBlob& code) {
  Expr* expr = process_expr(v->get_cond(), code);
  expr->chk_rvalue();
  auto cnt_type = TypeExpr::new_atomic(TypeExpr::_Int);
  try {
    unify(expr->e_type, cnt_type);
  } catch (UnifyError& ue) {
    std::ostringstream os;
    os << "repeat count value of type " << expr->e_type << " is not an integer: " << ue;
    v->get_cond()->error(os.str());
  }
  std::vector<var_idx_t> tmp_vars = expr->pre_compile(code);
  if (tmp_vars.size() != 1) {
    v->get_cond()->error("repeat count value is not a singleton");
  }
  Op& repeat_op = code.emplace_back(v->loc, Op::_Repeat, tmp_vars);
  code.push_set_cur(repeat_op.block0);
  blk_fl::val res = process_vertex(v->get_body(), code);
  code.close_pop_cur(v->get_body()->loc_end);
  return res | blk_fl::end;
}

static blk_fl::val process_vertex(V<ast_while_statement> v, CodeBlob& code) {
  Expr* expr = process_expr(v->get_cond(), code);
  expr->chk_rvalue();
  auto cnt_type = TypeExpr::new_atomic(TypeExpr::_Int);
  try {
    unify(expr->e_type, cnt_type);
  } catch (UnifyError& ue) {
    std::ostringstream os;
    os << "while condition value of type " << expr->e_type << " is not an integer: " << ue;
    v->get_cond()->error(os.str());
  }
  Op& while_op = code.emplace_back(v->loc, Op::_While);
  code.push_set_cur(while_op.block0);
  while_op.left = expr->pre_compile(code);
  code.close_pop_cur(v->get_body()->loc);
  if (while_op.left.size() != 1) {
    v->get_cond()->error("while condition value is not a singleton");
  }
  code.push_set_cur(while_op.block1);
  blk_fl::val res1 = process_vertex(v->get_body(), code);
  code.close_pop_cur(v->get_body()->loc_end);
  return res1 | blk_fl::end;
}

static blk_fl::val process_vertex(V<ast_do_while_statement> v, CodeBlob& code) {
  Op& until_op = code.emplace_back(v->loc, Op::_Until);
  code.push_set_cur(until_op.block0);
  open_scope(v->loc);
  blk_fl::val res = process_vertex(v->get_body(), code, true);

  // in TVM, there is only "do until", but in Tolk, we want "do while"
  // here we negate condition to pass it forward to legacy to Op::_Until
  // also, handle common situations as a hardcoded "optimization": replace (a<0) with (a>=0) and so on
  // todo these hardcoded conditions should be removed from this place in the future
  AnyV cond = v->get_cond();
  AnyV until_cond;
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

  Expr* expr = process_expr(until_cond, code);
  expr->chk_rvalue();
  close_scope();
  auto cnt_type = TypeExpr::new_atomic(TypeExpr::_Int);
  try {
    unify(expr->e_type, cnt_type);
  } catch (UnifyError& ue) {
    std::ostringstream os;
    os << "`while` condition value of type " << expr->e_type << " is not an integer: " << ue;
    v->get_cond()->error(os.str());
  }
  until_op.left = expr->pre_compile(code);
  code.close_pop_cur(v->get_body()->loc_end);
  if (until_op.left.size() != 1) {
    v->get_cond()->error("`while` condition value is not a singleton");
  }
  return res & ~blk_fl::empty;
}

static blk_fl::val process_vertex(V<ast_throw_statement> v, CodeBlob& code) {
  std::vector<Expr*> args;
  SymDef* builtin_sym;
  if (v->has_thrown_arg()) {
    builtin_sym = lookup_symbol(calc_sym_idx("__throw_arg"));
    args.push_back(process_expr(v->get_thrown_arg(), code));
    args.push_back(process_expr(v->get_thrown_code(), code));
  } else {
    builtin_sym = lookup_symbol(calc_sym_idx("__throw"));
    args.push_back(process_expr(v->get_thrown_code(), code));
  }

  Expr* apply = create_expr_apply(v->loc, builtin_sym, std::move(args));
  apply->flags |= Expr::_IsImpure;
  apply->pre_compile(code);
  return blk_fl::end;
}

static blk_fl::val process_vertex(V<ast_assert_statement> v, CodeBlob& code) {
  std::vector<Expr*> args(3);
  if (auto v_not = v->get_cond()->try_as<ast_unary_operator>(); v_not && v_not->tok == tok_logical_not) {
    args[0] = process_expr(v->get_thrown_code(), code);
    args[1] = process_expr(v->get_cond()->as<ast_unary_operator>()->get_rhs(), code);
    args[2] = process_expr(createV<ast_bool_const>(v->loc, true), code);
  } else {
    args[0] = process_expr(v->get_thrown_code(), code);
    args[1] = process_expr(v->get_cond(), code);
    args[2] = process_expr(createV<ast_bool_const>(v->loc, false), code);
  }

  SymDef* builtin_sym = lookup_symbol(calc_sym_idx("__throw_if_unless"));
  Expr* apply = create_expr_apply(v->loc, builtin_sym, std::move(args));
  apply->flags |= Expr::_IsImpure;
  apply->pre_compile(code);
  return blk_fl::end;
}

static Expr* process_catch_variable(AnyV catch_var, TypeExpr* var_type) {
  if (auto v_ident = catch_var->try_as<ast_identifier>()) {
    return create_new_local_variable(catch_var->loc, v_ident->name, var_type, true);
  }
  return create_new_underscore_variable(catch_var->loc, var_type);
}

static blk_fl::val process_vertex(V<ast_try_catch_statement> v, CodeBlob& code) {
  code.require_callxargs = true;
  Op& try_catch_op = code.emplace_back(v->loc, Op::_TryCatch);
  code.push_set_cur(try_catch_op.block0);
  blk_fl::val res0 = process_vertex(v->get_try_body(), code);
  code.close_pop_cur(v->get_try_body()->loc_end);
  code.push_set_cur(try_catch_op.block1);
  open_scope(v->get_catch_expr()->loc);

  // transform catch (excNo, arg) into TVM-catch (arg, excNo), where arg is untyped and thus almost useless now
  TypeExpr* tvm_error_type = TypeExpr::new_tensor(TypeExpr::new_var(), TypeExpr::new_atomic(TypeExpr::_Int));
  const std::vector<AnyV>& catch_items = v->get_catch_expr()->get_items();
  tolk_assert(catch_items.size() == 2);
  Expr* e_catch = new Expr{Expr::_Tensor, v->get_catch_expr()->loc};
  e_catch->pb_arg(process_catch_variable(catch_items[1], tvm_error_type->args[0]));
  e_catch->pb_arg(process_catch_variable(catch_items[0], tvm_error_type->args[1]));
  e_catch->flags = Expr::_IsLvalue;
  e_catch->e_type = tvm_error_type;
  e_catch->predefine_vars();
  e_catch->define_new_vars(code);
  try_catch_op.left = e_catch->pre_compile(code);
  tolk_assert(try_catch_op.left.size() == 2);

  blk_fl::val res1 = process_vertex(v->get_catch_body(), code);
  close_scope();
  code.close_pop_cur(v->get_catch_body()->loc_end);
  blk_fl::combine_parallel(res0, res1);
  return res0;
}

static blk_fl::val process_vertex(V<ast_if_statement> v, CodeBlob& code) {
  Expr* expr = process_expr(v->get_cond(), code);
  expr->chk_rvalue();
  TypeExpr* flag_type = TypeExpr::new_atomic(TypeExpr::_Int);
  try {
    unify(expr->e_type, flag_type);
  } catch (UnifyError& ue) {
    std::ostringstream os;
    os << "`if` condition value of type " << expr->e_type << " is not an integer: " << ue;
    v->get_cond()->error(os.str());
  }
  std::vector<var_idx_t> tmp_vars = expr->pre_compile(code);
  if (tmp_vars.size() != 1) {
    v->get_cond()->error("condition value is not a singleton");
  }
  Op& if_op = code.emplace_back(v->loc, Op::_If, tmp_vars);
  code.push_set_cur(if_op.block0);
  blk_fl::val res1 = process_vertex(v->get_if_body(), code);
  blk_fl::val res2 = blk_fl::init;
  code.close_pop_cur(v->get_if_body()->loc_end);
  code.push_set_cur(if_op.block1);
  res2 = process_vertex(v->get_else_body(), code);
  code.close_pop_cur(v->get_else_body()->loc_end);
  if (v->is_ifnot) {
    std::swap(if_op.block0, if_op.block1);
  }
  blk_fl::combine_parallel(res1, res2);
  return res1;
}

blk_fl::val process_statement(AnyV v, CodeBlob& code) {
  switch (v->type) {
    case ast_local_vars_declaration:
      return process_vertex(v->as<ast_local_vars_declaration>(), code);
    case ast_return_statement:
      return process_vertex(v->as<ast_return_statement>(), code);
    case ast_sequence:
      return process_vertex(v->as<ast_sequence>(), code);
    case ast_empty:
      return blk_fl::init;
    case ast_repeat_statement:
      return process_vertex(v->as<ast_repeat_statement>(), code);
    case ast_if_statement:
      return process_vertex(v->as<ast_if_statement>(), code);
    case ast_do_while_statement:
      return process_vertex(v->as<ast_do_while_statement>(), code);
    case ast_while_statement:
      return process_vertex(v->as<ast_while_statement>(), code);
    case ast_throw_statement:
      return process_vertex(v->as<ast_throw_statement>(), code);
    case ast_assert_statement:
      return process_vertex(v->as<ast_assert_statement>(), code);
    case ast_try_catch_statement:
      return process_vertex(v->as<ast_try_catch_statement>(), code);
    default: {
      Expr* expr = process_expr(v, code);
      expr->chk_rvalue();
      expr->pre_compile(code);
      return blk_fl::end;
    }
  }
}

static FormalArg process_vertex(V<ast_parameter> v, SymDef* param_sym) {
  if (!param_sym) {
    return std::make_tuple(v->param_type, nullptr, v->loc);
  }
  SymDef* new_sym_def = define_symbol(calc_sym_idx(v->get_identifier()->name), true, v->loc);
  if (!new_sym_def || new_sym_def->value) {
    v->error("redefined parameter");
  }
  const SymValVariable* param_val = dynamic_cast<SymValVariable*>(param_sym->value);
  new_sym_def->value = new SymValVariable(*param_val);
  return std::make_tuple(v->param_type, new_sym_def, v->loc);
}

static void convert_function_body_to_CodeBlob(V<ast_function_declaration> v, V<ast_sequence> v_body) {
  SymDef* sym_def = lookup_symbol(calc_sym_idx(v->get_identifier()->name));
  SymValCodeFunc* sym_val = dynamic_cast<SymValCodeFunc*>(sym_def->value);
  tolk_assert(sym_val != nullptr);

  open_scope(v->loc);
  CodeBlob* blob = new CodeBlob{static_cast<std::string>(v->get_identifier()->name), v->loc, sym_val, v->ret_type};
  if (v->marked_as_pure) {
    blob->flags |= CodeBlob::_ForbidImpure;
  }
  FormalArgList legacy_arg_list;
  for (int i = 0; i < v->get_num_params(); ++i) {
    legacy_arg_list.emplace_back(process_vertex(v->get_param(i), sym_val->parameters[i]));
  }
  blob->import_params(std::move(legacy_arg_list));

  blk_fl::val res = blk_fl::init;
  bool warned = false;
  for (AnyV item : v_body->get_items()) {
    if (!(res & blk_fl::end) && !warned) {
      item->loc.show_warning("unreachable code");
      warned = true;
    }
    blk_fl::combine(res, process_statement(item, *blob));
  }
  if (res & blk_fl::end) {
    append_implicit_ret_stmt(v_body->loc_end, *blob);
  }

  blob->close_blk(v_body->loc_end);
  close_scope();
  sym_val->set_code(blob);
}

static void convert_asm_body_to_AsmOp(V<ast_function_declaration> v, V<ast_asm_body> v_body) {
  SymDef* sym_def = lookup_symbol(calc_sym_idx(v->get_identifier()->name));
  SymValAsmFunc* sym_val = dynamic_cast<SymValAsmFunc*>(sym_def->value);
  tolk_assert(sym_val != nullptr);

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

  sym_val->set_code(std::move(asm_ops));
}


void pipeline_convert_ast_to_legacy_Expr_Op(const AllSrcFiles& all_src_files) {
  for (const SrcFile* file : all_src_files) {
    tolk_assert(file->ast);

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
