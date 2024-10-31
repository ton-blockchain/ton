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
#include "ast-to-legacy.h"
#include "ast.h"
#include "ast-visitor.h"
#include "ast-from-tokens.h"    // todo should be deleted
#include "compiler-state.h"
#include "src-file.h"
#include "tolk.h"
#include "td/utils/crypto.h"
#include "common/refint.h"
#include "openssl/digest.hpp"
#include "block/block.h"
#include "block-parse.h"

/*
 *   In this module, we convert modern AST representation to legacy representation
 * (global state, Expr, CodeBlob, etc.) to make the rest of compiling process remain unchanged for now.
 *   Since time goes, I'll gradually get rid of legacy, since most of the code analysis
 * should be done at AST level.
 */

namespace tolk {

static int calc_sym_idx(std::string_view sym_name) {
  return G.symbols.lookup_add(sym_name);
}


Expr* process_expr(AnyV v, CodeBlob& code, bool nv = false);

static SymValCodeFunc* make_new_glob_func(SymDef* func_sym, TypeExpr* func_type, bool marked_as_pure) {
  SymValCodeFunc* res = new SymValCodeFunc{G.glob_func_cnt, func_type, marked_as_pure};
#ifdef TOLK_DEBUG
  res->name = func_sym->name();
#endif
  func_sym->value = res;
  G.glob_func.push_back(func_sym);
  G.glob_func_cnt++;
  return res;
}

static bool check_global_func(SrcLocation loc, sym_idx_t func_name) {
  SymDef* def = lookup_symbol(func_name);
  if (!def) {
    throw ParseError(loc, "undefined symbol `" + G.symbols.get_name(func_name) + "`");
    return false;
  }
  SymVal* val = dynamic_cast<SymVal*>(def->value);
  if (!val) {
    throw ParseError(loc, "symbol `" + G.symbols.get_name(func_name) + "` has no value and no type");
    return false;
  } else if (!val->get_type()) {
    throw ParseError(loc, "symbol `" + G.symbols.get_name(func_name) + "` has no type, possibly not a function");
    return false;
  } else {
    return true;
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

Expr* process_expr(V<ast_binary_operator> v, CodeBlob& code, bool nv) {
  TokenType t = v->tok;
  std::string operator_name = static_cast<std::string>(v->operator_name);

  if (t == tok_set_plus || t == tok_set_minus || t == tok_set_mul || t == tok_set_div || t == tok_set_divR || t == tok_set_divC ||
      t == tok_set_mod || t == tok_set_modC || t == tok_set_modR || t == tok_set_lshift || t == tok_set_rshift || t == tok_set_rshiftC ||
      t == tok_set_rshiftR || t == tok_set_bitwise_and || t == tok_set_bitwise_or || t == tok_set_bitwise_xor) {
    Expr* x = process_expr(v->get_lhs(), code, nv);
    x->chk_lvalue();
    x->chk_rvalue();
    sym_idx_t name = G.symbols.lookup_add("^_" + operator_name + "_");
    check_global_func(v->loc, name);
    Expr* y = process_expr(v->get_rhs(), code, false);
    y->chk_rvalue();
    Expr* z = new Expr{Expr::_Apply, name, {x, y}};
    z->here = v->loc;
    z->set_val(t);
    z->flags = Expr::_IsRvalue;
    z->deduce_type();
    Expr* res = new Expr{Expr::_Letop, {x->copy(), z}};
    res->here = v->loc;
    res->flags = (x->flags & ~Expr::_IsType) | Expr::_IsRvalue;
    res->set_val(t);
    res->deduce_type();
    return res;
  }
  if (t == tok_assign) {
    Expr* x = process_expr(v->get_lhs(), code, nv);
    x->chk_lvalue();
    Expr* y = process_expr(v->get_rhs(), code, false);
    y->chk_rvalue();
    x->predefine_vars();
    x->define_new_vars(code);
    Expr* res = new Expr{Expr::_Letop, {x, y}};
    res->here = v->loc;
    res->flags = (x->flags & ~Expr::_IsType) | Expr::_IsRvalue;
    res->set_val(t);
    res->deduce_type();
    return res;
  }
  if (t == tok_minus || t == tok_plus ||
      t == tok_bitwise_and || t == tok_bitwise_or || t == tok_bitwise_xor ||
      t == tok_eq || t == tok_lt || t == tok_gt || t == tok_leq || t == tok_geq || t == tok_neq || t == tok_spaceship ||
      t == tok_lshift || t == tok_rshift || t == tok_rshiftC || t == tok_rshiftR ||
      t == tok_mul || t == tok_div || t == tok_mod || t == tok_divmod ||
      t == tok_divC || t == tok_divR || t == tok_modC || t == tok_modR) {
    Expr* res = process_expr(v->get_lhs(), code, nv);
    res->chk_rvalue();
    sym_idx_t name = G.symbols.lookup_add("_" + operator_name + "_");
    check_global_func(v->loc, name);
    Expr* x = process_expr(v->get_rhs(), code, false);
    x->chk_rvalue();
    res = new Expr{Expr::_Apply, name, {res, x}};
    res->here = v->loc;
    res->set_val(t);
    res->flags = Expr::_IsRvalue;
    res->deduce_type();
    return res;
  }

  v->error("unsupported binary operator");
}

Expr* process_expr(V<ast_unary_operator> v, CodeBlob& code) {
  TokenType t = v->tok;
  sym_idx_t name = G.symbols.lookup_add(static_cast<std::string>(v->operator_name) + "_");
  check_global_func(v->loc, name);
  Expr* x = process_expr(v->get_rhs(), code, false);
  x->chk_rvalue();

  // here's an optimization to convert "-1" (tok_minus tok_int_const) to a const -1, not to Expr::Apply(-,1)
  // without this, everything still works, but Tolk looses some vars/stack knowledge for now (to be fixed later)
  // in FunC, it was:
  // `var fst = -1;`   // is constantly 1
  // `var snd = - 1;`  // is Expr::Apply(-), a comment "snd=1" is lost in stack layout comments, and so on
  // hence, when after grammar modification tok_minus became a true unary operator (not a part of a number),
  // and thus to preserve existing behavior until compiler parts are completely rewritten, handle this case here
  if (x->cls == Expr::_Const) {
    if (t == tok_bitwise_not) {
      x->intval = ~x->intval;
    } else if (t == tok_minus) {
      x->intval = -x->intval;
    }
    if (!x->intval->signed_fits_bits(257)) {
      v->error("integer overflow");
    }
    return x;
  }

  auto res = new Expr{Expr::_Apply, name, {x}};
  res->here = v->loc;
  res->set_val(t);
  res->flags = Expr::_IsRvalue;
  res->deduce_type();
  return res;
}

Expr* process_expr(V<ast_dot_tilde_call> v, CodeBlob& code, bool nv) {
  Expr* res = process_expr(v->get_lhs(), code, nv);
  bool modify = v->method_name[0] == '~';
  Expr* obj = res;
  if (modify) {
    obj->chk_lvalue();
  } else {
    obj->chk_rvalue();
  }
  sym_idx_t name = calc_sym_idx(v->method_name);
  const SymDef* sym = lookup_symbol(name);
  if (!sym || !dynamic_cast<SymValFunc*>(sym->value)) {
    sym_idx_t name1 = G.symbols.lookup(v->method_name.substr(1));
    if (name1) {
      const SymDef* sym1 = lookup_symbol(name1);
      if (sym1 && dynamic_cast<SymValFunc*>(sym1->value)) {
        name = name1;
        sym = sym1;
      }
    }
  }
  check_global_func(v->loc, name);
  if (G.is_verbosity(2)) {
    std::cerr << "using symbol `" << G.symbols.get_name(name) << "` for method call of " << v->method_name << std::endl;
  }
  sym = lookup_symbol(name);
  SymValFunc* val = sym ? dynamic_cast<SymValFunc*>(sym->value) : nullptr;
  if (!val) {
    v->error("undefined method call");
  }
  Expr* x = process_expr(v->get_arg(), code, false);
  x->chk_rvalue();
  if (x->cls == Expr::_Tensor) {
    res = new Expr{Expr::_Apply, name, {obj}};
    res->args.insert(res->args.end(), x->args.begin(), x->args.end());
  } else {
    res = new Expr{Expr::_Apply, name, {obj, x}};
  }
  res->here = v->loc;
  res->flags = Expr::_IsRvalue | (val->is_marked_as_pure() ? 0 : Expr::_IsImpure);
  res->deduce_type();
  if (modify) {
    auto tmp = res;
    res = new Expr{Expr::_LetFirst, {obj->copy(), tmp}};
    res->here = v->loc;
    res->flags = tmp->flags;
    res->set_val(name);
    res->deduce_type();
  }
  return res;
}

Expr* process_expr(V<ast_ternary_operator> v, CodeBlob& code, bool nv) {
  Expr* cond = process_expr(v->get_cond(), code, nv);
  cond->chk_rvalue();
  Expr* x = process_expr(v->get_when_true(), code, false);
  x->chk_rvalue();
  Expr* y = process_expr(v->get_when_false(), code, false);
  y->chk_rvalue();
  Expr* res = new Expr{Expr::_CondExpr, {cond, x, y}};
  res->here = v->loc;
  res->flags = Expr::_IsRvalue;
  res->deduce_type();
  return res;
}

Expr* process_expr(V<ast_function_call> v, CodeBlob& code, bool nv) {
  Expr* res = process_expr(v->get_called_f(), code, nv);
  Expr* x = process_expr(v->get_called_arg(), code, false);
  x->chk_rvalue();
  res = make_func_apply(res, x);
  res->here = v->loc;
  res->deduce_type();
  return res;
}

Expr* process_expr(V<ast_tensor> v, CodeBlob& code, bool nv) {
  if (v->empty()) {
    Expr* res = new Expr{Expr::_Tensor, {}};
    res->flags = Expr::_IsRvalue;
    res->here = v->loc;
    res->e_type = TypeExpr::new_unit();
    return res;
  }

  Expr* res = process_expr(v->get_item(0), code, nv);
  std::vector<TypeExpr*> type_list;
  type_list.push_back(res->e_type);
  int f = res->flags;
  res = new Expr{Expr::_Tensor, {res}};
  for (int i = 1; i < v->size(); ++i) {
    Expr* x = process_expr(v->get_item(i), code, nv);
    res->pb_arg(x);
    f &= x->flags;
    type_list.push_back(x->e_type);
  }
  res->here = v->loc;
  res->flags = f;
  res->e_type = TypeExpr::new_tensor(std::move(type_list));
  return res;
}

Expr* process_expr(V<ast_variable_declaration> v, CodeBlob& code) {
  Expr* x = process_expr(v->get_variable_or_list(), code, true);
  x->chk_lvalue();  // chk_lrvalue() ?
  Expr* res = new Expr{Expr::_TypeApply, {x}};
  res->e_type = v->declared_type;
  res->here = v->loc;
  try {
    unify(res->e_type, x->e_type);
  } catch (UnifyError& ue) {
    std::ostringstream os;
    os << "cannot transform expression of type " << x->e_type << " to explicitly requested type " << res->e_type
       << ": " << ue;
    v->error(os.str());
  }
  res->flags = x->flags;
  return res;
}

Expr* process_expr(V<ast_tensor_square> v, CodeBlob& code, bool nv) {
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

  Expr* res = process_expr(v->get_item(0), code, nv);
  std::vector<TypeExpr*> type_list;
  type_list.push_back(res->e_type);
  int f = res->flags;
  res = new Expr{Expr::_Tensor, {res}};
  for (int i = 1; i < v->size(); ++i) {
    Expr* x = process_expr(v->get_item(i), code, nv);
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

Expr* process_expr(V<ast_int_const> v) {
  Expr* res = new Expr{Expr::_Const, v->loc};
  res->flags = Expr::_IsRvalue;
  res->intval = td::string_to_int256(static_cast<std::string>(v->int_val));
  if (res->intval.is_null() || !res->intval->signed_fits_bits(257)) {
    v->error("invalid integer constant");
  }
  res->e_type = TypeExpr::new_atomic(TypeExpr::_Int);
  return res;
}

Expr* process_expr(V<ast_string_const> v) {
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
        v->error("Invalid hex bitstring constant '" + str + "'");
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
      __builtin_unreachable();
  }
  return res;
}

Expr* process_expr(V<ast_bool_const> v) {
  SymDef* sym = lookup_symbol(calc_sym_idx(v->bool_val ? "true" : "false"));
  tolk_assert(sym);
  Expr* res = new Expr{Expr::_Apply, sym, {}};
  res->flags = Expr::_IsRvalue;
  res->deduce_type();
  return res;
}

Expr* process_expr([[maybe_unused]] V<ast_nil_tuple> v) {
  SymDef* sym = lookup_symbol(calc_sym_idx("nil"));
  tolk_assert(sym);
  Expr* res = new Expr{Expr::_Apply, sym, {}};
  res->flags = Expr::_IsRvalue;
  res->deduce_type();
  return res;
}

Expr* process_expr(V<ast_identifier> v, bool nv) {
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
      v->error("Invalid symbolic constant type");
    }
    return res;
  }
  if (sym && dynamic_cast<SymValFunc*>(sym->value)) {
    check_import_exists_when_using_sym(v, sym);
  }
  Expr* res = new Expr{Expr::_Var, v->loc};
  if (nv) {
    res->val = ~calc_sym_idx(v->name);
    res->e_type = TypeExpr::new_hole();
    res->flags = Expr::_IsLvalue;
    // std::cerr << "defined new variable " << lex.cur().str << " : " << res->e_type << std::endl;
  } else {
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
  }
  res->deduce_type();
  return res;
}

Expr* process_expr(AnyV v, CodeBlob& code, bool nv) {
  switch (v->type) {
    case ast_binary_operator:
      return process_expr(v->as<ast_binary_operator>(), code, nv);
    case ast_unary_operator:
      return process_expr(v->as<ast_unary_operator>(), code);
    case ast_dot_tilde_call:
      return process_expr(v->as<ast_dot_tilde_call>(), code, nv);
    case ast_ternary_operator:
      return process_expr(v->as<ast_ternary_operator>(), code, nv);
    case ast_function_call:
      return process_expr(v->as<ast_function_call>(), code, nv);
    case ast_parenthesized_expr:
      return process_expr(v->as<ast_parenthesized_expr>()->get_expr(), code, nv);
    case ast_variable_declaration:
      return process_expr(v->as<ast_variable_declaration>(), code);
    case ast_tensor:
      return process_expr(v->as<ast_tensor>(), code, nv);
    case ast_tensor_square:
      return process_expr(v->as<ast_tensor_square>(), code, nv);
    case ast_int_const:
      return process_expr(v->as<ast_int_const>());
    case ast_string_const:
      return process_expr(v->as<ast_string_const>());
    case ast_bool_const:
      return process_expr(v->as<ast_bool_const>());
    case ast_nil_tuple:
      return process_expr(v->as<ast_nil_tuple>());
    case ast_identifier:
      return process_expr(v->as<ast_identifier>(), nv);

    case ast_underscore: {
      Expr* res = new Expr{Expr::_Hole, v->loc};
      res->val = -1;
      res->flags = Expr::_IsLvalue;
      res->e_type = TypeExpr::new_hole();
      return res;
    }
    case ast_type_expression: {
      Expr* res = new Expr{Expr::_Type, v->loc};
      res->flags = Expr::_IsType;
      res->e_type = v->as<ast_type_expression>()->declared_type;
      return res;
    }
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

blk_fl::val process_vertex(V<ast_return_statement> v, CodeBlob& code) {
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

void append_implicit_ret_stmt(V<ast_sequence> v, CodeBlob& code) {
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

blk_fl::val process_stmt(AnyV v, CodeBlob& code);

blk_fl::val process_vertex(V<ast_sequence> v, CodeBlob& code, bool no_new_scope = false) {
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
    blk_fl::combine(res, process_stmt(item, code));
  }
  if (!no_new_scope) {
    close_scope();
  }
  return res;
}

blk_fl::val process_vertex(V<ast_repeat_statement> v, CodeBlob& code) {
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

blk_fl::val process_vertex(V<ast_while_statement> v, CodeBlob& code) {
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

blk_fl::val process_vertex(V<ast_do_until_statement> v, CodeBlob& code) {
  Op& until_op = code.emplace_back(v->loc, Op::_Until);
  code.push_set_cur(until_op.block0);
  open_scope(v->loc);
  blk_fl::val res = process_vertex(v->get_body(), code, true);
  Expr* expr = process_expr(v->get_cond(), code);
  expr->chk_rvalue();
  close_scope();
  auto cnt_type = TypeExpr::new_atomic(TypeExpr::_Int);
  try {
    unify(expr->e_type, cnt_type);
  } catch (UnifyError& ue) {
    std::ostringstream os;
    os << "`until` condition value of type " << expr->e_type << " is not an integer: " << ue;
    v->get_cond()->error(os.str());
  }
  until_op.left = expr->pre_compile(code);
  code.close_pop_cur(v->get_body()->loc_end);
  if (until_op.left.size() != 1) {
    v->get_cond()->error("`until` condition value is not a singleton");
  }
  return res & ~blk_fl::empty;
}

blk_fl::val process_vertex(V<ast_try_catch_statement> v, CodeBlob& code) {
  code.require_callxargs = true;
  Op& try_catch_op = code.emplace_back(v->loc, Op::_TryCatch);
  code.push_set_cur(try_catch_op.block0);
  blk_fl::val res0 = process_vertex(v->get_try_body(), code);
  code.close_pop_cur(v->get_try_body()->loc_end);
  code.push_set_cur(try_catch_op.block1);
  open_scope(v->get_catch_expr()->loc);
  Expr* expr = process_expr(v->get_catch_expr(), code, true);
  expr->chk_lvalue();
  TypeExpr* tvm_error_type = TypeExpr::new_tensor(TypeExpr::new_var(), TypeExpr::new_atomic(TypeExpr::_Int));
  try {
    unify(expr->e_type, tvm_error_type);
  } catch (UnifyError& ue) {
    std::ostringstream os;
    os << "`catch` arguments have incorrect type " << expr->e_type << ": " << ue;
    v->get_catch_expr()->error(os.str());
  }
  expr->predefine_vars();
  expr->define_new_vars(code);
  try_catch_op.left = expr->pre_compile(code);
  tolk_assert(try_catch_op.left.size() == 2 || try_catch_op.left.size() == 1);
  blk_fl::val res1 = process_vertex(v->get_catch_body(), code);
  close_scope();
  code.close_pop_cur(v->get_catch_body()->loc_end);
  blk_fl::combine_parallel(res0, res1);
  return res0;
}

blk_fl::val process_vertex(V<ast_if_statement> v, CodeBlob& code, TokenType first_lex = tok_if) {
  Expr* expr = process_expr(v->get_cond(), code);
  expr->chk_rvalue();
  auto flag_type = TypeExpr::new_atomic(TypeExpr::_Int);
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

blk_fl::val process_stmt(AnyV v, CodeBlob& code) {
  switch (v->type) {
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
    case ast_do_until_statement:
      return process_vertex(v->as<ast_do_until_statement>(), code);
    case ast_while_statement:
      return process_vertex(v->as<ast_while_statement>(), code);
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

FormalArg process_vertex(V<ast_argument> v, int fa_idx) {
  if (v->arg_name.empty()) {
    return std::make_tuple(v->arg_type, (SymDef*)nullptr, v->loc);
  }
  if (G.prohibited_var_names.count(static_cast<std::string>(v->arg_name))) {
    v->error("symbol `" + static_cast<std::string>(v->arg_name) + "` cannot be redefined as a variable");
  }
  SymDef* new_sym_def = define_symbol(calc_sym_idx(v->arg_name), true, v->loc);
  if (!new_sym_def) {
    v->error("cannot define symbol");
  }
  if (new_sym_def->value) {
    v->error("redefined argument");
  }
  new_sym_def->value = new SymVal{SymValKind::_Param, fa_idx, v->arg_type};
  return std::make_tuple(v->arg_type, new_sym_def, v->loc);
}

CodeBlob* process_vertex(V<ast_sequence> v_body, V<ast_argument_list> arg_list, TypeExpr* ret_type, bool marked_as_pure) {
  CodeBlob* blob = new CodeBlob{ret_type};
  if (marked_as_pure) {
    blob->flags |= CodeBlob::_ForbidImpure;
  }
  FormalArgList legacy_arg_list;
  for (int i = 0; i < arg_list->size(); ++i) {
    legacy_arg_list.emplace_back(process_vertex(arg_list->get_arg(i), i));
  }
  blob->import_params(std::move(legacy_arg_list));
  blk_fl::val res = blk_fl::init;
  bool warned = false;
  for (AnyV item : v_body->get_items()) {
    if (!(res & blk_fl::end) && !warned) {
      item->loc.show_warning("unreachable code");
      warned = true;
    }
    blk_fl::combine(res, process_stmt(item, *blob));
  }
  if (res & blk_fl::end) {
    append_implicit_ret_stmt(v_body, *blob);
  }
  blob->close_blk(v_body->loc_end);
  return blob;
}

SymValAsmFunc* process_vertex(V<ast_asm_body> v_body, TypeExpr* func_type, V<ast_argument_list> arg_list, TypeExpr* ret_type,
                                   bool marked_as_pure) {
  int cnt = arg_list->size();
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
    V<ast_argument> arg = arg_list->get_arg(i);
    int arg_width = arg->arg_type->get_width();
    if (arg_width < 0 || arg_width > 16) {
      arg->error("parameters of an assembler built-in function must have a well-defined fixed width");
    }
    cum_arg_width.push_back(tot_width += arg_width);
  }
  std::vector<AsmOp> asm_ops;
  std::vector<int> arg_order, ret_order;
  if (!v_body->arg_order.empty()) {
    if (static_cast<int>(v_body->arg_order.size()) != cnt) {
      v_body->error("arg_order of asm function must specify all arguments");
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
  for (AnyV v_child : v_body->get_asm_commands()) {
    std::string_view ops = v_child->as<ast_string_const>()->str_val; // <op>\n<op>\n...
    std::string op;
    for (const char& c : ops) {
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
  std::string crc_s;
  for (const AsmOp& asm_op : asm_ops) {
    crc_s += asm_op.op;
  }
  crc_s.push_back(!marked_as_pure);
  for (const int& x : arg_order) {
    crc_s += std::string((const char*) (&x), (const char*) (&x + 1));
  }
  for (const int& x : ret_order) {
    crc_s += std::string((const char*) (&x), (const char*) (&x + 1));
  }
  auto res = new SymValAsmFunc{func_type, std::move(asm_ops), marked_as_pure};
  res->arg_order = std::move(arg_order);
  res->ret_order = std::move(ret_order);
  res->crc = td::crc64(crc_s);
  return res;
}

// if a function looks like `T f(...args) { return anotherF(...args); }`,
// set a bit to flags
// then, all calls to `f(...)` will be effectively replaced with `anotherF(...)`
void detect_if_function_just_wraps_another(SymValCodeFunc* v_current, const td::RefInt256 &method_id) {
  const std::string& function_name = v_current->code->name;

  // in "AST" representation, the first is Op::_Import (input arguments, even if none)
  const auto& op_import = v_current->code->ops;
  tolk_assert(op_import && op_import->cl == Op::_Import);

  // then Op::_Call (anotherF)
  const Op* op_call = op_import->next.get();
  if (!op_call || op_call->cl != Op::_Call)
    return;
  tolk_assert(op_call->left.size() == 1);

  const auto& op_return = op_call->next;
  if (!op_return || op_return->cl != Op::_Return || op_return->left.size() != 1)
    return;

  bool indices_expected = op_import->left.size() == op_call->left[0] && op_call->left[0] == op_return->left[0];
  if (!indices_expected)
    return;

  const SymDef* f_called = op_call->fun_ref;
  const SymValFunc* v_called = dynamic_cast<SymValFunc*>(f_called->value);
  if (!v_called)
    return;

  // `return` must use all arguments, e.g. `return (_0,_2,_1)`, not `return (_0,_1,_1)`
  int args_used_mask = 0;
  for (var_idx_t arg_idx : op_call->right) {
    args_used_mask |= 1 << arg_idx;
  }
  if (args_used_mask != (1 << op_call->right.size()) - 1)
    return;

  // detect getters (having method_id), they should not be treated as wrappers
  // v_current->method_id will be assigned later; todo refactor function parsing completely, it's weird
  // moreover, `recv_external()` and others are also exported, but FunC is unaware of method_id
  // (it's assigned by Fift later)
  // so, for now, just handle "special" function names, the same as in Asm.fif
  if (!method_id.is_null())
    return;
  if (function_name == "main" || function_name == "recv_internal" || function_name == "recv_external" ||
      function_name == "run_ticktock" || function_name == "split_prepare" || function_name == "split_install")
    return;

  // all types must be strictly defined (on mismatch, a compilation error will be triggered anyway)
  if (v_called->sym_type->has_unknown_inside() || v_current->sym_type->has_unknown_inside())
    return;
  // avoid situations like `f(int a, (int,int) b)`, inlining will be cumbersome
  if (v_current->get_arg_type()->get_width() != op_call->right.size())
    return;
  // 'return true;' (false, nil) are (surprisingly) also function calls
  if (f_called->name() == "true" || f_called->name() == "false" || f_called->name() == "nil")
    return;
  // if an original is marked `pure`, and this one doesn't, it's okay; just check for inline_ref storage
  if (v_current->is_inline_ref())
    return;

  // ok, f_current is a wrapper
  v_current->flags |= SymValFunc::flagWrapsAnotherF;
  if (G.is_verbosity(2)) {
    std::cerr << function_name << " -> " << f_called->name() << std::endl;
  }
}

static td::RefInt256 calculate_method_id_by_func_name(std::string_view func_name) {
  unsigned int crc = td::crc16(static_cast<std::string>(func_name));
  return td::make_refint((crc & 0xffff) | 0x10000);
}

void process_vertex(V<ast_function_declaration> v_function) {
  open_scope(v_function->loc);
  std::vector<TypeExpr*> type_vars;
  if (v_function->forall_list) {
    type_vars.reserve(v_function->forall_list->size());
    for (int idx = 0; idx < v_function->forall_list->size(); ++idx) {
      type_vars.emplace_back(v_function->forall_list->get_item(idx)->created_type);
    }
  }
  std::string func_name = v_function->name;
  int func_sym_idx = calc_sym_idx(func_name);
  int flags_inline = 0;
  if (v_function->marked_as_inline) {
    flags_inline = SymValFunc::flagInline;
  } else if (v_function->marked_as_inline_ref) {
    flags_inline = SymValFunc::flagInlineRef;
  }
  td::RefInt256 method_id;
  if (v_function->method_id) {
    method_id = td::string_to_int256(static_cast<std::string>(v_function->method_id->int_val));
    if (method_id.is_null()) {
      v_function->method_id->error("invalid integer constant");
    }
  } else if (v_function->marked_as_get_method) {
    method_id = calculate_method_id_by_func_name(func_name);
    for (const SymDef* other : G.glob_get_methods) {
      if (!td::cmp(dynamic_cast<const SymValFunc*>(other->value)->method_id, method_id)) {
        v_function->error(PSTRING() << "GET methods hash collision: `" << other->name() << "` and `" + func_name + "` produce the same hash. Consider renaming one of these functions.");
      }
    }
  }
  TypeExpr* arg_list_type = nullptr;
  if (int n_args = v_function->get_num_args()) {
    std::vector<TypeExpr*> arg_types;
    arg_types.reserve(n_args);
    for (int idx = 0; idx < n_args; ++idx) {
      arg_types.emplace_back(v_function->get_arg(idx)->arg_type);
    }
    arg_list_type = TypeExpr::new_tensor(std::move(arg_types));
  } else {
    arg_list_type = TypeExpr::new_unit();
  }
  TypeExpr* func_type = TypeExpr::new_map(arg_list_type, v_function->ret_type);
  if (!type_vars.empty()) {
    func_type = TypeExpr::new_forall(std::move(type_vars), func_type);
  }
  if (v_function->marked_as_builtin) {
    const SymDef* builtin_func = lookup_symbol(G.symbols.lookup(func_name));
    const SymValFunc* func_val = builtin_func ? dynamic_cast<SymValFunc*>(builtin_func->value) : nullptr;
    if (!func_val || !func_val->is_builtin()) {
      v_function->error("`builtin` used for non-builtin function");
    }
#ifdef TOLK_DEBUG
    // in release, we don't need this check, since `builtin` is used only in stdlib.tolk, which is our responsibility
    if (!func_val->sym_type->equals_to(func_type) || func_val->is_marked_as_pure() != v_function->marked_as_pure) {
      v_function->error("declaration for `builtin` function doesn't match an actual one");
    }
#endif
    close_scope();
    return;
  }
  if (G.is_verbosity(1)) {
    std::cerr << "fun " << func_name << " : " << func_type << std::endl;
  }
  SymDef* func_sym = define_global_symbol(func_sym_idx, 0, v_function->loc);
  tolk_assert(func_sym);
  SymValFunc* func_sym_val = dynamic_cast<SymValFunc*>(func_sym->value);
  if (func_sym->value) {
    // todo remove all about pre-declarations and prototypes
    if (func_sym->value->kind != SymValKind::_Func || !func_sym_val) {
      v_function->error("was not defined as a function before");
    }
    try {
      unify(func_sym_val->sym_type, func_type);
    } catch (UnifyError& ue) {
      std::ostringstream os;
      os << "previous type of function " << func_name << " : " << func_sym_val->sym_type
         << " cannot be unified with new type " << func_type << ": " << ue;
      v_function->error(os.str());
    }
  }
  if (v_function->get_body()->type == ast_empty) {
    make_new_glob_func(func_sym, func_type, v_function->marked_as_pure);
  } else if (const auto* v_seq = v_function->get_body()->try_as<ast_sequence>()) {
    if (dynamic_cast<SymValAsmFunc*>(func_sym_val)) {
      v_function->error("function `" + func_name + "` has been already defined as an assembler built-in");
    }
    SymValCodeFunc* func_sym_code;
    if (func_sym_val) {
      func_sym_code = dynamic_cast<SymValCodeFunc*>(func_sym_val);
      if (!func_sym_code) {
        v_function->error("function `" + func_name + "` has been already defined in an yet-unknown way");
      }
    } else {
      func_sym_code = make_new_glob_func(func_sym, func_type, v_function->marked_as_pure);
    }
    if (func_sym_code->code) {
      v_function->error("redefinition of function `" + func_name + "`");
    }
    if (v_function->marked_as_pure && v_function->ret_type->get_width() == 0) {
      v_function->error("a pure function should return something, otherwise it will be optimized out anyway");
    }
    CodeBlob* code = process_vertex(v_seq, v_function->get_arg_list(), v_function->ret_type, v_function->marked_as_pure);
    code->name = func_name;
    code->loc = v_function->loc;
    func_sym_code->code = code;
    // todo it should be done not here, it should be on ast level, it should work when functions are declared swapped
    detect_if_function_just_wraps_another(func_sym_code, method_id);
  } else if (const auto* v_asm = v_function->get_body()->try_as<ast_asm_body>()) {
    SymValAsmFunc* asm_func = process_vertex(v_asm, func_type, v_function->get_arg_list(), v_function->ret_type, v_function->marked_as_pure);
#ifdef TOLK_DEBUG
    asm_func->name = func_name;
#endif
    if (func_sym_val) {
      if (dynamic_cast<SymValCodeFunc*>(func_sym_val)) {
        v_function->error("function `" + func_name + "` was already declared as an ordinary function");
      }
      SymValAsmFunc* asm_func_old = dynamic_cast<SymValAsmFunc*>(func_sym_val);
      if (asm_func_old) {
        if (asm_func->crc != asm_func_old->crc) {
          v_function->error("redefinition of built-in assembler function `" + func_name + "`");
        }
      } else {
        v_function->error("redefinition of previously (somehow) defined function `" + func_name + "`");
      }
    }
    func_sym->value = asm_func;
  }
  if (method_id.not_null()) {
    auto val = dynamic_cast<SymValFunc*>(func_sym->value);
    if (!val) {
      v_function->error("cannot set method id for unknown function `" + func_name + "`");
    }
    if (val->method_id.is_null()) {
      val->method_id = std::move(method_id);
    } else if (td::cmp(val->method_id, method_id) != 0) {
      v_function->error("integer method identifier for `" + func_name + "` changed from " +
                      val->method_id->to_dec_string() + " to a different value " + method_id->to_dec_string());
    }
  }
  if (flags_inline) {
    auto val = dynamic_cast<SymValFunc*>(func_sym->value);
    if (!val) {
      v_function->error("cannot set unknown function `" + func_name + "` as an inline");
    }
    if (!val->is_inline() && !val->is_inline_ref()) {
      val->flags |= flags_inline;
    } else if ((val->flags & (SymValFunc::flagInline | SymValFunc::flagInlineRef)) != flags_inline) {
      v_function->error("inline mode for `" + func_name + "` changed with respect to a previous declaration");
    }
  }
  if (v_function->marked_as_get_method) {
    auto val = dynamic_cast<SymValFunc*>(func_sym->value);
    if (!val) {
      v_function->error("cannot set unknown function `" + func_name + "` as a get method");
    }
    val->flags |= SymValFunc::flagGetMethod;
    G.glob_get_methods.push_back(func_sym);
  }
  close_scope();
}

td::Result<SrcFile*> locate_source_file(const std::string& rel_filename) {
  td::Result<std::string> path = G.settings.read_callback(CompilerSettings::FsReadCallbackKind::Realpath, rel_filename.c_str());
  if (path.is_error()) {
    return path.move_as_error();
  }

  std::string abs_filename = path.move_as_ok();
  if (SrcFile* file = G.all_src_files.find_file(abs_filename)) {
    return file; // file was already parsed (imported from somewhere else)
  }

  td::Result<std::string> text = G.settings.read_callback(CompilerSettings::FsReadCallbackKind::ReadFile, abs_filename.c_str());
  if (text.is_error()) {
    return text.move_as_error();
  }

  return G.all_src_files.register_file(rel_filename, abs_filename, text.move_as_ok());
}

void process_vertex(V<ast_pragma_no_arg> v) {
  std::string_view pragma_name = v->pragma_name;
  if (pragma_name == G.pragma_allow_post_modification.name()) {
    G.pragma_allow_post_modification.enable(v->loc);
  } else if (pragma_name == G.pragma_compute_asm_ltr.name()) {
    G.pragma_compute_asm_ltr.enable(v->loc);
  } else if (pragma_name == G.pragma_remove_unused_functions.name()) {
    G.pragma_remove_unused_functions.enable(v->loc);
  } else {
    v->error("unknown pragma name");
  }
}

void process_vertex(V<ast_pragma_version> v) {
  char op = '='; bool eq = false;
  TokenType cmp_tok = v->cmp_tok;
  if (cmp_tok == tok_gt || cmp_tok == tok_geq) {
    op = '>';
    eq = cmp_tok == tok_geq;
  } else if (cmp_tok == tok_lt || cmp_tok == tok_leq) {
    op = '<';
    eq = cmp_tok == tok_leq;
  } else if (cmp_tok == tok_eq) {
    op = '=';
  } else if (cmp_tok == tok_bitwise_xor) {
    op = '^';
  } else {
    v->error("invalid comparison operator");
  }
  std::string_view pragma_value = v->semver;
  int sem_ver[3] = {0, 0, 0};
  char segs = 1;
  auto stoi = [&](std::string_view s) {
    auto R = td::to_integer_safe<int>(static_cast<std::string>(s));
    if (R.is_error()) {
      v->error("invalid semver format");
    }
    return R.move_as_ok();
  };
  std::istringstream iss_value(static_cast<std::string>(pragma_value));
  for (int idx = 0; idx < 3; idx++) {
    std::string s{"0"};
    std::getline(iss_value, s, '.');
    sem_ver[idx] = stoi(s);
  }
  // End reading semver from source code
  int tolk_ver[3] = {0, 0, 0};
  std::istringstream iss(tolk_version);
  for (int idx = 0; idx < 3; idx++) {
    std::string s;
    std::getline(iss, s, '.');
    tolk_ver[idx] = stoi(s);
  }
  // End parsing embedded semver
  bool match = true;
  switch (op) {
    case '=':
      if ((tolk_ver[0] != sem_ver[0]) ||
          (tolk_ver[1] != sem_ver[1]) ||
          (tolk_ver[2] != sem_ver[2])) {
        match = false;
          }
    break;
    case '>':
      if ( ((tolk_ver[0] == sem_ver[0]) && (tolk_ver[1] == sem_ver[1]) && (tolk_ver[2] == sem_ver[2]) && !eq) ||
           ((tolk_ver[0] == sem_ver[0]) && (tolk_ver[1] == sem_ver[1]) && (tolk_ver[2] < sem_ver[2])) ||
           ((tolk_ver[0] == sem_ver[0]) && (tolk_ver[1] < sem_ver[1])) ||
           ((tolk_ver[0] < sem_ver[0])) ) {
        match = false;
           }
    break;
    case '<':
      if ( ((tolk_ver[0] == sem_ver[0]) && (tolk_ver[1] == sem_ver[1]) && (tolk_ver[2] == sem_ver[2]) && !eq) ||
           ((tolk_ver[0] == sem_ver[0]) && (tolk_ver[1] == sem_ver[1]) && (tolk_ver[2] > sem_ver[2])) ||
           ((tolk_ver[0] == sem_ver[0]) && (tolk_ver[1] > sem_ver[1])) ||
           ((tolk_ver[0] > sem_ver[0])) ) {
        match = false;
           }
    break;
    case '^':
      if ( ((segs == 3) && ((tolk_ver[0] != sem_ver[0]) || (tolk_ver[1] != sem_ver[1]) || (tolk_ver[2] < sem_ver[2])))
        || ((segs == 2) && ((tolk_ver[0] != sem_ver[0]) || (tolk_ver[1] < sem_ver[1])))
        || ((segs == 1) && ((tolk_ver[0] < sem_ver[0]))) ) {
        match = false;
        }
    break;
    default:
      __builtin_unreachable();
  }
  if (!match) {
    v->error("Tolk version " + tolk_version + " does not satisfy this condition");
  }
}

void process_vertex(V<ast_include_statement> v, SrcFile* current_file) {
  std::string rel_filename = static_cast<std::string>(v->file_name);
  if (size_t rc = current_file->rel_filename.rfind('/'); rc != std::string::npos) {
    rel_filename = current_file->rel_filename.substr(0, rc + 1) + rel_filename;
  }

  td::Result<SrcFile*> locate_res = locate_source_file(rel_filename);
  if (locate_res.is_error()) {
    v->error("Failed to import: " + locate_res.move_as_error().message().str());
  }

  SrcFile* imported_file = locate_res.move_as_ok();
  current_file->imports.emplace_back(SrcFile::ImportStatement{imported_file});
  if (!imported_file->was_parsed) {
    // todo it's wrong, but ok for now
    process_file_ast(parse_src_file_to_ast(imported_file));
  }
}

void process_vertex(V<ast_constant_declaration> v) {
  AnyV init_value = v->get_init_value();
  SymDef* sym_def = define_global_symbol(calc_sym_idx(v->const_name), false, v->loc);
  if (!sym_def) {
    v->error("cannot define global symbol");
  }
  if (sym_def->value) {
    v->error("symbol already exists");
  }
  CodeBlob code;
  Expr* x = process_expr(init_value, code, false);
  if (!x->is_rvalue()) {
    v->get_init_value()->error("expression is not strictly Rvalue");
  }
  if (v->declared_type && !v->declared_type->equals_to(x->e_type)) {
    v->error("expression type does not match declared type");
  }
  SymValConst* new_value = nullptr;
  if (x->cls == Expr::_Const) {  // Integer constant
    new_value = new SymValConst{G.const_cnt++, x->intval};
  } else if (x->cls == Expr::_SliceConst) {  // Slice constant (string)
    new_value = new SymValConst{G.const_cnt++, x->strval};
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
    new_value = new SymValConst{G.const_cnt++, op.origin};
  } else {
    init_value->error("integer or slice literal or constant expected");
  }
  sym_def->value = new_value;
}

void process_vertex(V<ast_global_var_declaration> v) {
  TypeExpr* var_type = v->declared_type;
  SymDef* sym_def = define_global_symbol(calc_sym_idx(v->var_name), false, v->loc);
  if (!sym_def) {
    v->error("cannot define global symbol");
  }
  if (sym_def->value) {
    auto val = dynamic_cast<SymValGlobVar*>(sym_def->value);
    if (!val) {
      v->error("symbol cannot be redefined as a global variable");
    }
    try {
      unify(var_type, val->sym_type);
    } catch (UnifyError& ue) {
      std::ostringstream os;
      os << "cannot unify new type " << var_type << " of global variable `" << sym_def->name()
         << "` with its previous type " << val->sym_type << ": " << ue;
      v->error(os.str());
    }
  } else {
    sym_def->value = new SymValGlobVar{G.glob_var_cnt++, var_type};
#ifdef TOLK_DEBUG
    dynamic_cast<SymValGlobVar*>(sym_def->value)->name = v->var_name;
#endif
    G.glob_vars.push_back(sym_def);
  }
}

class FileToLegacyVisitor final : public ASTVisitorToplevelDeclarations {
  SrcFile* current_file;

  // todo inline here all these
  void on_pragma_no_arg(V<ast_pragma_no_arg> v) override {
    process_vertex(v);
  }

  void on_pragma_version(V<ast_pragma_version> v) override {
    process_vertex(v);
  }

  void on_include_statement(V<ast_include_statement> v) override {
    process_vertex(v, current_file);
  }

  void on_function_declaration(V<ast_function_declaration> v) override {
    process_vertex(v);
  }

  void on_constant_declaration(V<ast_constant_declaration> v) override {
    process_vertex(v);
  }

  void on_global_var_declaration(V<ast_global_var_declaration> v) override {
    process_vertex(v);
  }

public:
  explicit FileToLegacyVisitor(SrcFile* file) : current_file(file) {
  }
};

void process_file_ast(AnyV file_ast) {
  auto v = file_ast->try_as<ast_tolk_file>();
  if (!v) {
    throw UnexpectedASTNodeType(file_ast, "process_file_ast");
  }

  const SrcFile* file = v->file;
  if (!file->is_stdlib_file()) {
    // v->debug_print();
    G.generated_from += file->rel_filename;
    G.generated_from += ", ";
  }

  FileToLegacyVisitor(const_cast<SrcFile*>(file)).start_visiting_file(v);
}

} // namespace tolk
