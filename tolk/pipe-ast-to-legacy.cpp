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
#include "block/block.h"
#include "block-parse.h"
#include "td/utils/crypto.h"

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

Expr* process_expr(AnyV v, CodeBlob& code);

static void check_global_func(SrcLocation loc, sym_idx_t func_name) {
  SymDef* sym_def = lookup_symbol(func_name);
  if (!sym_def) {
    throw ParseError(loc, "undefined symbol `" + G.symbols.get_name(func_name) + "`");
  }
}

static Expr* make_func_apply(Expr* fun, Expr* x) {
  Expr* res{nullptr};
  if (fun->cls == Expr::_GlobFunc) {
    if (x->cls == Expr::_Tensor) {
      res = new Expr{Expr::_Apply, fun->sym, x->args};
    } else {
      res = new Expr{Expr::_Apply, fun->sym, {x}};
    }
    res->flags = Expr::_IsRvalue | (fun->flags & Expr::_IsImpure);
  } else {
    res = new Expr{Expr::_VarApply, {fun, x}};
    res->flags = Expr::_IsRvalue;
  }
  return res;
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

static Expr* create_new_local_variable(SrcLocation loc, std::string_view var_name, TypeExpr* var_type) {
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
  x->flags = Expr::_IsLvalue;
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
    x->chk_lvalue();
    x->chk_rvalue();
    sym_idx_t name = G.symbols.lookup_add("^_" + operator_name + "_");
    Expr* y = process_expr(v->get_rhs(), code);
    y->chk_rvalue();
    Expr* z = new Expr{Expr::_Apply, name, {x, y}};
    z->here = v->loc;
    z->flags = Expr::_IsRvalue;
    z->deduce_type();
    Expr* res = new Expr{Expr::_Letop, {x->copy(), z}};
    res->here = v->loc;
    res->flags = x->flags | Expr::_IsRvalue;
    res->deduce_type();
    return res;
  }
  if (t == tok_assign) {
    Expr* x = process_expr(v->get_lhs(), code);
    x->chk_lvalue();
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
    sym_idx_t name = G.symbols.lookup_add("_" + operator_name + "_");
    Expr* x = process_expr(v->get_rhs(), code);
    x->chk_rvalue();
    res = new Expr{Expr::_Apply, name, {res, x}};
    res->here = v->loc;
    res->flags = Expr::_IsRvalue;
    res->deduce_type();
    return res;
  }
  if (t == tok_logical_and || t == tok_logical_or) {
    v->error("logical operators are not supported yet");
  }

  v->error("unsupported binary operator");
}

static Expr* process_expr(V<ast_unary_operator> v, CodeBlob& code) {
  TokenType t = v->tok;
  sym_idx_t name = G.symbols.lookup_add(static_cast<std::string>(v->operator_name) + "_");
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

  auto res = new Expr{Expr::_Apply, name, {x}};
  res->here = v->loc;
  res->flags = Expr::_IsRvalue;
  res->deduce_type();
  return res;
}

static Expr* process_expr(V<ast_dot_tilde_call> v, CodeBlob& code) {
  Expr* res = process_expr(v->get_lhs(), code);
  bool modify = v->method_name[0] == '~';
  Expr* obj = res;
  if (modify) {
    obj->chk_lvalue();
  } else {
    obj->chk_rvalue();
  }
  sym_idx_t name_idx = calc_sym_idx(v->method_name);
  const SymDef* sym = lookup_symbol(name_idx);
  if (!sym || !dynamic_cast<SymValFunc*>(sym->value)) {
    sym_idx_t name1 = G.symbols.lookup(v->method_name.substr(1));
    if (name1) {
      const SymDef* sym1 = lookup_symbol(name1);
      if (sym1 && dynamic_cast<SymValFunc*>(sym1->value)) {
        name_idx = name1;
      }
    }
  }
  check_global_func(v->loc, name_idx);
  sym = lookup_symbol(name_idx);
  SymValFunc* val = sym ? dynamic_cast<SymValFunc*>(sym->value) : nullptr;
  if (!val) {
    v->error("undefined method call");
  }
  Expr* x = process_expr(v->get_arg(), code);
  x->chk_rvalue();
  if (x->cls == Expr::_Tensor) {
    res = new Expr{Expr::_Apply, name_idx, {obj}};
    res->args.insert(res->args.end(), x->args.begin(), x->args.end());
  } else {
    res = new Expr{Expr::_Apply, name_idx, {obj, x}};
  }
  res->here = v->loc;
  res->flags = Expr::_IsRvalue | (val->is_marked_as_pure() ? 0 : Expr::_IsImpure);
  res->deduce_type();
  if (modify) {
    Expr* tmp = res;
    res = new Expr{Expr::_LetFirst, {obj->copy(), tmp}};
    res->here = v->loc;
    res->flags = tmp->flags;
    res->set_val(name_idx);
    res->deduce_type();
  }
  return res;
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

static Expr* process_expr(V<ast_function_call> v, CodeBlob& code) {
  // special error for "null()" which is a FunC syntax
  if (v->get_called_f()->type == ast_null_keyword) {
    v->error("null is not a function: use `null`, not `null()`");
  }

  Expr* res = process_expr(v->get_called_f(), code);
  Expr* x = process_expr(v->get_called_arg(), code);
  x->chk_rvalue();
  res = make_func_apply(res, x);
  res->here = v->loc;
  res->deduce_type();
  return res;
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
    f &= x->flags;
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
    f &= x->flags;
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
    case 'a': {  // MsgAddressInt
      // todo rewrite stdaddress parsing (if done, CMake dep "ton_crypto" can be replaced with "ton_crypto_core")
      block::StdAddress a;
      if (a.parse_addr(str)) {
        res->strval = block::tlb::MsgAddressInt().pack_std_address(a)->as_bitslice().to_hex();
      } else {
        v->error("invalid standard address '" + str + "'");
      }
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
  Expr* res = new Expr{Expr::_Apply, builtin_sym, {}};
  res->flags = Expr::_IsRvalue;
  res->deduce_type();
  return res;
}

static Expr* process_expr([[maybe_unused]] V<ast_null_keyword> v) {
  SymDef* builtin_sym = lookup_symbol(calc_sym_idx("__null"));
  Expr* res = new Expr{Expr::_Apply, builtin_sym, {}};
  res->flags = Expr::_IsRvalue;
  res->deduce_type();
  return res;
}

static Expr* process_identifier(V<ast_identifier> v) {
  SymDef* sym = lookup_symbol(calc_sym_idx(v->name));
  if (sym && dynamic_cast<SymValGlobVar*>(sym->value)) {
    check_import_exists_when_using_sym(v, sym);
    auto val = dynamic_cast<SymValGlobVar*>(sym->value);
    Expr* res = new Expr{Expr::_GlobVar, v->loc};
    res->e_type = val->get_type();
    res->sym = sym;
    res->flags = Expr::_IsLvalue | Expr::_IsRvalue | Expr::_IsImpure;
    return res;
  }
  if (sym && dynamic_cast<SymValConst*>(sym->value)) {
    check_import_exists_when_using_sym(v, sym);
    auto val = dynamic_cast<SymValConst*>(sym->value);
    Expr* res = new Expr{Expr::_None, v->loc};
    res->flags = Expr::_IsRvalue;
    if (val->get_kind() == SymValConst::IntConst) {
      res->cls = Expr::_Const;
      res->intval = val->get_int_value();
      res->e_type = TypeExpr::new_atomic(tok_int);
    } else if (val->get_kind() == SymValConst::SliceConst) {
      res->cls = Expr::_SliceConst;
      res->strval = val->get_str_value();
      res->e_type = TypeExpr::new_atomic(tok_slice);
    } else {
      v->error("invalid symbolic constant type");
    }
    return res;
  }
  if (sym && dynamic_cast<SymValFunc*>(sym->value)) {
    check_import_exists_when_using_sym(v, sym);
  }
  Expr* res = new Expr{Expr::_Var, v->loc};
  if (!sym) {
    check_global_func(v->loc, calc_sym_idx(v->name));
    sym = lookup_symbol(calc_sym_idx(v->name));
  }
  res->sym = sym;
  SymVal* val = nullptr;
  bool impure = false;
  if (sym) {
    val = dynamic_cast<SymVal*>(sym->value);
  }
  if (!val) {
    v->error("undefined identifier '" + static_cast<std::string>(v->name) + "'");
  }
  if (val->kind == SymValKind::_Func) {
    res->e_type = val->get_type();
    res->cls = Expr::_GlobFunc;
    impure = !dynamic_cast<SymValFunc*>(val)->is_marked_as_pure();
  } else {
    tolk_assert(val->idx >= 0);
    res->val = val->idx;
    res->e_type = val->get_type();
    // std::cerr << "accessing variable " << lex.cur().str << " : " << res->e_type << std::endl;
  }
  // std::cerr << "accessing symbol " << lex.cur().str << " : " << res->e_type << (val->impure ? " (impure)" : " (pure)") << std::endl;
  res->flags = Expr::_IsLvalue | Expr::_IsRvalue | (impure ? Expr::_IsImpure : 0);
  res->deduce_type();
  return res;
}

Expr* process_expr(AnyV v, CodeBlob& code) {
  switch (v->type) {
    case ast_binary_operator:
      return process_expr(v->as<ast_binary_operator>(), code);
    case ast_unary_operator:
      return process_expr(v->as<ast_unary_operator>(), code);
    case ast_dot_tilde_call:
      return process_expr(v->as<ast_dot_tilde_call>(), code);
    case ast_ternary_operator:
      return process_expr(v->as<ast_ternary_operator>(), code);
    case ast_function_call:
      return process_expr(v->as<ast_function_call>(), code);
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
    case ast_identifier:
      return process_identifier(v->as<ast_identifier>());
    case ast_underscore:
      return create_new_underscore_variable(v->loc, TypeExpr::new_hole());
    default:
      throw UnexpectedASTNodeType(v, "process_expr");
  }
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

static Expr* process_local_vars_lhs(AnyV v, CodeBlob& code) {
  switch (v->type) {
    case ast_local_var: {
      if (v->as<ast_local_var>()->marked_as_redef) {
        return process_identifier(v->as<ast_local_var>()->get_identifier()->as<ast_identifier>());
      }
      TypeExpr* declared_type = v->as<ast_local_var>()->declared_type;
      if (auto v_ident = v->as<ast_local_var>()->get_identifier()->try_as<ast_identifier>()) {
        return create_new_local_variable(v->loc, v_ident->name, declared_type ? declared_type : TypeExpr::new_hole());
      } else {
        return create_new_underscore_variable(v->loc, declared_type ? declared_type : TypeExpr::new_hole());
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
  x->chk_lvalue();
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

static blk_fl::val process_vertex(V<ast_return_statement> v, CodeBlob& code) {
  Expr* expr = process_expr(v->get_return_value(), code);
  expr->chk_rvalue();
  try {
    // std::cerr << "in return: ";
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

static void append_implicit_ret_stmt(V<ast_sequence> v, CodeBlob& code) {
  TypeExpr* ret_type = TypeExpr::new_unit();
  try {
    // std::cerr << "in implicit return: ";
    unify(ret_type, code.ret_type);
  } catch (UnifyError& ue) {
    std::ostringstream os;
    os << "previous function return type " << code.ret_type
       << " cannot be unified with implicit end-of-block return type " << ret_type << ": " << ue;
    throw ParseError(v->loc_end, os.str());
  }
  code.emplace_back(v->loc_end, Op::_Return);
}

blk_fl::val process_statement(AnyV v, CodeBlob& code);

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

  Expr* expr = new Expr{Expr::_Apply, builtin_sym, std::move(args)};
  expr->here = v->loc;
  expr->flags = Expr::_IsRvalue | Expr::_IsImpure;
  expr->deduce_type();
  expr->pre_compile(code);
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
  Expr* expr = new Expr{Expr::_Apply, builtin_sym, std::move(args)};
  expr->here = v->loc;
  expr->flags = Expr::_IsRvalue | Expr::_IsImpure;
  expr->deduce_type();
  expr->pre_compile(code);
  return blk_fl::end;
}

static Expr* process_catch_variable(AnyV catch_var, TypeExpr* var_type) {
  if (auto v_ident = catch_var->try_as<ast_identifier>()) {
    return create_new_local_variable(catch_var->loc, v_ident->name, var_type);
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
      auto expr = process_expr(v, code);
      expr->chk_rvalue();
      expr->pre_compile(code);
      return blk_fl::end;
    }
  }
}

static FormalArg process_vertex(V<ast_parameter> v, int fa_idx) {
  if (v->get_identifier()->name.empty()) {
    return std::make_tuple(v->param_type, (SymDef*)nullptr, v->loc);
  }
  SymDef* new_sym_def = define_symbol(calc_sym_idx(v->get_identifier()->name), true, v->loc);
  if (!new_sym_def) {
    v->error("cannot define symbol");
  }
  if (new_sym_def->value) {
    v->error("redefined argument");
  }
  new_sym_def->value = new SymVal{SymValKind::_Param, fa_idx, v->param_type};
  return std::make_tuple(v->param_type, new_sym_def, v->loc);
}

static void convert_function_body_to_CodeBlob(V<ast_function_declaration> v, V<ast_sequence> v_body) {
  SymDef* sym_def = lookup_symbol(calc_sym_idx(v->get_identifier()->name));
  SymValCodeFunc* sym_val = dynamic_cast<SymValCodeFunc*>(sym_def->value);
  tolk_assert(sym_val != nullptr);

  open_scope(v->loc);
  CodeBlob* blob = new CodeBlob{static_cast<std::string>(v->get_identifier()->name), v->loc, v->ret_type};
  if (v->marked_as_pure) {
    blob->flags |= CodeBlob::_ForbidImpure;
  }
  FormalArgList legacy_arg_list;
  for (int i = 0; i < v->get_num_params(); ++i) {
    legacy_arg_list.emplace_back(process_vertex(v->get_param(i), i));
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
    append_implicit_ret_stmt(v_body, *blob);
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
