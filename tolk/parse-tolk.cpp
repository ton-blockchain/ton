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
#include "platform-utils.h"
#include "compiler-state.h"
#include "td/utils/crypto.h"
#include "common/refint.h"
#include "openssl/digest.hpp"
#include "block/block.h"
#include "block-parse.h"

namespace tolk {
using namespace std::literals::string_literals;

inline bool is_dot_ident(sym_idx_t idx) {
  return G.symbols.get_subclass(idx) == SymbolSubclass::dot_identifier;
}

inline bool is_tilde_ident(sym_idx_t idx) {
  return G.symbols.get_subclass(idx) == SymbolSubclass::tilde_identifier;
}

inline bool is_special_ident(sym_idx_t idx) {
  return G.symbols.get_subclass(idx) != SymbolSubclass::undef;
}

// given Expr::_Apply (a function call / a variable call), determine whether it's <, or >, or similar
// (an expression `1 < 2` is expressed as `_<_(1,2)`, see builtins.cpp)
static bool is_comparison_binary_op(const Expr* e_apply) {
  const std::string& name = e_apply->sym->name();
  const size_t len = name.size();
  if (len < 3 || len > 5 || name[0] != '_' || name[len-1] != '_') {
    return false; // not "_<_" and similar
  }

  char c1 = name[1];
  char c2 = name[2];
  // < > <= != == >= <=>
  return (len == 3 && (c1 == '<' || c1 == '>')) ||
         (len == 4 && (c1 == '<' || c1 == '>' || c1 == '!' || c1 == '=') && c2 == '=') ||
         (len == 5 && (c1 == '<' && c2 == '=' && name[3] == '>'));
}

// same as above, but to detect bitwise operators: & | ^
// (in Tolk, they are used as logical ones due to absence of a boolean type and && || operators)
static bool is_bitwise_binary_op(const Expr* e_apply) {
  const std::string& name = e_apply->sym->name();
  const size_t len = name.size();
  if (len != 3 || name[0] != '_' || name[len-1] != '_') {
    return false;
  }

  char c1 = name[1];
  return c1 == '&' || c1 == '|' || c1 == '^';
}

// same as above, but to detect addition/subtraction
static bool is_add_or_sub_binary_op(const Expr* e_apply) {
  const std::string& name = e_apply->sym->name();
  const size_t len = name.size();
  if (len != 3 || name[0] != '_' || name[len-1] != '_') {
    return false;
  }

  char c1 = name[1];
  return c1 == '+' || c1 == '-';
}

static inline std::string get_builtin_operator_name(sym_idx_t sym_builtin) {
  std::string underscored = G.symbols.get_name(sym_builtin);
  return underscored.substr(1, underscored.size() - 2);
}

// fire an error for a case "flags & 0xFF != 0" (equivalent to "flags & 1", probably unexpected)
// it would better be a warning, but we decided to make it a strict error
GNU_ATTRIBUTE_NORETURN GNU_ATTRIBUTE_COLD
static void fire_error_lower_precedence(SrcLocation loc, sym_idx_t op_lower, sym_idx_t op_higher) {
  std::string name_lower = get_builtin_operator_name(op_lower);
  std::string name_higher = get_builtin_operator_name(op_higher);
  throw ParseError(loc, name_lower + " has lower precedence than " + name_higher +
                                 ", probably this code won't work as you expected.  "
                                 "Use parenthesis: either (... " + name_lower + " ...) to evaluate it first, or (... " + name_higher + " ...) to suppress this error.");
}

// fire an error for a case "arg1 & arg2 | arg3"
GNU_ATTRIBUTE_NORETURN GNU_ATTRIBUTE_COLD
static void fire_error_mix_bitwise_and_or(SrcLocation loc, sym_idx_t op1, sym_idx_t op2) {
  std::string name1 = get_builtin_operator_name(op1);
  std::string name2 = get_builtin_operator_name(op2);
  throw ParseError(loc, "mixing " + name1 + " with " + name2 + " without parenthesis"
                                 ", probably this code won't work as you expected.  "
                                 "Use parenthesis to emphasize operator precedence.");
}

// diagnose when bitwise operators are used in a probably wrong way due to tricky precedence
// example: "flags & 0xFF != 0" is equivalent to "flags & 1", most likely it's unexpected
// the only way to suppress this error for the programmer is to use parenthesis
static void diagnose_bitwise_precedence(SrcLocation loc, sym_idx_t bitwise_sym, const Expr* lhs, const Expr* rhs) {
  // handle "0 != flags & 0xFF" (lhs = "0 != flags")
  if (!lhs->is_inside_parenthesis() &&
    lhs->cls == Expr::_Apply && lhs->e_type->is_int() &&  // fast false if 100% not
    is_comparison_binary_op(lhs)) {
    fire_error_lower_precedence(loc, bitwise_sym, lhs->sym->sym_idx);
    // there is a tiny bug: "flags & _!=_(0xFF,0)" will also suggest to wrap rhs into parenthesis
  }

  // handle "flags & 0xFF != 0" (rhs = "0xFF != 0")
  if (!rhs->is_inside_parenthesis() &&
    rhs->cls == Expr::_Apply && rhs->e_type->is_int() &&
    is_comparison_binary_op(rhs)) {
    fire_error_lower_precedence(loc, bitwise_sym, rhs->sym->sym_idx);
  }

  // handle "arg1 & arg2 | arg3" (lhs = "arg1 & arg2")
  if (!lhs->is_inside_parenthesis() &&
    lhs->cls == Expr::_Apply && lhs->e_type->is_int() &&
    is_bitwise_binary_op(lhs) &&
    lhs->sym->sym_idx != bitwise_sym) {
    fire_error_mix_bitwise_and_or(loc, lhs->sym->sym_idx, bitwise_sym);
  }
}

// diagnose "a << 8 + 1" (equivalent to "a << 9", probably unexpected)
static void diagnose_addition_in_bitshift(SrcLocation loc, sym_idx_t bitshift_sym, const Expr* rhs) {
  if (!rhs->is_inside_parenthesis() &&
    rhs->cls == Expr::_Apply && rhs->e_type->is_int() &&
    is_add_or_sub_binary_op(rhs)) {
    fire_error_lower_precedence(loc, bitshift_sym, rhs->sym->sym_idx);
  }
}

/*
 *
 *   PARSE SOURCE
 *
 */

// TE ::= TA | TA -> TE
// TA ::= int | ... | cont | var | _ | () | ( TE { , TE } ) | [ TE { , TE } ]
TypeExpr* parse_type(Lexer& lex);

TypeExpr* parse_type1(Lexer& lex) {
  switch (lex.tok()) {
    case tok_int:
      lex.next();
      return TypeExpr::new_atomic(TypeExpr::_Int);
    case tok_cell:
      lex.next();
      return TypeExpr::new_atomic(TypeExpr::_Cell);
    case tok_slice:
      lex.next();
      return TypeExpr::new_atomic(TypeExpr::_Slice);
    case tok_builder:
      lex.next();
      return TypeExpr::new_atomic(TypeExpr::_Builder);
    case tok_cont:
      lex.next();
      return TypeExpr::new_atomic(TypeExpr::_Cont);
    case tok_tuple:
      lex.next();
      return TypeExpr::new_atomic(TypeExpr::_Tuple);
    case tok_var:
    case tok_underscore:
      lex.next();
      return TypeExpr::new_hole();
    case tok_identifier: {
      auto sym = lookup_symbol(lex.cur_sym_idx());
      if (sym && dynamic_cast<SymValType*>(sym->value)) {
        auto val = dynamic_cast<SymValType*>(sym->value);
        lex.next();
        return val->get_type();
      }
      lex.error_at("`", "` is not a type identifier");
    }
    default:
      break;
  }
  TokenType c;
  if (lex.tok() == tok_opbracket) {
    lex.next();
    c = tok_clbracket;
  } else {
    lex.expect(tok_oppar, "<type>");
    c = tok_clpar;
  }
  if (lex.tok() == c) {
    lex.next();
    return c == tok_clpar ? TypeExpr::new_unit() : TypeExpr::new_tuple({});
  }
  auto t1 = parse_type(lex);
  if (lex.tok() == tok_clpar) {
    lex.expect(c, c == tok_clpar ? "')'" : "']'");
    return t1;
  }
  std::vector<TypeExpr*> tlist{1, t1};
  while (lex.tok() == tok_comma) {
    lex.next();
    tlist.push_back(parse_type(lex));
  }
  lex.expect(c, c == tok_clpar ? "')'" : "']'");
  return c == tok_clpar ? TypeExpr::new_tensor(std::move(tlist)) : TypeExpr::new_tuple(std::move(tlist));
}

TypeExpr* parse_type(Lexer& lex) {
  auto res = parse_type1(lex);
  if (lex.tok() == tok_mapsto) {
    lex.next();
    auto to = parse_type(lex);
    return TypeExpr::new_map(res, to);
  } else {
    return res;
  }
}

FormalArg parse_formal_arg(Lexer& lex, int fa_idx) {
  TypeExpr* arg_type = 0;
  SrcLocation loc = lex.cur_location();
  if (lex.tok() == tok_underscore) {
    lex.next();
    if (lex.tok() == tok_comma || lex.tok() == tok_clpar) {
      return std::make_tuple(TypeExpr::new_hole(), (SymDef*)nullptr, loc);
    }
    arg_type = TypeExpr::new_hole();
    loc = lex.cur_location();
  } else if (lex.tok() != tok_identifier) {
    arg_type = parse_type(lex);
  } else {
    auto sym = lookup_symbol(lex.cur_sym_idx());
    if (sym && dynamic_cast<SymValType*>(sym->value)) {
      auto val = dynamic_cast<SymValType*>(sym->value);
      lex.next();
      arg_type = val->get_type();
    } else {
      arg_type = TypeExpr::new_hole();
    }
  }
  if (lex.tok() == tok_underscore || lex.tok() == tok_comma || lex.tok() == tok_clpar) {
    if (lex.tok() == tok_underscore) {
      loc = lex.cur_location();
      lex.next();
    }
    return std::make_tuple(arg_type, (SymDef*)nullptr, loc);
  }
  lex.check(tok_identifier, "formal parameter name");
  loc = lex.cur_location();
  if (G.prohibited_var_names.count(G.symbols.get_name(lex.cur_sym_idx()))) {
    throw ParseError{
        loc, PSTRING() << "symbol `" << G.symbols.get_name(lex.cur_sym_idx()) << "` cannot be redefined as a variable"};
  }
  SymDef* new_sym_def = define_symbol(lex.cur_sym_idx(), true, loc);
  if (!new_sym_def) {
    lex.error_at("cannot define symbol `", "`");
  }
  if (new_sym_def->value) {
    lex.error_at("redefined formal parameter `", "`");
  }
  new_sym_def->value = new SymVal{SymValKind::_Param, fa_idx, arg_type};
  lex.next();
  return std::make_tuple(arg_type, new_sym_def, loc);
}

void parse_global_var_decl(Lexer& lex) {
  TypeExpr* var_type = 0;
  SrcLocation loc = lex.cur_location();
  if (lex.tok() == tok_underscore) {
    lex.next();
    var_type = TypeExpr::new_hole();
    loc = lex.cur_location();
  } else if (lex.tok() != tok_identifier) {
    var_type = parse_type(lex);
  } else {
    auto sym = lookup_symbol(lex.cur_sym_idx());
    if (sym && dynamic_cast<SymValType*>(sym->value)) {
      auto val = dynamic_cast<SymValType*>(sym->value);
      lex.next();
      var_type = val->get_type();
    } else {
      var_type = TypeExpr::new_hole();
    }
  }
  lex.check(tok_identifier, "global variable name");
  loc = lex.cur_location();
  SymDef* sym_def = define_global_symbol(lex.cur_sym_idx(), false, loc);
  if (!sym_def) {
    lex.error_at("cannot define global symbol `", "`");
  }
  if (sym_def->value) {
    auto val = dynamic_cast<SymValGlobVar*>(sym_def->value);
    if (!val) {
      lex.error_at("symbol `", "` cannot be redefined as a global variable");
    }
    try {
      unify(var_type, val->sym_type);
    } catch (UnifyError& ue) {
      std::ostringstream os;
      os << "cannot unify new type " << var_type << " of global variable `" << sym_def->name()
         << "` with its previous type " << val->sym_type << ": " << ue;
      lex.error(os.str());
    }
  } else {
    sym_def->value = new SymValGlobVar{G.glob_var_cnt++, var_type};
#ifdef TOLK_DEBUG
    dynamic_cast<SymValGlobVar*>(sym_def->value)->name = lex.cur_str();
#endif
    G.glob_vars.push_back(sym_def);
  }
  lex.next();
}

Expr* parse_expr(Lexer& lex, CodeBlob& code, bool nv = false);

void parse_const_decl(Lexer& lex) {
  SrcLocation loc = lex.cur_location();
  int wanted_type = Expr::_None;
  if (lex.tok() == tok_int) {
    wanted_type = Expr::_Const;
    lex.next();
  } else if (lex.tok() == tok_slice) {
    wanted_type = Expr::_SliceConst;
    lex.next();
  }
  lex.check(tok_identifier, "constant name");
  loc = lex.cur_location();
  SymDef* sym_def = define_global_symbol(lex.cur_sym_idx(), false, loc);
  if (!sym_def) {
    lex.error_at("cannot define global symbol `", "`");
  }
  if (sym_def->value) { // todo below it was a check (for duplicate include?)
    lex.error_at("global symbol `", "` already exists");
  }
  lex.next();
  if (lex.tok() != tok_assign) {
    lex.error_at("expected = instead of ", "");
  }
  lex.next();
  CodeBlob code;
  // Handles processing and resolution of literals and consts
  auto x = parse_expr(lex, code, false); // also does lex.next() !
  if (!x->is_rvalue()) {
    lex.error("expression is not strictly Rvalue");
  }
  if ((wanted_type == Expr::_Const) && (x->cls == Expr::_Apply))
    wanted_type = Expr::_None; // Apply is additionally checked to result in an integer
  if ((wanted_type != Expr::_None) && (x->cls != wanted_type)) {
    lex.error("expression type does not match wanted type");
  }
  SymValConst* new_value = nullptr;
  if (x->cls == Expr::_Const) { // Integer constant
    new_value = new SymValConst{G.const_cnt++, x->intval};
  } else if (x->cls == Expr::_SliceConst) { // Slice constant (string)
    new_value = new SymValConst{G.const_cnt++, x->strval};
  } else if (x->cls == Expr::_Apply) {  // even "1 + 2" is Expr::_Apply (it applies `_+_`)
    code.emplace_back(loc, Op::_Import, std::vector<var_idx_t>());
    auto tmp_vars = x->pre_compile(code);
    code.emplace_back(loc, Op::_Return, std::move(tmp_vars));
    code.emplace_back(loc, Op::_Nop); // This is neccessary to prevent SIGSEGV!
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
      lex.error("precompiled expression must result in single operation");
    }
    auto op = out_list.list_[0];
    if (!op.is_const()) {
      lex.error("precompiled expression must result in compilation time constant");
    }
    if (op.origin.is_null() || !op.origin->is_valid()) {
      lex.error("precompiled expression did not result in a valid integer constant");
    }
    new_value = new SymValConst{G.const_cnt++, op.origin};
  } else {
    lex.error("integer or slice literal or constant expected");
  }
  sym_def->value = new_value;
}

FormalArgList parse_formal_args(Lexer& lex) {
  FormalArgList args;
  lex.expect(tok_oppar, "formal argument list");
  if (lex.tok() == tok_clpar) {
    lex.next();
    return args;
  }
  int fa_idx = 0;
  args.push_back(parse_formal_arg(lex, fa_idx++));
  while (lex.tok() == tok_comma) {
    lex.next();
    args.push_back(parse_formal_arg(lex, fa_idx++));
  }
  lex.expect(tok_clpar, "')'");
  return args;
}

void parse_const_decls(Lexer& lex) {
  lex.expect(tok_const, "'const'");
  while (true) {
    parse_const_decl(lex);
    if (lex.tok() != tok_comma) {
      break;
    }
    lex.expect(tok_comma, "','");
  }
  lex.expect(tok_semicolon, "';'");
}

TypeExpr* extract_total_arg_type(const FormalArgList& arg_list) {
  if (arg_list.empty()) {
    return TypeExpr::new_unit();
  }
  if (arg_list.size() == 1) {
    return std::get<0>(arg_list[0]);
  }
  std::vector<TypeExpr*> type_list;
  for (auto& x : arg_list) {
    type_list.push_back(std::get<0>(x));
  }
  return TypeExpr::new_tensor(std::move(type_list));
}

void parse_global_var_decls(Lexer& lex) {
  lex.expect(tok_global, "'global'");
  while (true) {
    parse_global_var_decl(lex);
    if (lex.tok() != tok_comma) {
      break;
    }
    lex.expect(tok_comma, "','");
  }
  lex.expect(tok_semicolon, "';'");
}

SymValCodeFunc* make_new_glob_func(SymDef* func_sym, TypeExpr* func_type, bool marked_as_pure) {
  SymValCodeFunc* res = new SymValCodeFunc{G.glob_func_cnt, func_type, marked_as_pure};
#ifdef TOLK_DEBUG
  res->name = func_sym->name();
#endif
  func_sym->value = res;
  G.glob_func.push_back(func_sym);
  G.glob_func_cnt++;
  return res;
}

bool check_global_func(const Lexer& lex, sym_idx_t func_name) {
  SymDef* def = lookup_symbol(func_name);
  if (!def) {
    lex.error("undefined symbol `" + G.symbols.get_name(func_name) + "`");
    return false;
  }
  SymVal* val = dynamic_cast<SymVal*>(def->value);
  if (!val) {
    lex.error(std::string{"symbol `"} + G.symbols.get_name(func_name) + "` has no value and no type");
    return false;
  } else if (!val->get_type()) {
    lex.error(std::string{"symbol `"} + G.symbols.get_name(func_name) + "` has no type, possibly not a function");
    return false;
  } else {
    return true;
  }
}

Expr* make_func_apply(Expr* fun, Expr* x) {
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

void check_import_exists_when_using_sym(const Lexer& lex, const SymDef* used_sym) {
  if (!lex.cur_location().is_symbol_from_same_or_builtin_file(used_sym->loc)) {
    const SrcFile* declared_in = used_sym->loc.get_src_file();
    bool has_import = false;
    for (const SrcFile::ImportStatement& import_stmt : lex.cur_file()->imports) {
      if (import_stmt.imported_file == declared_in) {
        has_import = true;
      }
    }
    if (!has_import) {
      lex.error("Using a non-imported symbol `" + used_sym->name() + "`. Forgot to import \"" + declared_in->rel_filename + "\"?");
    }
  }
}

// parse ( E { , E } ) | () | [ E { , E } ] | [] | id | num | _
Expr* parse_expr100(Lexer& lex, CodeBlob& code, bool nv) {
  if (lex.tok() == tok_oppar || lex.tok() == tok_opbracket) {
    bool tf = (lex.tok() == tok_opbracket);
    TokenType clbr = (tf ? tok_clbracket : tok_clpar);
    SrcLocation loc{lex.cur_location()};
    lex.next();
    if (lex.tok() == clbr) {
      lex.next();
      Expr* res = new Expr{Expr::_Tensor, {}};
      res->flags = Expr::_IsRvalue;
      res->here = loc;
      res->e_type = TypeExpr::new_unit();
      if (tf) {
        res = new Expr{Expr::_MkTuple, {res}};
        res->flags = Expr::_IsRvalue;
        res->here = loc;
        res->e_type = TypeExpr::new_tuple(res->args.at(0)->e_type);
      }
      return res;
    }
    Expr* res = parse_expr(lex, code, nv);
    if (lex.tok() == tok_clpar) {
      lex.expect(clbr, clbr == tok_clbracket ? "']'" : "')'");
      res->flags |= Expr::_IsInsideParenthesis;
      return res;
    }
    std::vector<TypeExpr*> type_list;
    type_list.push_back(res->e_type);
    int f = res->flags;
    res = new Expr{Expr::_Tensor, {res}};
    while (lex.tok() == tok_comma) {
      lex.next();
      auto x = parse_expr(lex, code, nv);
      res->pb_arg(x);
      if ((f ^ x->flags) & Expr::_IsType) {
        lex.error("mixing type and non-type expressions inside the same tuple");
      }
      f &= x->flags;
      type_list.push_back(x->e_type);
    }
    res->here = loc;
    res->flags = f;
    res->e_type = TypeExpr::new_tensor(std::move(type_list), !tf);
    if (tf) {
      res = new Expr{Expr::_MkTuple, {res}};
      res->flags = f;
      res->here = loc;
      res->e_type = TypeExpr::new_tuple(res->args.at(0)->e_type);
    }
    lex.expect(clbr, clbr == tok_clbracket ? "']'" : "')'");
    return res;
  }
  TokenType t = lex.tok();
  if (t == tok_int_const) {
    Expr* res = new Expr{Expr::_Const, lex.cur_location()};
    res->flags = Expr::_IsRvalue;
    res->intval = td::string_to_int256(lex.cur_str_std_string());
    if (res->intval.is_null() || !res->intval->signed_fits_bits(257)) {
      lex.error_at("invalid integer constant `", "`");
    }
    res->e_type = TypeExpr::new_atomic(TypeExpr::_Int);
    lex.next();
    return res;
  }
  if (t == tok_string_const) {
    std::string str = lex.cur_str_std_string();
    lex.next();
    char modifier = 0;
    if (lex.tok() == tok_string_modifier) {
      modifier = lex.cur_str()[0];
      lex.next();
    }
    Expr* res;
    switch (modifier) {
      case 0:
      case 's':
      case 'a':
        res = new Expr{Expr::_SliceConst, lex.cur_location()};
        res->e_type = TypeExpr::new_atomic(TypeExpr::_Slice);
        break;
      case 'u':
      case 'h':
      case 'H':
      case 'c':
        res = new Expr{Expr::_Const, lex.cur_location()};
        res->e_type = TypeExpr::new_atomic(TypeExpr::_Int);
        break;
      default:
        lex.error("invalid string type `" + std::string(1, modifier) + "`");
    }
    res->flags = Expr::_IsRvalue;
    switch (modifier) {
      case 0: {
        res->strval = td::hex_encode(str);
        break;
      }
      case 's': {
        res->strval = str;
        unsigned char buff[128];
        int bits = (int)td::bitstring::parse_bitstring_hex_literal(buff, sizeof(buff), str.data(), str.data() + str.size());
        if (bits < 0) {
          lex.error_at("Invalid hex bitstring constant `", "`");
        }
        break;
      }
      case 'a': {  // MsgAddressInt
        // todo rewrite stdaddress parsing (if done, CMake dep "ton_crypto" can be replaced with "ton_crypto_core")
        block::StdAddress a;
        if (a.parse_addr(str)) {
          res->strval = block::tlb::MsgAddressInt().pack_std_address(a)->as_bitslice().to_hex();
        } else {
          lex.error_at("invalid standard address `", "`");
        }
        break;
      }
      case 'u': {
        res->intval = td::hex_string_to_int256(td::hex_encode(str));
        if (str.empty()) {
          lex.error("empty integer ascii-constant");
        }
        if (res->intval.is_null()) {
          lex.error_at("too long integer ascii-constant `", "`");
        }
        break;
      }
      case 'h':
      case 'H': {
        unsigned char hash[32];
        digest::hash_str<digest::SHA256>(hash, str.data(), str.size());
        res->intval = td::bits_to_refint(hash, (modifier == 'h') ? 32 : 256, false);
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
  if (t == tok_underscore) {
    Expr* res = new Expr{Expr::_Hole, lex.cur_location()};
    res->val = -1;
    res->flags = Expr::_IsLvalue;
    res->e_type = TypeExpr::new_hole();
    lex.next();
    return res;
  }
  if (t == tok_var) {
    Expr* res = new Expr{Expr::_Type, lex.cur_location()};
    res->flags = Expr::_IsType;
    res->e_type = TypeExpr::new_hole();
    lex.next();
    return res;
  }
  if (t == tok_int || t == tok_cell || t == tok_slice || t == tok_builder || t == tok_cont || t == tok_type || t == tok_tuple) {
    Expr* res = new Expr{Expr::_Type, lex.cur_location()};
    res->flags = Expr::_IsType;
    res->e_type = TypeExpr::new_atomic(t);
    lex.next();
    return res;
  }
  if (t == tok_identifier) {
    auto sym = lookup_symbol(lex.cur_sym_idx());
    if (sym && dynamic_cast<SymValType*>(sym->value)) {
      auto val = dynamic_cast<SymValType*>(sym->value);
      Expr* res = new Expr{Expr::_Type, lex.cur_location()};
      res->flags = Expr::_IsType;
      res->e_type = val->get_type();
      lex.next();
      return res;
    }
    if (sym && dynamic_cast<SymValGlobVar*>(sym->value)) {
      check_import_exists_when_using_sym(lex, sym);
      auto val = dynamic_cast<SymValGlobVar*>(sym->value);
      Expr* res = new Expr{Expr::_GlobVar, lex.cur_location()};
      res->e_type = val->get_type();
      res->sym = sym;
      res->flags = Expr::_IsLvalue | Expr::_IsRvalue | Expr::_IsImpure;
      lex.next();
      return res;
    }
    if (sym && dynamic_cast<SymValConst*>(sym->value)) {
      check_import_exists_when_using_sym(lex, sym);
      auto val = dynamic_cast<SymValConst*>(sym->value);
      Expr* res = new Expr{Expr::_None, lex.cur_location()};
      res->flags = Expr::_IsRvalue;
      if (val->get_kind() == SymValConst::IntConst) {
        res->cls = Expr::_Const;
        res->intval = val->get_int_value();
        res->e_type = TypeExpr::new_atomic(tok_int);
      }
      else if (val->get_kind() == SymValConst::SliceConst) {
        res->cls = Expr::_SliceConst;
        res->strval = val->get_str_value();
        res->e_type = TypeExpr::new_atomic(tok_slice);
      }
      else {
        lex.error("Invalid symbolic constant type");
      }
      lex.next();
      return res;
    }
    if (sym && dynamic_cast<SymValFunc*>(sym->value)) {
      check_import_exists_when_using_sym(lex, sym);
    }
    bool auto_apply = false;
    Expr* res = new Expr{Expr::_Var, lex.cur_location()};
    if (nv) {
      res->val = ~lex.cur_sym_idx();
      res->e_type = TypeExpr::new_hole();
      res->flags = Expr::_IsLvalue;
      // std::cerr << "defined new variable " << lex.cur().str << " : " << res->e_type << std::endl;
    } else {
      if (!sym) {
        check_global_func(lex, lex.cur_sym_idx());
        sym = lookup_symbol(lex.cur_sym_idx());
      }
      res->sym = sym;
      SymVal* val = nullptr;
      bool impure = false;
      if (sym) {
        val = dynamic_cast<SymVal*>(sym->value);
      }
      if (!val) {
        lex.error_at("undefined identifier `", "`");
      } else if (val->kind == SymValKind::_Func) {
        res->e_type = val->get_type();
        res->cls = Expr::_GlobFunc;
        auto_apply = val->auto_apply;
        impure = !dynamic_cast<SymValFunc*>(val)->is_marked_as_pure();
      } else if (val->idx < 0) {
        lex.error_at("accessing variable `", "` being defined");
      } else {
        res->val = val->idx;
        res->e_type = val->get_type();
        // std::cerr << "accessing variable " << lex.cur().str << " : " << res->e_type << std::endl;
      }
      // std::cerr << "accessing symbol " << lex.cur().str << " : " << res->e_type << (val->impure ? " (impure)" : " (pure)") << std::endl;
      res->flags = Expr::_IsLvalue | Expr::_IsRvalue | (impure ? Expr::_IsImpure : 0);
    }
    if (auto_apply) {
      int impure = res->flags & Expr::_IsImpure;
      delete res;
      res = new Expr{Expr::_Apply, sym, {}};
      res->flags = Expr::_IsRvalue | impure;
    }
    res->deduce_type(lex);
    lex.next();
    return res;
  }
  lex.expect(tok_identifier, "identifier");
  return nullptr;
}

// parse E { E }
Expr* parse_expr90(Lexer& lex, CodeBlob& code, bool nv) {
  Expr* res = parse_expr100(lex, code, nv);
  while (lex.tok() == tok_oppar || lex.tok() == tok_opbracket || (lex.tok() == tok_identifier && !is_special_ident(lex.cur_sym_idx()))) {
    if (res->is_type()) {
      Expr* x = parse_expr100(lex, code, true);
      x->chk_lvalue(lex);  // chk_lrvalue() ?
      TypeExpr* tp = res->e_type;
      delete res;
      res = new Expr{Expr::_TypeApply, {x}};
      res->e_type = tp;
      res->here = lex.cur_location();
      try {
        unify(res->e_type, x->e_type);
      } catch (UnifyError& ue) {
        std::ostringstream os;
        os << "cannot transform expression of type " << x->e_type << " to explicitly requested type " << res->e_type
           << ": " << ue;
        lex.error(os.str());
      }
      res->flags = x->flags;
    } else {
      Expr* x = parse_expr100(lex, code, false);
      x->chk_rvalue(lex);
      res = make_func_apply(res, x);
      res->here = lex.cur_location();
      res->deduce_type(lex);
    }
  }
  return res;
}

// parse E { .method E | ~method E }
Expr* parse_expr80(Lexer& lex, CodeBlob& code, bool nv) {
  Expr* res = parse_expr90(lex, code, nv);
  while (lex.tok() == tok_identifier && is_special_ident(lex.cur_sym_idx())) {
    auto modify = is_tilde_ident(lex.cur_sym_idx());
    auto obj = res;
    if (modify) {
      obj->chk_lvalue(lex);
    } else {
      obj->chk_rvalue(lex);
    }
    SrcLocation loc = lex.cur_location();
    sym_idx_t name = lex.cur_sym_idx();
    auto sym = lookup_symbol(name);
    if (!sym || !dynamic_cast<SymValFunc*>(sym->value)) {
      auto name1 = G.symbols.lookup(lex.cur_str().substr(1));
      if (name1) {
        auto sym1 = lookup_symbol(name1);
        if (sym1 && dynamic_cast<SymValFunc*>(sym1->value)) {
          name = name1;
          sym = sym1;
        }
      }
    }
    check_global_func(lex, name);
    if (G.is_verbosity(2)) {
      std::cerr << "using symbol `" << G.symbols.get_name(name) << "` for method call of " << lex.cur_str() << std::endl;
    }
    sym = lookup_symbol(name);
    SymValFunc* val = sym ? dynamic_cast<SymValFunc*>(sym->value) : nullptr;
    if (!val) {
      lex.error_at("undefined method identifier `", "`");
    }
    lex.next();
    auto x = parse_expr100(lex, code, false);
    x->chk_rvalue(lex);
    if (x->cls == Expr::_Tensor) {
      res = new Expr{Expr::_Apply, name, {obj}};
      res->args.insert(res->args.end(), x->args.begin(), x->args.end());
    } else {
      res = new Expr{Expr::_Apply, name, {obj, x}};
    }
    res->here = loc;
    res->flags = Expr::_IsRvalue | (val->is_marked_as_pure() ? 0 : Expr::_IsImpure);
    res->deduce_type(lex);
    if (modify) {
      auto tmp = res;
      res = new Expr{Expr::_LetFirst, {obj->copy(), tmp}};
      res->here = loc;
      res->flags = tmp->flags;
      res->set_val(name);
      res->deduce_type(lex);
    }
  }
  return res;
}

// parse [ ~ | - | + ] E
Expr* parse_expr75(Lexer& lex, CodeBlob& code, bool nv) {
  if (lex.tok() == tok_bitwise_not || lex.tok() == tok_minus || lex.tok() == tok_plus) {
    TokenType t = lex.tok();
    sym_idx_t name = G.symbols.lookup_add(lex.cur_str_std_string() + "_");
    check_global_func(lex, name);
    SrcLocation loc{lex.cur_location()};
    lex.next();
    auto x = parse_expr75(lex, code, false);
    x->chk_rvalue(lex);

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
        lex.error("integer overflow");
      }
      return x;
    }

    auto res = new Expr{Expr::_Apply, name, {x}};
    res->here = loc;
    res->set_val(t);
    res->flags = Expr::_IsRvalue;
    res->deduce_type(lex);
    return res;
  } else {
    return parse_expr80(lex, code, nv);
  }
}

// parse E { (* | / | % | /% | ^/ | ~/ | ^% | ~% ) E }
Expr* parse_expr30(Lexer& lex, CodeBlob& code, bool nv) {
  Expr* res = parse_expr75(lex, code, nv);
  while (lex.tok() == tok_mul || lex.tok() == tok_div || lex.tok() == tok_mod || lex.tok() == tok_divmod || lex.tok() == tok_divC ||
         lex.tok() == tok_divR || lex.tok() == tok_modC || lex.tok() == tok_modR) {
    res->chk_rvalue(lex);
    TokenType t = lex.tok();
    sym_idx_t name = G.symbols.lookup_add(std::string{"_"} + lex.cur_str_std_string() + "_");
    SrcLocation loc{lex.cur_location()};
    check_global_func(lex, name);
    lex.next();
    auto x = parse_expr75(lex, code, false);
    x->chk_rvalue(lex);
    res = new Expr{Expr::_Apply, name, {res, x}};
    res->here = loc;
    res->set_val(t);
    res->flags = Expr::_IsRvalue;
    res->deduce_type(lex);
  }
  return res;
}

// parse E { (+ | -) E }
Expr* parse_expr20(Lexer& lex, CodeBlob& code, bool nv) {
  Expr* res = parse_expr30(lex, code, nv);
  while (lex.tok() == tok_minus || lex.tok() == tok_plus) {
    res->chk_rvalue(lex);
    TokenType t = lex.tok();
    sym_idx_t name = G.symbols.lookup_add(std::string{"_"} + lex.cur_str_std_string() + "_");
    check_global_func(lex, name);
    SrcLocation loc{lex.cur_location()};
    lex.next();
    auto x = parse_expr30(lex, code, false);
    x->chk_rvalue(lex);
    res = new Expr{Expr::_Apply, name, {res, x}};
    res->here = loc;
    res->set_val(t);
    res->flags = Expr::_IsRvalue;
    res->deduce_type(lex);
  }
  return res;
}

// parse E { ( << | >> | ~>> | ^>> ) E }
Expr* parse_expr17(Lexer& lex, CodeBlob& code, bool nv) {
  Expr* res = parse_expr20(lex, code, nv);
  while (lex.tok() == tok_lshift || lex.tok() == tok_rshift || lex.tok() == tok_rshiftC || lex.tok() == tok_rshiftR) {
    res->chk_rvalue(lex);
    TokenType t = lex.tok();
    sym_idx_t name = G.symbols.lookup_add(std::string{"_"} + lex.cur_str_std_string() + "_");
    check_global_func(lex, name);
    SrcLocation loc{lex.cur_location()};
    lex.next();
    auto x = parse_expr20(lex, code, false);
    x->chk_rvalue(lex);
    diagnose_addition_in_bitshift(loc, name, x);
    res = new Expr{Expr::_Apply, name, {res, x}};
    res->here = loc;
    res->set_val(t);
    res->flags = Expr::_IsRvalue;
    res->deduce_type(lex);
  }
  return res;
}

// parse E [ (== | < | > | <= | >= | != | <=> ) E ]
Expr* parse_expr15(Lexer& lex, CodeBlob& code, bool nv) {
  Expr* res = parse_expr17(lex, code, nv);
  if (lex.tok() == tok_eq || lex.tok() == tok_lt || lex.tok() == tok_gt || lex.tok() == tok_leq || lex.tok() == tok_geq ||
      lex.tok() == tok_neq || lex.tok() == tok_spaceship) {
    res->chk_rvalue(lex);
    TokenType t = lex.tok();
    sym_idx_t name = G.symbols.lookup_add(std::string{"_"} + lex.cur_str_std_string() + "_");
    check_global_func(lex, name);
    SrcLocation loc{lex.cur_location()};
    lex.next();
    auto x = parse_expr17(lex, code, false);
    x->chk_rvalue(lex);
    res = new Expr{Expr::_Apply, name, {res, x}};
    res->here = loc;
    res->set_val(t);
    res->flags = Expr::_IsRvalue;
    res->deduce_type(lex);
  }
  return res;
}

// parse E { ( & | `|` | ^ ) E }
Expr* parse_expr14(Lexer& lex, CodeBlob& code, bool nv) {
  Expr* res = parse_expr15(lex, code, nv);
  while (lex.tok() == tok_bitwise_and || lex.tok() == tok_bitwise_or || lex.tok() == tok_bitwise_xor) {
    res->chk_rvalue(lex);
    TokenType t = lex.tok();
    sym_idx_t name = G.symbols.lookup_add(std::string{"_"} + lex.cur_str_std_string() + "_");
    check_global_func(lex, name);
    SrcLocation loc{lex.cur_location()};
    lex.next();
    auto x = parse_expr15(lex, code, false);
    x->chk_rvalue(lex);
    // diagnose tricky bitwise precedence, like "flags & 0xFF != 0" (& has lower precedence)
    diagnose_bitwise_precedence(loc, name, res, x);

    res = new Expr{Expr::_Apply, name, {res, x}};
    res->here = loc;
    res->set_val(t);
    res->flags = Expr::_IsRvalue;
    res->deduce_type(lex);
  }
  return res;
}

// parse E [ ? E : E ]
Expr* parse_expr13(Lexer& lex, CodeBlob& code, bool nv) {
  Expr* res = parse_expr14(lex, code, nv);
  if (lex.tok() == tok_question) {
    res->chk_rvalue(lex);
    SrcLocation loc{lex.cur_location()};
    lex.next();
    auto x = parse_expr(lex, code, false);
    x->chk_rvalue(lex);
    lex.expect(tok_colon, "':'");
    auto y = parse_expr13(lex, code, false);
    y->chk_rvalue(lex);
    res = new Expr{Expr::_CondExpr, {res, x, y}};
    res->here = loc;
    res->flags = Expr::_IsRvalue;
    res->deduce_type(lex);
  }
  return res;
}

// parse LE1 (= | += | -= | ... ) E2
Expr* parse_expr10(Lexer& lex, CodeBlob& code, bool nv) {
  auto x = parse_expr13(lex, code, nv);
  TokenType t = lex.tok();
  if (t == tok_set_plus || t == tok_set_minus || t == tok_set_mul || t == tok_set_div || t == tok_set_divR || t == tok_set_divC ||
      t == tok_set_mod || t == tok_set_modC || t == tok_set_modR || t == tok_set_lshift || t == tok_set_rshift || t == tok_set_rshiftC ||
      t == tok_set_rshiftR || t == tok_set_bitwise_and || t == tok_set_bitwise_or || t == tok_set_bitwise_xor) {
    x->chk_lvalue(lex);
    x->chk_rvalue(lex);
    sym_idx_t name = G.symbols.lookup_add(std::string{"^_"} + lex.cur_str_std_string() + "_");
    check_global_func(lex, name);
    SrcLocation loc{lex.cur_location()};
    lex.next();
    auto y = parse_expr10(lex, code, false);
    y->chk_rvalue(lex);
    Expr* z = new Expr{Expr::_Apply, name, {x, y}};
    z->here = loc;
    z->set_val(t);
    z->flags = Expr::_IsRvalue;
    z->deduce_type(lex);
    Expr* res = new Expr{Expr::_Letop, {x->copy(), z}};
    res->here = loc;
    res->flags = (x->flags & ~Expr::_IsType) | Expr::_IsRvalue;
    res->set_val(t);
    res->deduce_type(lex);
    return res;
  } else if (t == tok_assign) {
    x->chk_lvalue(lex);
    SrcLocation loc{lex.cur_location()};
    lex.next();
    auto y = parse_expr10(lex, code, false);
    y->chk_rvalue(lex);
    x->predefine_vars();
    x->define_new_vars(code);
    Expr* res = new Expr{Expr::_Letop, {x, y}};
    res->here = loc;
    res->flags = (x->flags & ~Expr::_IsType) | Expr::_IsRvalue;
    res->set_val(t);
    res->deduce_type(lex);
    return res;
  } else {
    return x;
  }
}

Expr* parse_expr(Lexer& lex, CodeBlob& code, bool nv) {
  return parse_expr10(lex, code, nv);
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
}  // namespace blk_fl

blk_fl::val parse_return_stmt(Lexer& lex, CodeBlob& code) {
  auto expr = parse_expr(lex, code);
  expr->chk_rvalue(lex);
  try {
    // std::cerr << "in return: ";
    unify(expr->e_type, code.ret_type);
  } catch (UnifyError& ue) {
    std::ostringstream os;
    os << "previous function return type " << code.ret_type
       << " cannot be unified with return statement expression type " << expr->e_type << ": " << ue;
    lex.error(os.str());
  }
  std::vector<var_idx_t> tmp_vars = expr->pre_compile(code);
  code.emplace_back(lex.cur_location(), Op::_Return, std::move(tmp_vars));
  lex.expect(tok_semicolon, "';'");
  return blk_fl::ret;
}

blk_fl::val parse_implicit_ret_stmt(Lexer& lex, CodeBlob& code) {
  auto ret_type = TypeExpr::new_unit();
  try {
    // std::cerr << "in implicit return: ";
    unify(ret_type, code.ret_type);
  } catch (UnifyError& ue) {
    std::ostringstream os;
    os << "previous function return type " << code.ret_type
       << " cannot be unified with implicit end-of-block return type " << ret_type << ": " << ue;
    lex.error(os.str());
  }
  code.emplace_back(lex.cur_location(), Op::_Return);
  return blk_fl::ret;
}

blk_fl::val parse_stmt(Lexer& lex, CodeBlob& code);

blk_fl::val parse_block_stmt(Lexer& lex, CodeBlob& code, bool no_new_scope = false) {
  lex.expect(tok_opbrace, "'{'");
  if (!no_new_scope) {
    open_scope(lex.cur_location());
  }
  blk_fl::val res = blk_fl::init;
  bool warned = false;
  while (lex.tok() != tok_clbrace) {
    if (!(res & blk_fl::end) && !warned) {
      lex.cur_location().show_warning("unreachable code");
      warned = true;
    }
    blk_fl::combine(res, parse_stmt(lex, code));
  }
  if (!no_new_scope) {
    close_scope(lex.cur_location());
  }
  lex.expect(tok_clbrace, "'}'");
  return res;
}

blk_fl::val parse_repeat_stmt(Lexer& lex, CodeBlob& code) {
  SrcLocation loc{lex.cur_location()};
  lex.expect(tok_repeat, "'repeat'");
  auto expr = parse_expr(lex, code);
  expr->chk_rvalue(lex);
  auto cnt_type = TypeExpr::new_atomic(TypeExpr::_Int);
  try {
    unify(expr->e_type, cnt_type);
  } catch (UnifyError& ue) {
    std::ostringstream os;
    os << "repeat count value of type " << expr->e_type << " is not an integer: " << ue;
    lex.error(os.str());
  }
  std::vector<var_idx_t> tmp_vars = expr->pre_compile(code);
  if (tmp_vars.size() != 1) {
    lex.error("repeat count value is not a singleton");
  }
  Op& repeat_op = code.emplace_back(loc, Op::_Repeat, tmp_vars);
  code.push_set_cur(repeat_op.block0);
  blk_fl::val res = parse_block_stmt(lex, code);
  code.close_pop_cur(lex.cur_location());
  return res | blk_fl::end;
}

blk_fl::val parse_while_stmt(Lexer& lex, CodeBlob& code) {
  SrcLocation loc{lex.cur_location()};
  lex.expect(tok_while, "'while'");
  auto expr = parse_expr(lex, code);
  expr->chk_rvalue(lex);
  auto cnt_type = TypeExpr::new_atomic(TypeExpr::_Int);
  try {
    unify(expr->e_type, cnt_type);
  } catch (UnifyError& ue) {
    std::ostringstream os;
    os << "while condition value of type " << expr->e_type << " is not an integer: " << ue;
    lex.error(os.str());
  }
  Op& while_op = code.emplace_back(loc, Op::_While);
  code.push_set_cur(while_op.block0);
  while_op.left = expr->pre_compile(code);
  code.close_pop_cur(lex.cur_location());
  if (while_op.left.size() != 1) {
    lex.error("while condition value is not a singleton");
  }
  code.push_set_cur(while_op.block1);
  blk_fl::val res1 = parse_block_stmt(lex, code);
  code.close_pop_cur(lex.cur_location());
  return res1 | blk_fl::end;
}

blk_fl::val parse_do_stmt(Lexer& lex, CodeBlob& code) {
  Op& while_op = code.emplace_back(lex.cur_location(), Op::_Until);
  lex.expect(tok_do, "'do'");
  code.push_set_cur(while_op.block0);
  open_scope(lex.cur_location());
  blk_fl::val res = parse_block_stmt(lex, code, true);
  lex.expect(tok_until, "'until'");
  auto expr = parse_expr(lex, code);
  expr->chk_rvalue(lex);
  close_scope(lex.cur_location());
  auto cnt_type = TypeExpr::new_atomic(TypeExpr::_Int);
  try {
    unify(expr->e_type, cnt_type);
  } catch (UnifyError& ue) {
    std::ostringstream os;
    os << "`until` condition value of type " << expr->e_type << " is not an integer: " << ue;
    lex.error(os.str());
  }
  while_op.left = expr->pre_compile(code);
  code.close_pop_cur(lex.cur_location());
  if (while_op.left.size() != 1) {
    lex.error("`until` condition value is not a singleton");
  }
  return res & ~blk_fl::empty;
}

blk_fl::val parse_try_catch_stmt(Lexer& lex, CodeBlob& code) {
  code.require_callxargs = true;
  lex.expect(tok_try, "'try'");
  Op& try_catch_op = code.emplace_back(lex.cur_location(), Op::_TryCatch);
  code.push_set_cur(try_catch_op.block0);
  blk_fl::val res0 = parse_block_stmt(lex, code);
  code.close_pop_cur(lex.cur_location());
  lex.expect(tok_catch, "'catch'");
  code.push_set_cur(try_catch_op.block1);
  open_scope(lex.cur_location());
  Expr* expr = parse_expr(lex, code, true);
  expr->chk_lvalue(lex);
  TypeExpr* tvm_error_type = TypeExpr::new_tensor(TypeExpr::new_var(), TypeExpr::new_atomic(TypeExpr::_Int));
  try {
    unify(expr->e_type, tvm_error_type);
  } catch (UnifyError& ue) {
    std::ostringstream os;
    os << "`catch` arguments have incorrect type " << expr->e_type << ": " << ue;
    lex.error(os.str());
  }
  expr->predefine_vars();
  expr->define_new_vars(code);
  try_catch_op.left = expr->pre_compile(code);
  tolk_assert(try_catch_op.left.size() == 2 || try_catch_op.left.size() == 1);
  blk_fl::val res1 = parse_block_stmt(lex, code);
  close_scope(lex.cur_location());
  code.close_pop_cur(lex.cur_location());
  blk_fl::combine_parallel(res0, res1);
  return res0;
}

blk_fl::val parse_if_stmt(Lexer& lex, CodeBlob& code, TokenType first_lex = tok_if) {
  SrcLocation loc{lex.cur_location()};
  lex.next();
  auto expr = parse_expr(lex, code);
  expr->chk_rvalue(lex);
  auto flag_type = TypeExpr::new_atomic(TypeExpr::_Int);
  try {
    unify(expr->e_type, flag_type);
  } catch (UnifyError& ue) {
    std::ostringstream os;
    os << "`if` condition value of type " << expr->e_type << " is not an integer: " << ue;
    lex.error(os.str());
  }
  std::vector<var_idx_t> tmp_vars = expr->pre_compile(code);
  if (tmp_vars.size() != 1) {
    lex.error("condition value is not a singleton");
  }
  Op& if_op = code.emplace_back(loc, Op::_If, tmp_vars);
  code.push_set_cur(if_op.block0);
  blk_fl::val res1 = parse_block_stmt(lex, code);
  blk_fl::val res2 = blk_fl::init;
  code.close_pop_cur(lex.cur_location());
  if (lex.tok() == tok_else) {
    lex.expect(tok_else, "'else'");
    code.push_set_cur(if_op.block1);
    res2 = parse_block_stmt(lex, code);
    code.close_pop_cur(lex.cur_location());
  } else if (lex.tok() == tok_elseif || lex.tok() == tok_elseifnot) {
    code.push_set_cur(if_op.block1);
    res2 = parse_if_stmt(lex, code, lex.tok());
    code.close_pop_cur(lex.cur_location());
  } else {
    if_op.block1 = std::make_unique<Op>(lex.cur_location(), Op::_Nop);
  }
  if (first_lex == tok_ifnot || first_lex == tok_elseifnot) {
    std::swap(if_op.block0, if_op.block1);
  }
  blk_fl::combine_parallel(res1, res2);
  return res1;
}

blk_fl::val parse_stmt(Lexer& lex, CodeBlob& code) {
  switch (lex.tok()) {
    case tok_return: {
      lex.next();
      return parse_return_stmt(lex, code);
    }
    case tok_opbrace: {
      return parse_block_stmt(lex, code);
    }
    case tok_semicolon: {
      lex.next();
      return blk_fl::init;
    }
    case tok_repeat:
      return parse_repeat_stmt(lex, code);
    case tok_if:
    case tok_ifnot:
      return parse_if_stmt(lex, code, lex.tok());
    case tok_do:
      return parse_do_stmt(lex, code);
    case tok_while:
      return parse_while_stmt(lex, code);
    case tok_try:
      return parse_try_catch_stmt(lex, code);
    default: {
      auto expr = parse_expr(lex, code);
      expr->chk_rvalue(lex);
      expr->pre_compile(code);
      lex.expect(tok_semicolon, "';'");
      return blk_fl::end;
    }
  }
}

CodeBlob* parse_func_body(Lexer& lex, FormalArgList arg_list, TypeExpr* ret_type, bool marked_as_pure) {
  lex.expect(tok_opbrace, "'{'");
  CodeBlob* blob = new CodeBlob{ret_type};
  if (marked_as_pure) {
    blob->flags |= CodeBlob::_ForbidImpure;
  }
  blob->import_params(std::move(arg_list));
  blk_fl::val res = blk_fl::init;
  bool warned = false;
  while (lex.tok() != tok_clbrace) {
    if (!(res & blk_fl::end) && !warned) {
      lex.cur_location().show_warning("unreachable code");
      warned = true;
    }
    blk_fl::combine(res, parse_stmt(lex, *blob));
  }
  if (res & blk_fl::end) {
    parse_implicit_ret_stmt(lex, *blob);
  }
  blob->close_blk(lex.cur_location());
  lex.expect(tok_clbrace, "'}'");
  return blob;
}

SymValAsmFunc* parse_asm_func_body(Lexer& lex, TypeExpr* func_type, const FormalArgList& arg_list, TypeExpr* ret_type,
                                   bool marked_as_pure) {
  SrcLocation loc = lex.cur_location();
  lex.expect(tok_asm, "'asm'");
  int cnt = (int)arg_list.size();
  int width = ret_type->get_width();
  if (width < 0 || width > 16) {
    throw ParseError{loc, "return type of an assembler built-in function must have a well-defined fixed width"};
  }
  if (arg_list.size() > 16) {
    throw ParseError{loc, "assembler built-in function must have at most 16 arguments"};
  }
  std::vector<int> cum_arg_width;
  cum_arg_width.push_back(0);
  int tot_width = 0;
  for (auto& arg : arg_list) {
    int arg_width = std::get<TypeExpr*>(arg)->get_width();
    if (arg_width < 0 || arg_width > 16) {
      throw ParseError{std::get<SrcLocation>(arg),
                            "parameters of an assembler built-in function must have a well-defined fixed width"};
    }
    cum_arg_width.push_back(tot_width += arg_width);
  }
  std::vector<AsmOp> asm_ops;
  std::vector<int> arg_order, ret_order;
  if (lex.tok() == tok_oppar) {
    lex.next();
    if (lex.tok() != tok_mapsto) {
      std::vector<bool> visited(cnt, false);
      for (int i = 0; i < cnt; i++) {
        lex.check(tok_identifier, "identifier");
        auto sym = lookup_symbol(lex.cur_sym_idx());
        int j;
        for (j = 0; j < cnt; j++) {
          if (std::get<SymDef*>(arg_list[j]) == sym) {
            break;
          }
        }
        if (j == cnt) {
          lex.error("formal argument name expected");
        }
        if (visited[j]) {
          lex.error("formal argument listed twice");
        }
        visited[j] = true;
        int c1 = cum_arg_width[j], c2 = cum_arg_width[j + 1];
        while (c1 < c2) {
          arg_order.push_back(c1++);
        }
        lex.next();
      }
      tolk_assert(arg_order.size() == (unsigned)tot_width);
    }
    if (lex.tok() == tok_mapsto) {
      lex.next();
      std::vector<bool> visited(width, false);
      for (int i = 0; i < width; i++) {
        if (lex.tok() != tok_int_const || lex.cur_str().size() > 3) {
          lex.expect(tok_int_const, "number");
        }
        int j = atoi(lex.cur_str_std_string().c_str());
        if (j < 0 || j >= width || visited[j]) {
          lex.error("expected integer return value index 0 .. width-1");
        }
        visited[j] = true;
        ret_order.push_back(j);
        lex.next();
      }
    }
    lex.expect(tok_clpar, "')'");
  }
  while (lex.tok() == tok_string_const) {
    std::string ops = lex.cur_str_std_string(); // <op>\n<op>\n...
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
    lex.next();
  }
  if (asm_ops.empty()) {
    lex.error("string with assembler instruction expected");
  }
  lex.expect(tok_semicolon, "';'");
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

std::vector<TypeExpr*> parse_type_var_list(Lexer& lex) {
  std::vector<TypeExpr*> res;
  lex.expect(tok_forall, "'forall'");
  int idx = 0;
  while (true) {
    if (lex.tok() == tok_type) {
      lex.next();
    }
    if (lex.tok() != tok_identifier) {
      lex.error("free type identifier expected");
    }
    SrcLocation loc = lex.cur_location();
    if (G.prohibited_var_names.count(G.symbols.get_name(lex.cur_sym_idx()))) {
      throw ParseError{loc, PSTRING() << "symbol `" << G.symbols.get_name(lex.cur_sym_idx())
                                           << "` cannot be redefined as a variable"};
    }
    SymDef* new_sym_def = define_symbol(lex.cur_sym_idx(), true, loc);
    if (!new_sym_def || new_sym_def->value) {
      lex.error_at("redefined type variable `", "`");
    }
    auto var = TypeExpr::new_var(idx);
    new_sym_def->value = new SymValType{SymValKind::_Typename, idx++, var};
    res.push_back(var);
    lex.next();
    if (lex.tok() != tok_comma) {
      break;
    }
    lex.next();
  }
  lex.expect(tok_mapsto, "'->'");
  return res;
}

void type_var_usage(TypeExpr* expr, const std::vector<TypeExpr*>& typevars, std::vector<bool>& used) {
  if (expr->constr != TypeExpr::te_Var) {
    for (auto arg : expr->args) {
      type_var_usage(arg, typevars, used);
    }
    return;
  }
  for (std::size_t i = 0; i < typevars.size(); i++) {
    if (typevars[i] == expr) {
      used.at(i) = true;
      return;
    }
  }
  return;
}

TypeExpr* compute_type_closure(TypeExpr* expr, const std::vector<TypeExpr*>& typevars) {
  if (typevars.empty()) {
    return expr;
  }
  std::vector<bool> used(typevars.size(), false);
  type_var_usage(expr, typevars, used);
  std::vector<TypeExpr*> used_vars;
  for (std::size_t i = 0; i < typevars.size(); i++) {
    if (used.at(i)) {
      used_vars.push_back(typevars[i]);
    }
  }
  if (!used_vars.empty()) {
    expr = TypeExpr::new_forall(std::move(used_vars), expr);
  }
  return expr;
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

  bool indices_expected = static_cast<int>(op_import->left.size()) == op_call->left[0] && op_call->left[0] == op_return->left[0];
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
  if (v_current->get_arg_type()->get_width() != static_cast<int>(op_call->right.size()))
    return;
  // 'return true;' (false, nil) are (surprisingly) also function calls, with auto_apply=true
  if (v_called->auto_apply)
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

// todo rewrite function declaration parsing completely, it's weird
void parse_func_def(Lexer& lex) {
  SrcLocation loc = lex.cur_location();
  open_scope(loc);
  std::vector<TypeExpr*> type_vars;
  bool is_get_method = false;
  if (lex.tok() == tok_forall) {
    type_vars = parse_type_var_list(lex);
  } else if (lex.tok() == tok_get) {
    is_get_method = true;
    lex.next();
  }
  auto ret_type = parse_type(lex);
  if (lex.tok() != tok_identifier) {
    lex.error("function name identifier expected");
  }
  std::string func_name = lex.cur_str_std_string();
  int func_sym_idx = lex.cur_sym_idx();
  lex.next();
  FormalArgList arg_list = parse_formal_args(lex);
  bool marked_as_pure = false;
  if (lex.tok() == tok_impure) {
    static bool warning_shown = false;
    if (!warning_shown) {
      lex.cur_location().show_warning("`impure` specifier is deprecated. All functions are impure by default, use `pure` to mark a function as pure");
      warning_shown = true;
    }
    lex.next();
  } else if (lex.tok() == tok_pure) {
    marked_as_pure = true;
    lex.next();
  }
  int flags_inline = 0;
  if (lex.tok() == tok_inline) {
    flags_inline = SymValFunc::flagInline;
    lex.next();
  } else if (lex.tok() == tok_inlineref) {
    flags_inline = SymValFunc::flagInlineRef;
    lex.next();
  }
  td::RefInt256 method_id;
  if (lex.tok() == tok_method_id) {
    if (is_get_method) {
      lex.error("both `get` and `method_id` are not allowed");
    }
    lex.next();
    if (lex.tok() == tok_oppar) {  // method_id(N)
      lex.next();
      method_id = td::string_to_int256(lex.cur_str_std_string());
      lex.expect(tok_int_const, "number");
      if (method_id.is_null()) {
        lex.error_at("invalid integer constant `", "`");
      }
      lex.expect(tok_clpar, "')'");
    } else {
      static bool warning_shown = false;
      if (!warning_shown) {
        lex.cur_location().show_warning("`method_id` specifier is deprecated, use `get` keyword.\nExample: `get int seqno() { ... }`");
        warning_shown = true;
      }
      method_id = calculate_method_id_by_func_name(func_name);
    }
  }
  if (is_get_method) {
    tolk_assert(method_id.is_null());
    method_id = calculate_method_id_by_func_name(func_name);
    for (const SymDef* other : G.glob_get_methods) {
      if (!td::cmp(dynamic_cast<const SymValFunc*>(other->value)->method_id, method_id)) {
        lex.error(PSTRING() << "GET methods hash collision: `" << other->name() << "` and `" + func_name + "` produce the same hash. Consider renaming one of these functions.");
      }
    }
  }
  TypeExpr* func_type = TypeExpr::new_map(extract_total_arg_type(arg_list), ret_type);
  func_type = compute_type_closure(func_type, type_vars);
  if (lex.tok() == tok_builtin) {
    const SymDef* builtin_func = lookup_symbol(G.symbols.lookup(func_name));
    const SymValFunc* func_val = builtin_func ? dynamic_cast<SymValFunc*>(builtin_func->value) : nullptr;
    if (!func_val || !func_val->is_builtin()) {
      lex.error("`builtin` used for non-builtin function");
    }
#ifdef TOLK_DEBUG
    // in release, we don't need this check, since `builtin` is used only in stdlib.tolk, which is our responsibility
    if (!func_val->sym_type->equals_to(func_type) || func_val->is_marked_as_pure() != marked_as_pure) {
      lex.error("declaration for `builtin` function doesn't match an actual one");
    }
#endif
    lex.next();
    lex.expect(tok_semicolon, "';'");
    close_scope(lex.cur_location());
    return;
  }
  if (lex.tok() != tok_semicolon && lex.tok() != tok_opbrace && lex.tok() != tok_asm) {
    lex.expect(tok_opbrace, "function body block");
  }
  if (G.is_verbosity(1)) {
    std::cerr << "function " << func_name << " : " << func_type << std::endl;
  }
  SymDef* func_sym = define_global_symbol(func_sym_idx, 0, loc);
  tolk_assert(func_sym);
  SymValFunc* func_sym_val = dynamic_cast<SymValFunc*>(func_sym->value);
  if (func_sym->value) {
    if (func_sym->value->kind != SymValKind::_Func || !func_sym_val) {
      lex.error("was not defined as a function before");
    }
    try {
      unify(func_sym_val->sym_type, func_type);
    } catch (UnifyError& ue) {
      std::ostringstream os;
      os << "previous type of function " << func_name << " : " << func_sym_val->sym_type
         << " cannot be unified with new type " << func_type << ": " << ue;
      lex.error(os.str());
    }
  }
  if (lex.tok() == tok_semicolon) {
    make_new_glob_func(func_sym, func_type, marked_as_pure);
    lex.next();
  } else if (lex.tok() == tok_opbrace) {
    if (dynamic_cast<SymValAsmFunc*>(func_sym_val)) {
      lex.error("function `" + func_name + "` has been already defined as an assembler built-in");
    }
    SymValCodeFunc* func_sym_code;
    if (func_sym_val) {
      func_sym_code = dynamic_cast<SymValCodeFunc*>(func_sym_val);
      if (!func_sym_code) {
        lex.error("function `" + func_name + "` has been already defined in an yet-unknown way");
      }
    } else {
      func_sym_code = make_new_glob_func(func_sym, func_type, marked_as_pure);
    }
    if (func_sym_code->code) {
      lex.error("redefinition of function `"s + func_name + "`");
    }
    if (marked_as_pure && ret_type->get_width() == 0) {
      lex.error("a pure function should return something, otherwise it will be optimized out anyway");
    }
    CodeBlob* code = parse_func_body(lex, arg_list, ret_type, marked_as_pure);
    code->name = func_name;
    code->loc = loc;
    // code->print(std::cerr);  // !!!DEBUG!!!
    func_sym_code->code = code;
    detect_if_function_just_wraps_another(func_sym_code, method_id);
  } else {
    SrcLocation asm_location = lex.cur_location();
    SymValAsmFunc* asm_func = parse_asm_func_body(lex, func_type, arg_list, ret_type, marked_as_pure);
#ifdef TOLK_DEBUG
    asm_func->name = func_name;
#endif
    if (func_sym_val) {
      if (dynamic_cast<SymValCodeFunc*>(func_sym_val)) {
        throw ParseError(asm_location, "function `" + func_name + "` was already declared as an ordinary function");
      }
      SymValAsmFunc* asm_func_old = dynamic_cast<SymValAsmFunc*>(func_sym_val);
      if (asm_func_old) {
        if (asm_func->crc != asm_func_old->crc) {
          throw ParseError(asm_location, "redefinition of built-in assembler function `" + func_name + "`");
        }
      } else {
        throw ParseError(asm_location, "redefinition of previously (somehow) defined function `" + func_name + "`");
      }
    }
    func_sym->value = asm_func;
  }
  if (method_id.not_null()) {
    auto val = dynamic_cast<SymValFunc*>(func_sym->value);
    if (!val) {
      lex.error("cannot set method id for unknown function `" + func_name + "`");
    }
    if (val->method_id.is_null()) {
      val->method_id = std::move(method_id);
    } else if (td::cmp(val->method_id, method_id) != 0) {
      lex.error("integer method identifier for `" + func_name + "` changed from " +
                      val->method_id->to_dec_string() + " to a different value " + method_id->to_dec_string());
    }
  }
  if (flags_inline) {
    auto val = dynamic_cast<SymValFunc*>(func_sym->value);
    if (!val) {
      lex.error("cannot set unknown function `" + func_name + "` as an inline");
    }
    if (!val->is_inline() && !val->is_inline_ref()) {
      val->flags |= flags_inline;
    } else if ((val->flags & (SymValFunc::flagInline | SymValFunc::flagInlineRef)) != flags_inline) {
      lex.error("inline mode for `" + func_name + "` changed with respect to a previous declaration");
    }
  }
  if (is_get_method) {
    auto val = dynamic_cast<SymValFunc*>(func_sym->value);
    if (!val) {
      lex.error("cannot set unknown function `" + func_name + "` as a get method");
    }
    val->flags |= SymValFunc::flagGetMethod;
    G.glob_get_methods.push_back(func_sym);
  }
  if (G.is_verbosity(1)) {
    std::cerr << "new type of function " << func_name << " : " << func_type << std::endl;
  }
  close_scope(lex.cur_location());
}

void parse_pragma(Lexer& lex) {
  SrcLocation loc = lex.cur_location();
  lex.next_special(tok_pragma_name, "pragma name");
  std::string_view pragma_name = lex.cur_str();
  if (pragma_name == "version") {
    lex.next();
    TokenType cmp_tok = lex.tok();
    char op = '='; bool eq = false;
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
      lex.error("invalid comparison operator");
    }
    lex.next_special(tok_semver, "semver");
    std::string_view pragma_value = lex.cur_str();
    int sem_ver[3] = {0, 0, 0};
    char segs = 1;
    auto stoi = [&](std::string_view s) {
      auto R = td::to_integer_safe<int>(static_cast<std::string>(s));
      if (R.is_error()) {
        lex.error("invalid semver format");
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
      throw ParseError(loc, std::string("Tolk version ") + tolk_version + " does not satisfy this condition");
    }
  } else if (pragma_name == G.pragma_allow_post_modification.name()) {
    G.pragma_allow_post_modification.enable(loc);
  } else if (pragma_name == G.pragma_compute_asm_ltr.name()) {
    G.pragma_compute_asm_ltr.enable(loc);
  } else if (pragma_name == G.pragma_remove_unused_functions.name()) {
    G.pragma_remove_unused_functions.enable(loc);
  } else {
    lex.error("unknown pragma name");
  }
  lex.next();
  lex.expect(tok_semicolon, "';'");
}

void parse_include(Lexer& lex, SrcFile* parent_file) {
  SrcLocation loc = lex.cur_location();
  lex.expect(tok_include, "#include");
  if (lex.tok() != tok_string_const) {
    lex.expect(tok_string_const, "source file name");
  }
  std::string rel_filename = lex.cur_str_std_string();
  if (rel_filename.empty()) {
    lex.error("imported file name is an empty string");
  }
  if (size_t rc = parent_file->rel_filename.rfind('/'); rc != std::string::npos) {
    rel_filename = parent_file->rel_filename.substr(0, rc + 1) + rel_filename;
  }
  lex.next();
  lex.expect(tok_semicolon, "';'");

  td::Result<SrcFile*> locate_res = locate_source_file(rel_filename);
  if (locate_res.is_error()) {
    throw ParseError(loc, "Failed to import: " + locate_res.move_as_error().message().str());
  }

  SrcFile* imported_file = locate_res.move_as_ok();
  parent_file->imports.emplace_back(SrcFile::ImportStatement{imported_file});
  if (!imported_file->was_parsed) {
    parse_source_file(imported_file);
  }
}

// this function either throws (on any error) or returns nothing meaning success (filling global variables)
void parse_source_file(SrcFile* file) {
  if (!file->is_stdlib_file()) {
    G.generated_from += file->rel_filename;
    G.generated_from += ", ";
  }
  file->was_parsed = true;

  Lexer lex(file);
  while (!lex.is_eof()) {
    if (lex.tok() == tok_pragma) {
      parse_pragma(lex);
    } else if (lex.tok() == tok_include) {
      parse_include(lex, file);
    } else if (lex.tok() == tok_global) {
      parse_global_var_decls(lex);
    } else if (lex.tok() == tok_const) {
      parse_const_decls(lex);
    } else {
      parse_func_def(lex);
    }
  }
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

}  // namespace tolk
