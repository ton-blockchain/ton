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
#include "ast-from-tokens.h"
#include "ast.h"
#include "platform-utils.h"
#include "type-expr.h"
#include "tolk-version.h"

/*
 *   Here we construct AST for a tolk file.
 *   While constructing, no global state is modified.
 *   Historically, in FunC, there was no AST: while lexing, symbols were registered, types were inferred, and so on.
 * There was no way to perform any more or less semantic analysis.
 *   Implementing AST gives a giant advance for future modifications and stability.
 */

namespace tolk {

// given a token, determine whether it's <, or >, or similar
static bool is_comparison_binary_op(TokenType tok) {
  return tok == tok_lt || tok == tok_gt || tok == tok_leq || tok == tok_geq || tok == tok_eq || tok == tok_neq || tok == tok_spaceship;
}

// same as above, but to detect bitwise operators: & | ^
static bool is_bitwise_binary_op(TokenType tok) {
  return tok == tok_bitwise_and || tok == tok_bitwise_or || tok == tok_bitwise_xor;
}

// same as above, but to detect logical operators: && ||
static bool is_logical_binary_op(TokenType tok) {
  return tok == tok_logical_and || tok == tok_logical_or;
}

// same as above, but to detect addition/subtraction
static bool is_add_or_sub_binary_op(TokenType tok) {
  return tok == tok_plus || tok == tok_minus;
}

// fire an error for a case "flags & 0xFF != 0" (equivalent to "flags & 1", probably unexpected)
// it would better be a warning, but we decided to make it a strict error
GNU_ATTRIBUTE_NORETURN GNU_ATTRIBUTE_COLD
static void fire_error_lower_precedence(SrcLocation loc, std::string_view op_lower, std::string_view op_higher) {
  std::string name_lower = static_cast<std::string>(op_lower);
  std::string name_higher = static_cast<std::string>(op_higher);
  throw ParseError(loc, name_lower + " has lower precedence than " + name_higher +
                                 ", probably this code won't work as you expected.  "
                                 "Use parenthesis: either (... " + name_lower + " ...) to evaluate it first, or (... " + name_higher + " ...) to suppress this error.");
}

// fire an error for a case "arg1 & arg2 | arg3"
GNU_ATTRIBUTE_NORETURN GNU_ATTRIBUTE_COLD
static void fire_error_mix_and_or_no_parenthesis(SrcLocation loc, std::string_view op1, std::string_view op2) {
  std::string name1 = static_cast<std::string>(op1);
  std::string name2 = static_cast<std::string>(op2);
  throw ParseError(loc, "mixing " + name1 + " with " + name2 + " without parenthesis may lead to accidental errors.  "
                                 "Use parenthesis to emphasize operator precedence.");
}

// diagnose when bitwise operators are used in a probably wrong way due to tricky precedence
// example: "flags & 0xFF != 0" is equivalent to "flags & 1", most likely it's unexpected
// the only way to suppress this error for the programmer is to use parenthesis
// (how do we detect presence of parenthesis? simple: (0!=1) is ast_parenthesized_expr{ast_binary_operator},
//  that's why if rhs->type == ast_binary_operator, it's not surrounded by parenthesis)
static void diagnose_bitwise_precedence(SrcLocation loc, std::string_view operator_name, AnyV lhs, AnyV rhs) {
  // handle "flags & 0xFF != 0" (rhs = "0xFF != 0")
  if (rhs->type == ast_binary_operator && is_comparison_binary_op(rhs->as<ast_binary_operator>()->tok)) {
    fire_error_lower_precedence(loc, operator_name, rhs->as<ast_binary_operator>()->operator_name);
  }

  // handle "0 != flags & 0xFF" (lhs = "0 != flags")
  if (lhs->type == ast_binary_operator && is_comparison_binary_op(lhs->as<ast_binary_operator>()->tok)) {
    fire_error_lower_precedence(loc, operator_name, lhs->as<ast_binary_operator>()->operator_name);
  }
}

// similar to above, but detect potentially invalid usage of && and ||
// since anyway, using parenthesis when both && and || occur in the same expression,
// && and || have equal operator precedence in Tolk
static void diagnose_and_or_precedence(SrcLocation loc, AnyV lhs, TokenType rhs_tok, std::string_view rhs_operator_name) {
  if (auto lhs_op = lhs->try_as<ast_binary_operator>()) {
    // handle "arg1 & arg2 | arg3" (lhs = "arg1 & arg2")
    if (is_bitwise_binary_op(lhs_op->tok) && is_bitwise_binary_op(rhs_tok) && lhs_op->tok != rhs_tok) {
      fire_error_mix_and_or_no_parenthesis(loc, lhs_op->operator_name, rhs_operator_name);
    }

    // handle "arg1 && arg2 || arg3" (lhs = "arg1 && arg2")
    if (is_logical_binary_op(lhs_op->tok) && is_logical_binary_op(rhs_tok) && lhs_op->tok != rhs_tok) {
      fire_error_mix_and_or_no_parenthesis(loc, lhs_op->operator_name, rhs_operator_name);
    }
  }
}

// diagnose "a << 8 + 1" (equivalent to "a << 9", probably unexpected)
static void diagnose_addition_in_bitshift(SrcLocation loc, std::string_view bitshift_operator_name, AnyV rhs) {
  if (rhs->type == ast_binary_operator && is_add_or_sub_binary_op(rhs->as<ast_binary_operator>()->tok)) {
    fire_error_lower_precedence(loc, bitshift_operator_name, rhs->as<ast_binary_operator>()->operator_name);
  }
}

// fire an error for FunC-style variable declaration, like "int i"
GNU_ATTRIBUTE_NORETURN GNU_ATTRIBUTE_COLD
static void fire_error_FunC_style_var_declaration(Lexer& lex) {
  SrcLocation loc = lex.cur_location();
  std::string type_str = static_cast<std::string>(lex.cur_str());      // int / slice / etc.
  lex.next();
  std::string var_name = lex.tok() == tok_identifier ? static_cast<std::string>(lex.cur_str()) : "name";
  throw ParseError(loc, "can't parse; probably, you use FunC-like declarations; valid syntax is `var " + var_name + ": " + type_str + " = ...`");
}

// replace (a == null) and similar to isNull(a) (call of a built-in function)
static AnyV maybe_replace_eq_null_with_isNull_call(V<ast_binary_operator> v) {
  bool has_null = v->get_lhs()->type == ast_null_keyword || v->get_rhs()->type == ast_null_keyword;
  bool replace = has_null && (v->tok == tok_eq || v->tok == tok_neq);
  if (!replace) {
    return v;
  }

  auto v_ident = createV<ast_identifier>(v->loc, "__isNull"); // built-in function
  AnyV v_null = v->get_lhs()->type == ast_null_keyword ? v->get_rhs() : v->get_lhs();
  AnyV v_arg = createV<ast_argument>(v->loc, v_null, false);
  AnyV v_isNull = createV<ast_function_call>(v->loc, v_ident, createV<ast_argument_list>(v->loc, {v_arg}));
  if (v->tok == tok_neq) {
    v_isNull = createV<ast_unary_operator>(v->loc, "!", tok_logical_not, v_isNull);
  }
  return v_isNull;
}


/*
 *
 *   PARSE SOURCE
 *
 */

// TE ::= TA | TA -> TE
// TA ::= int | ... | cont | var | _ | () | ( TE { , TE } ) | [ TE { , TE } ]
static TypeExpr* parse_type(Lexer& lex, V<ast_genericsT_list> genericsT_list);

static TypeExpr* parse_type1(Lexer& lex, V<ast_genericsT_list> genericsT_list) {
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
    case tok_continuation:
      lex.next();
      return TypeExpr::new_atomic(TypeExpr::_Continutaion);
    case tok_tuple:
      lex.next();
      return TypeExpr::new_atomic(TypeExpr::_Tuple);
    case tok_auto:
      lex.next();
      return TypeExpr::new_hole();
    case tok_void:
      lex.next();
      return TypeExpr::new_tensor({});
    case tok_bool:
      lex.error("bool type is not supported yet");
    case tok_self:
      lex.error("`self` type can be used only as a return type of a function (enforcing it to be chainable)");
    case tok_identifier:
      if (int idx = genericsT_list ? genericsT_list->lookup_idx(lex.cur_str()) : -1; idx != -1) {
        lex.next();
        return genericsT_list->get_item(idx)->created_type;
      }
      break;
    case tok_oppar: {
      lex.next();
      if (lex.tok() == tok_clpar) {
        lex.next();
        return TypeExpr::new_unit();
      }
      std::vector<TypeExpr*> sub{1, parse_type(lex, genericsT_list)};
      while (lex.tok() == tok_comma) {
        lex.next();
        sub.push_back(parse_type(lex, genericsT_list));
      }
      lex.expect(tok_clpar, "`)`");
      return TypeExpr::new_tensor(std::move(sub));
    }
    case tok_opbracket: {
      lex.next();
      if (lex.tok() == tok_clbracket) {
        lex.next();
        return TypeExpr::new_tuple({});
      }
      std::vector<TypeExpr*> sub{1, parse_type(lex, genericsT_list)};
      while (lex.tok() == tok_comma) {
        lex.next();
        sub.push_back(parse_type(lex, genericsT_list));
      }
      lex.expect(tok_clbracket, "`]`");
      return TypeExpr::new_tuple(std::move(sub));
    }
    default:
      break;
  }
  lex.unexpected("<type>");
}

static TypeExpr* parse_type(Lexer& lex, V<ast_genericsT_list> genericsT_list) {
  TypeExpr* res = parse_type1(lex, genericsT_list);
  if (lex.tok() == tok_arrow) {
    lex.next();
    TypeExpr* to = parse_type(lex, genericsT_list);
    return TypeExpr::new_map(res, to);
  }
  return res;
}

AnyV parse_expr(Lexer& lex);

static AnyV parse_parameter(Lexer& lex, V<ast_genericsT_list> genericsT_list, bool is_first) {
  SrcLocation loc = lex.cur_location();

  // optional keyword `mutate` meaning that a function will mutate a passed argument (like passed by reference)
  bool declared_as_mutate = false;
  bool is_param_self = false;
  if (lex.tok() == tok_mutate) {
    lex.next();
    declared_as_mutate = true;
  }

  // parameter name (or underscore for an unnamed parameter)
  std::string_view param_name;
  if (lex.tok() == tok_identifier) {
    param_name = lex.cur_str();
  } else if (lex.tok() == tok_self) {
    if (!is_first) {
      lex.error("`self` can only be the first parameter");
    }
    param_name = "self";
    is_param_self = true;
  } else if (lex.tok() != tok_underscore) {
    lex.unexpected("parameter name");
  }
  auto v_ident = createV<ast_identifier>(lex.cur_location(), param_name);
  lex.next();

  // parameter type after colon, also mandatory (even explicit ":auto")
  lex.expect(tok_colon, "`: <parameter_type>`");
  TypeExpr* param_type = parse_type(lex, genericsT_list);
  if (declared_as_mutate && !param_type->has_fixed_width()) {
    throw ParseError(loc, "`mutate` parameter must be strictly typed");
  }
  if (is_param_self && !param_type->has_fixed_width()) {
    throw ParseError(loc, "`self` parameter must be strictly typed");
  }

  return createV<ast_parameter>(loc, v_ident, param_type, declared_as_mutate);
}

static AnyV parse_global_var_declaration(Lexer& lex, const std::vector<V<ast_annotation>>& annotations) {
  if (!annotations.empty()) {
    lex.error("@annotations are not applicable to global var declaration");
  }
  SrcLocation loc = lex.cur_location();
  lex.expect(tok_global, "`global`");
  lex.check(tok_identifier, "global variable name");
  auto v_ident = createV<ast_identifier>(lex.cur_location(), lex.cur_str());
  lex.next();
  lex.expect(tok_colon, "`:`");
  TypeExpr* declared_type = parse_type(lex, nullptr);
  if (lex.tok() == tok_comma) {
    lex.error("multiple declarations are not allowed, split globals on separate lines");
  }
  if (lex.tok() == tok_assign) {
    lex.error("assigning to a global is not allowed at declaration");
  }
  lex.expect(tok_semicolon, "`;`");
  return createV<ast_global_var_declaration>(loc, v_ident, declared_type);
}

static AnyV parse_constant_declaration(Lexer& lex, const std::vector<V<ast_annotation>>& annotations) {
  if (!annotations.empty()) {
    lex.error("@annotations are not applicable to global var declaration");
  }
  SrcLocation loc = lex.cur_location();
  lex.expect(tok_const, "`const`");
  lex.check(tok_identifier, "constant name");
  auto v_ident = createV<ast_identifier>(lex.cur_location(), lex.cur_str());
  lex.next();
  TypeExpr *declared_type = nullptr;
  if (lex.tok() == tok_colon) {
    lex.next();
    if (lex.tok() == tok_int) {
      declared_type = TypeExpr::new_atomic(TypeExpr::_Int);
      lex.next();
    } else if (lex.tok() == tok_slice) {
      declared_type = TypeExpr::new_atomic(TypeExpr::_Slice);
      lex.next();
    } else {
      lex.error("a constant can be int or slice only");
    }
  }
  lex.expect(tok_assign, "`=`");
  AnyV init_value = parse_expr(lex);
  if (lex.tok() == tok_comma) {
    lex.error("multiple declarations are not allowed, split constants on separate lines");
  }
  lex.expect(tok_semicolon, "`;`");
  return createV<ast_constant_declaration>(loc, v_ident, declared_type, init_value);
}

// "parameters" are at function declaration: `fun f(param1: int, mutate param2: slice)`
static V<ast_parameter_list> parse_parameter_list(Lexer& lex, V<ast_genericsT_list> genericsT_list) {
  SrcLocation loc = lex.cur_location();
  std::vector<AnyV> params;
  lex.expect(tok_oppar, "parameter list");
  if (lex.tok() != tok_clpar) {
    params.push_back(parse_parameter(lex, genericsT_list, true));
    while (lex.tok() == tok_comma) {
      lex.next();
      params.push_back(parse_parameter(lex, genericsT_list, false));
    }
  }
  lex.expect(tok_clpar, "`)`");
  return createV<ast_parameter_list>(loc, std::move(params));
}

// "arguments" are at function call: `f(arg1, mutate arg2)`
static AnyV parse_argument(Lexer& lex) {
  SrcLocation loc = lex.cur_location();

  // keyword `mutate` is necessary when a parameter is declared `mutate` (to make mutation obvious for the reader)
  bool passed_as_mutate = false;
  if (lex.tok() == tok_mutate) {
    lex.next();
    passed_as_mutate = true;
  }

  AnyV expr = parse_expr(lex);
  return createV<ast_argument>(loc, expr, passed_as_mutate);
}

static V<ast_argument_list> parse_argument_list(Lexer& lex) {
  SrcLocation loc = lex.cur_location();
  std::vector<AnyV> args;
  lex.expect(tok_oppar, "`(`");
  if (lex.tok() != tok_clpar) {
    args.push_back(parse_argument(lex));
    while (lex.tok() == tok_comma) {
      lex.next();
      args.push_back(parse_argument(lex));
    }
  }
  lex.expect(tok_clpar, "`)`");
  return createV<ast_argument_list>(loc, std::move(args));
}

// parse (expr) / [expr] / identifier / number
static AnyV parse_expr100(Lexer& lex) {
  SrcLocation loc = lex.cur_location();
  switch (lex.tok()) {
    case tok_oppar: {
      lex.next();
      if (lex.tok() == tok_clpar) {
        lex.next();
        return createV<ast_tensor>(loc, {});
      }
      AnyV first = parse_expr(lex);
      if (lex.tok() == tok_clpar) {
        lex.next();
        return createV<ast_parenthesized_expr>(loc, first);
      }
      std::vector<AnyV> items(1, first);
      while (lex.tok() == tok_comma) {
        lex.next();
        items.emplace_back(parse_expr(lex));
      }
      lex.expect(tok_clpar, "`)`");
      return createV<ast_tensor>(loc, std::move(items));
    }
    case tok_opbracket: {
      lex.next();
      if (lex.tok() == tok_clbracket) {
        lex.next();
        return createV<ast_tensor_square>(loc, {});
      }
      std::vector<AnyV> items(1, parse_expr(lex));
      while (lex.tok() == tok_comma) {
        lex.next();
        items.emplace_back(parse_expr(lex));
      }
      lex.expect(tok_clbracket, "`]`");
      return createV<ast_tensor_square>(loc, std::move(items));
    }
    case tok_int_const: {
      std::string_view int_val = lex.cur_str();
      lex.next();
      return createV<ast_int_const>(loc, int_val);
    }
    case tok_string_const: {
      std::string_view str_val = lex.cur_str();
      lex.next();
      char modifier = 0;
      if (lex.tok() == tok_string_modifier) {
        modifier = lex.cur_str()[0];
        lex.next();
      }
      return createV<ast_string_const>(loc, str_val, modifier);
    }
    case tok_underscore: {
      lex.next();
      return createV<ast_underscore>(loc);
    }
    case tok_true: {
      lex.next();
      return createV<ast_bool_const>(loc, true);
    }
    case tok_false: {
      lex.next();
      return createV<ast_bool_const>(loc, false);
    }
    case tok_null: {
      lex.next();
      return createV<ast_null_keyword>(loc);
    }
    case tok_self: {
      lex.next();
      return createV<ast_self_keyword>(loc);
    }
    case tok_identifier: {
      std::string_view str_val = lex.cur_str();
      lex.next();
      return createV<ast_identifier>(loc, str_val);
    }
    default: {
      // show a proper error for `int i` (FunC-style declarations)
      TokenType t = lex.tok();
      if (t == tok_int || t == tok_cell || t == tok_slice || t == tok_builder || t == tok_tuple) {
        fire_error_FunC_style_var_declaration(lex);
      }
      lex.unexpected("<expression>");
    }
  }
}

// parse E(args)
static AnyV parse_expr90(Lexer& lex) {
  AnyV res = parse_expr100(lex);
  if (lex.tok() == tok_oppar) {
    return createV<ast_function_call>(res->loc, res, parse_argument_list(lex));
  }
  return res;
}

// parse E.method(...) (left-to-right)
static AnyV parse_expr80(Lexer& lex) {
  AnyV lhs = parse_expr90(lex);
  while (lex.tok() == tok_dot) {
    SrcLocation loc = lex.cur_location();
    lex.next();
    lex.check(tok_identifier, "method name");
    std::string_view method_name = lex.cur_str();
    lex.next();
    lhs = createV<ast_dot_method_call>(loc, method_name, lhs, parse_argument_list(lex));
  }
  return lhs;
}

// parse ! ~ - + E (unary)
static AnyV parse_expr75(Lexer& lex) {
  TokenType t = lex.tok();
  if (t == tok_logical_not || t == tok_bitwise_not || t == tok_minus || t == tok_plus) {
    SrcLocation loc = lex.cur_location();
    std::string_view operator_name = lex.cur_str();
    lex.next();
    AnyV rhs = parse_expr75(lex);
    return createV<ast_unary_operator>(loc, operator_name, t, rhs);
  }
  return parse_expr80(lex);
}

// parse E * / % ^/ ~/ E (left-to-right)
static AnyV parse_expr30(Lexer& lex) {
  AnyV lhs = parse_expr75(lex);
  TokenType t = lex.tok();
  while (t == tok_mul || t == tok_div || t == tok_mod || t == tok_divC || t == tok_divR) {
    SrcLocation loc = lex.cur_location();
    std::string_view operator_name = lex.cur_str();
    lex.next();
    AnyV rhs = parse_expr75(lex);
    lhs = createV<ast_binary_operator>(loc, operator_name, t, lhs, rhs);
    t = lex.tok();
  }
  return lhs;
}

// parse E + - E (left-to-right)
static AnyV parse_expr20(Lexer& lex) {
  AnyV lhs = parse_expr30(lex);
  TokenType t = lex.tok();
  while (t == tok_minus || t == tok_plus) {
    SrcLocation loc = lex.cur_location();
    std::string_view operator_name = lex.cur_str();
    lex.next();
    AnyV rhs = parse_expr30(lex);
    lhs = createV<ast_binary_operator>(loc, operator_name, t, lhs, rhs);
    t = lex.tok();
  }
  return lhs;
}

// parse E << >> ~>> ^>> E (left-to-right)
static AnyV parse_expr17(Lexer& lex) {
  AnyV lhs = parse_expr20(lex);
  TokenType t = lex.tok();
  while (t == tok_lshift || t == tok_rshift || t == tok_rshiftC || t == tok_rshiftR) {
    SrcLocation loc = lex.cur_location();
    std::string_view operator_name = lex.cur_str();
    lex.next();
    AnyV rhs = parse_expr20(lex);
    diagnose_addition_in_bitshift(loc, operator_name, rhs);
    lhs = createV<ast_binary_operator>(loc, operator_name, t, lhs, rhs);
    t = lex.tok();
  }
  return lhs;
}

// parse E == < > <= >= != <=> E (left-to-right)
static AnyV parse_expr15(Lexer& lex) {
  AnyV lhs = parse_expr17(lex);
  TokenType t = lex.tok();
  if (t == tok_eq || t == tok_lt || t == tok_gt || t == tok_leq || t == tok_geq || t == tok_neq || t == tok_spaceship) {
    SrcLocation loc = lex.cur_location();
    std::string_view operator_name = lex.cur_str();
    lex.next();
    AnyV rhs = parse_expr17(lex);
    lhs = createV<ast_binary_operator>(loc, operator_name, t, lhs, rhs);
    if (t == tok_eq || t == tok_neq) {
      lhs = maybe_replace_eq_null_with_isNull_call(lhs->as<ast_binary_operator>());
    }
  }
  return lhs;
}

// parse E & | ^ E (left-to-right)
static AnyV parse_expr14(Lexer& lex) {
  AnyV lhs = parse_expr15(lex);
  TokenType t = lex.tok();
  while (t == tok_bitwise_and || t == tok_bitwise_or || t == tok_bitwise_xor) {
    SrcLocation loc = lex.cur_location();
    std::string_view operator_name = lex.cur_str();
    lex.next();
    AnyV rhs = parse_expr15(lex);
    diagnose_bitwise_precedence(loc, operator_name, lhs, rhs);
    diagnose_and_or_precedence(loc, lhs, t, operator_name);
    lhs = createV<ast_binary_operator>(loc, operator_name, t, lhs, rhs);
    t = lex.tok();
  }
  return lhs;
}

// parse E && || E (left-to-right)
static AnyV parse_expr13(Lexer& lex) {
  AnyV lhs = parse_expr14(lex);
  TokenType t = lex.tok();
  while (t == tok_logical_and || t == tok_logical_or) {
    SrcLocation loc = lex.cur_location();
    std::string_view operator_name = lex.cur_str();
    lex.next();
    AnyV rhs = parse_expr14(lex);
    diagnose_and_or_precedence(loc, lhs, t, operator_name);
    lhs = createV<ast_binary_operator>(loc, operator_name, t, lhs, rhs);
    t = lex.tok();
  }
  return lhs;
}

// parse E = += -= E and E ? E : E (right-to-left)
static AnyV parse_expr10(Lexer& lex) {
  AnyV lhs = parse_expr13(lex);
  TokenType t = lex.tok();
  if (t == tok_set_plus || t == tok_set_minus || t == tok_set_mul || t == tok_set_div ||
      t == tok_set_mod || t == tok_set_lshift || t == tok_set_rshift ||
      t == tok_set_bitwise_and || t == tok_set_bitwise_or || t == tok_set_bitwise_xor ||
      t == tok_assign) {
    SrcLocation loc = lex.cur_location();
    std::string_view operator_name = lex.cur_str();
    lex.next();
    AnyV rhs = parse_expr10(lex);
    return createV<ast_binary_operator>(loc, operator_name, t, lhs, rhs);
  }
  if (t == tok_question) {
    SrcLocation loc = lex.cur_location();
    lex.next();
    AnyV when_true = parse_expr10(lex);
    lex.expect(tok_colon, "`:`");
    AnyV when_false = parse_expr10(lex);
    return createV<ast_ternary_operator>(loc, lhs, when_true, when_false);
  }
  return lhs;
}

AnyV parse_expr(Lexer& lex) {
  return parse_expr10(lex);
}

AnyV parse_statement(Lexer& lex);

static AnyV parse_var_declaration_lhs(Lexer& lex, bool is_immutable) {
  SrcLocation loc = lex.cur_location();
  if (lex.tok() == tok_oppar) {
    lex.next();
    AnyV first = parse_var_declaration_lhs(lex, is_immutable);
    if (lex.tok() == tok_clpar) {
      lex.next();
      return createV<ast_parenthesized_expr>(loc, first);
    }
    std::vector<AnyV> args(1, first);
    while (lex.tok() == tok_comma) {
      lex.next();
      args.push_back(parse_var_declaration_lhs(lex, is_immutable));
    }
    lex.expect(tok_clpar, "`)`");
    return createV<ast_tensor>(loc, std::move(args));
  }
  if (lex.tok() == tok_opbracket) {
    lex.next();
    std::vector<AnyV> args(1, parse_var_declaration_lhs(lex, is_immutable));
    while (lex.tok() == tok_comma) {
      lex.next();
      args.push_back(parse_var_declaration_lhs(lex, is_immutable));
    }
    lex.expect(tok_clbracket, "`]`");
    return createV<ast_tensor_square>(loc, std::move(args));
  }
  if (lex.tok() == tok_identifier) {
    auto v_ident = createV<ast_identifier>(loc, lex.cur_str());
    TypeExpr* declared_type = nullptr;
    bool marked_as_redef = false;
    lex.next();
    if (lex.tok() == tok_colon) {
      lex.next();
      declared_type = parse_type(lex, nullptr);
    } else if (lex.tok() == tok_redef) {
      lex.next();
      marked_as_redef = true;
    }
    return createV<ast_local_var>(loc, v_ident, declared_type, is_immutable, marked_as_redef);
  }
  if (lex.tok() == tok_underscore) {
    TypeExpr* declared_type = nullptr;
    lex.next();
    if (lex.tok() == tok_colon) {
      lex.next();
      declared_type = parse_type(lex, nullptr);
    }
    return createV<ast_local_var>(loc, createV<ast_underscore>(loc), declared_type, true, false);
  }
  lex.unexpected("variable name");
}

static AnyV parse_local_vars_declaration(Lexer& lex) {
  SrcLocation loc = lex.cur_location();
  bool is_immutable = lex.tok() == tok_val;
  lex.next();

  AnyV lhs = parse_var_declaration_lhs(lex, is_immutable);
  if (lex.tok() != tok_assign) {
    lex.error("variables declaration must be followed by assignment: `var xxx = ...`");
  }
  lex.next();
  AnyV assigned_val = parse_expr(lex);

  if (lex.tok() == tok_comma) {
    lex.error("multiple declarations are not allowed, split variables on separate lines");
  }
  lex.expect(tok_semicolon, "`;`");
  return createV<ast_local_vars_declaration>(loc, lhs, assigned_val);
}

static V<ast_sequence> parse_sequence(Lexer& lex) {
  SrcLocation loc = lex.cur_location();
  lex.expect(tok_opbrace, "`{`");
  std::vector<AnyV> items;
  while (lex.tok() != tok_clbrace) {
    items.push_back(parse_statement(lex));
  }
  SrcLocation loc_end = lex.cur_location();
  lex.expect(tok_clbrace, "`}`");
  return createV<ast_sequence>(loc, loc_end, items);
}

static AnyV parse_return_statement(Lexer& lex) {
  SrcLocation loc = lex.cur_location();
  lex.expect(tok_return, "`return`");
  AnyV child = lex.tok() == tok_semicolon   // `return;` actually means `return ();` (which is void)
    ? createV<ast_tensor>(lex.cur_location(), {})
    : parse_expr(lex);
  lex.expect(tok_semicolon, "`;`");
  return createV<ast_return_statement>(loc, child);
}

static AnyV parse_if_statement(Lexer& lex, bool is_ifnot) {
  SrcLocation loc = lex.cur_location();
  lex.expect(tok_if, "`if`");

  lex.expect(tok_oppar, "`(`");
  AnyV cond = parse_expr(lex);
  lex.expect(tok_clpar, "`)`");
  // replace if(!expr) with ifnot(expr) (this should be done later, but for now, let this be right at parsing time)
  if (auto v_not = cond->try_as<ast_unary_operator>(); v_not && v_not->tok == tok_logical_not) {
    is_ifnot = !is_ifnot;
    cond = v_not->get_rhs();
  }

  V<ast_sequence> if_body = parse_sequence(lex);
  V<ast_sequence> else_body = nullptr;
  if (lex.tok() == tok_else) {  // else if(e) { } or else { }
    lex.next();
    if (lex.tok() == tok_if) {
      AnyV v_inner_if = parse_if_statement(lex, false);
      else_body = createV<ast_sequence>(v_inner_if->loc, lex.cur_location(), {v_inner_if});
    } else {
      else_body = parse_sequence(lex);
    }
  } else {  // no 'else', create empty block
    else_body = createV<ast_sequence>(lex.cur_location(), lex.cur_location(), {});
  }
  return createV<ast_if_statement>(loc, is_ifnot, cond, if_body, else_body);
}

static AnyV parse_repeat_statement(Lexer& lex) {
  SrcLocation loc = lex.cur_location();
  lex.expect(tok_repeat, "`repeat`");
  lex.expect(tok_oppar, "`(`");
  AnyV cond = parse_expr(lex);
  lex.expect(tok_clpar, "`)`");
  V<ast_sequence> body = parse_sequence(lex);
  return createV<ast_repeat_statement>(loc, cond, body);
}

static AnyV parse_while_statement(Lexer& lex) {
  SrcLocation loc = lex.cur_location();
  lex.expect(tok_while, "`while`");
  lex.expect(tok_oppar, "`(`");
  AnyV cond = parse_expr(lex);
  lex.expect(tok_clpar, "`)`");
  V<ast_sequence> body = parse_sequence(lex);
  return createV<ast_while_statement>(loc, cond, body);
}

static AnyV parse_do_while_statement(Lexer& lex) {
  SrcLocation loc = lex.cur_location();
  lex.expect(tok_do, "`do`");
  V<ast_sequence> body = parse_sequence(lex);
  lex.expect(tok_while, "`while`");
  lex.expect(tok_oppar, "`(`");
  AnyV cond = parse_expr(lex);
  lex.expect(tok_clpar, "`)`");
  lex.expect(tok_semicolon, "`;`");
  return createV<ast_do_while_statement>(loc, body, cond);
}

static AnyV parse_catch_variable(Lexer& lex) {
  SrcLocation loc = lex.cur_location();
  if (lex.tok() == tok_identifier) {
    std::string_view var_name = lex.cur_str();
    lex.next();
    return createV<ast_identifier>(loc, var_name);
  }
  if (lex.tok() == tok_underscore) {
    lex.next();
    return createV<ast_underscore>(loc);
  }
  lex.unexpected("identifier");
}

static AnyV parse_throw_statement(Lexer& lex) {
  SrcLocation loc = lex.cur_location();
  lex.expect(tok_throw, "`throw`");

  AnyV thrown_code, thrown_arg;
  if (lex.tok() == tok_oppar) {   // throw (code) or throw (code, arg)
    lex.next();
    thrown_code = parse_expr(lex);
    if (lex.tok() == tok_comma) {
      lex.next();
      thrown_arg = parse_expr(lex);
    } else {
      thrown_arg = createV<ast_empty>(loc);
    }
    lex.expect(tok_clpar, "`)`");
  } else {   // throw code
    thrown_code = parse_expr(lex);
    thrown_arg = createV<ast_empty>(loc);
  }

  lex.expect(tok_semicolon, "`;`");
  return createV<ast_throw_statement>(loc, thrown_code, thrown_arg);
}

static AnyV parse_assert_statement(Lexer& lex) {
  SrcLocation loc = lex.cur_location();
  lex.expect(tok_assert, "`assert`");

  lex.expect(tok_oppar, "`(`");
  AnyV cond = parse_expr(lex);
  AnyV thrown_code;
  if (lex.tok() == tok_comma) {   // assert(cond, code)
    lex.next();
    thrown_code = parse_expr(lex);
    lex.expect(tok_clpar, "`)`");
  } else {  // assert(cond) throw code
    lex.expect(tok_clpar, "`)`");
    lex.expect(tok_throw, "`throw excNo` after assert");
    thrown_code = parse_expr(lex);
  }

  lex.expect(tok_semicolon, "`;`");
  return createV<ast_assert_statement>(loc, cond, thrown_code);
}

static AnyV parse_try_catch_statement(Lexer& lex) {
  SrcLocation loc = lex.cur_location();
  lex.expect(tok_try, "`try`");
  V<ast_sequence> try_body = parse_sequence(lex);

  std::vector<AnyV> catch_args;
  lex.expect(tok_catch, "`catch`");
  SrcLocation catch_loc = lex.cur_location();
  if (lex.tok() == tok_oppar) {
    lex.next();
    catch_args.push_back(parse_catch_variable(lex));
    if (lex.tok() == tok_comma) { // catch (excNo, arg)
      lex.next();
      catch_args.push_back(parse_catch_variable(lex));
    } else {  // catch (excNo) -> catch (excNo, _)
      catch_args.push_back(createV<ast_underscore>(catch_loc));
    }
    lex.expect(tok_clpar, "`)`");
  } else {  // catch -> catch (_, _)
    catch_args.push_back(createV<ast_underscore>(catch_loc));
    catch_args.push_back(createV<ast_underscore>(catch_loc));
  }
  V<ast_tensor> catch_expr = createV<ast_tensor>(catch_loc, std::move(catch_args));

  V<ast_sequence> catch_body = parse_sequence(lex);
  return createV<ast_try_catch_statement>(loc, try_body, catch_expr, catch_body);
}

AnyV parse_statement(Lexer& lex) {
  switch (lex.tok()) {
    case tok_var:
    case tok_val:
      return parse_local_vars_declaration(lex);
    case tok_opbrace:
      return parse_sequence(lex);
    case tok_return:
      return parse_return_statement(lex);
    case tok_if:
      return parse_if_statement(lex, false);
    case tok_repeat:
      return parse_repeat_statement(lex);
    case tok_do:
      return parse_do_while_statement(lex);
    case tok_while:
      return parse_while_statement(lex);
    case tok_throw:
      return parse_throw_statement(lex);
    case tok_assert:
      return parse_assert_statement(lex);
    case tok_try:
      return parse_try_catch_statement(lex);
    case tok_semicolon: {
      SrcLocation loc = lex.cur_location();
      lex.next();
      return createV<ast_empty>(loc);
    }
    case tok_break:
    case tok_continue:
      lex.error("break/continue from loops are not supported yet");
    default: {
      AnyV expr = parse_expr(lex);
      lex.expect(tok_semicolon, "`;`");
      return expr;
    }
  }
}

static AnyV parse_func_body(Lexer& lex) {
  return parse_sequence(lex);
}

static AnyV parse_asm_func_body(Lexer& lex, V<ast_parameter_list> param_list) {
  SrcLocation loc = lex.cur_location();
  lex.expect(tok_asm, "`asm`");
  size_t n_params = param_list->size();
  if (n_params > 16) {
    throw ParseError{loc, "assembler built-in function can have at most 16 arguments"};
  }
  std::vector<int> arg_order, ret_order;
  if (lex.tok() == tok_oppar) {
    lex.next();
    while (lex.tok() == tok_identifier || lex.tok() == tok_self) {
      int arg_idx = param_list->lookup_idx(lex.cur_str());
      if (arg_idx == -1) {
        lex.unexpected("parameter name");
      }
      arg_order.push_back(arg_idx);
      lex.next();
    }
    if (lex.tok() == tok_arrow) {
      lex.next();
      while (lex.tok() == tok_int_const) {
        int ret_idx = std::atoi(static_cast<std::string>(lex.cur_str()).c_str());
        ret_order.push_back(ret_idx);
        lex.next();
      }
    }
    lex.expect(tok_clpar, "`)`");
  }
  std::vector<AnyV> asm_commands;
  lex.check(tok_string_const, "\"ASM COMMAND\"");
  while (lex.tok() == tok_string_const) {
    std::string_view asm_command = lex.cur_str();
    asm_commands.push_back(createV<ast_string_const>(lex.cur_location(), asm_command, 0));
    lex.next();
  }
  lex.expect(tok_semicolon, "`;`");
  return createV<ast_asm_body>(loc, std::move(arg_order), std::move(ret_order), std::move(asm_commands));
}

static AnyV parse_genericsT_list(Lexer& lex) {
  SrcLocation loc = lex.cur_location();
  std::vector<AnyV> genericsT_items;
  lex.expect(tok_lt, "`<`");
  int idx = 0;
  while (true) {
    lex.check(tok_identifier, "T");
    std::string_view nameT = lex.cur_str();
    TypeExpr* type = TypeExpr::new_var(idx++);
    genericsT_items.emplace_back(createV<ast_genericsT_item>(lex.cur_location(), type, nameT));
    lex.next();
    if (lex.tok() != tok_comma) {
      break;
    }
    lex.next();
  }
  lex.expect(tok_gt, "`>`");
  return createV<ast_genericsT_list>{loc, std::move(genericsT_items)};
}

static V<ast_annotation> parse_annotation(Lexer& lex) {
  SrcLocation loc = lex.cur_location();
  lex.check(tok_annotation_at, "`@`");
  std::string_view name = lex.cur_str();
  AnnotationKind kind = Vertex<ast_annotation>::parse_kind(name);
  lex.next();

  V<ast_tensor> v_arg = nullptr;
  if (lex.tok() == tok_oppar) {
    SrcLocation loc_args = lex.cur_location();
    lex.next();
    std::vector<AnyV> args;
    args.push_back(parse_expr(lex));
    while (lex.tok() == tok_comma) {
      lex.next();
      args.push_back(parse_expr(lex));
    }
    lex.expect(tok_clpar, "`)`");
    v_arg = createV<ast_tensor>(loc_args, std::move(args));
  }

  switch (kind) {
    case AnnotationKind::unknown:
      throw ParseError(loc, "unknown annotation " + static_cast<std::string>(name));
    case AnnotationKind::inline_simple:
    case AnnotationKind::inline_ref:
    case AnnotationKind::pure:
    case AnnotationKind::deprecated:
      if (v_arg) {
        throw ParseError(v_arg->loc, "arguments aren't allowed for " + static_cast<std::string>(name));
      }
      v_arg = createV<ast_tensor>(loc, {});
      break;
    case AnnotationKind::method_id:
      if (!v_arg || v_arg->size() != 1 || v_arg->get_item(0)->type != ast_int_const) {
        throw ParseError(loc, "expecting `(number)` after " + static_cast<std::string>(name));
      }
      break;
  }

  return createV<ast_annotation>(loc, kind, v_arg);
}

static AnyV parse_function_declaration(Lexer& lex, const std::vector<V<ast_annotation>>& annotations) {
  SrcLocation loc = lex.cur_location();
  bool is_get_method = lex.tok() == tok_get;
  lex.next();
  if (is_get_method && lex.tok() == tok_fun) {
    lex.next();   // 'get f()' and 'get fun f()' both correct
  }

  lex.check(tok_identifier, "function name identifier");

  std::string_view f_name = lex.cur_str();
  bool is_entrypoint =
        f_name == "main" || f_name == "onInternalMessage" || f_name == "onExternalMessage" ||
        f_name == "onRunTickTock" || f_name == "onSplitPrepare" || f_name == "onSplitInstall";
  bool is_FunC_entrypoint =
        f_name == "recv_internal" || f_name == "recv_external" ||
        f_name == "run_ticktock" || f_name == "split_prepare" || f_name == "split_install";
  if (is_FunC_entrypoint) {
    lex.error("this is a reserved FunC/Fift identifier; you need `onInternalMessage`");
  }

  auto v_ident = createV<ast_identifier>(lex.cur_location(), f_name);
  lex.next();

  V<ast_genericsT_list> genericsT_list = nullptr;
  if (lex.tok() == tok_lt) {    // 'fun f<T1,T2>'
    genericsT_list = parse_genericsT_list(lex)->as<ast_genericsT_list>();
  }

  V<ast_parameter_list> v_param_list = parse_parameter_list(lex, genericsT_list)->as<ast_parameter_list>();
  bool accepts_self = !v_param_list->empty() && v_param_list->get_param(0)->get_identifier()->name == "self";
  int n_mutate_params = v_param_list->get_mutate_params_count();

  TypeExpr* ret_type = nullptr;
  bool returns_self = false;
  if (lex.tok() == tok_colon) {   // : <ret_type> (if absent, it means "auto infer", not void)
    lex.next();
    if (lex.tok() == tok_self) {
      if (!accepts_self) {
        lex.error("only a member function can return `self` (which accepts `self` first parameter)");
      }
      lex.next();
      returns_self = true;
      ret_type = TypeExpr::new_unit();
    } else {
      ret_type = parse_type(lex, genericsT_list);
    }
  }

  if (is_entrypoint && (is_get_method || genericsT_list || n_mutate_params || accepts_self)) {
    throw ParseError(loc, "invalid declaration of a reserved function");
  }
  if (is_get_method && (genericsT_list || n_mutate_params || accepts_self)) {
    throw ParseError(loc, "get methods can't have `mutate` and `self` params");
  }

  if (n_mutate_params) {
    std::vector<TypeExpr*> ret_tensor_items;
    ret_tensor_items.reserve(1 + n_mutate_params);
    for (AnyV v_param : v_param_list->get_params()) {
      if (v_param->as<ast_parameter>()->declared_as_mutate) {
        ret_tensor_items.emplace_back(v_param->as<ast_parameter>()->param_type);
      }
    }
    ret_tensor_items.emplace_back(ret_type ? ret_type : TypeExpr::new_hole());
    ret_type = TypeExpr::new_tensor(std::move(ret_tensor_items));
  }

  AnyV v_body = nullptr;

  if (lex.tok() == tok_builtin) {
    v_body = createV<ast_empty>(lex.cur_location());
    lex.next();
    lex.expect(tok_semicolon, "`;`");
  } else if (lex.tok() == tok_opbrace) {
    v_body = parse_func_body(lex);
  } else if (lex.tok() == tok_asm) {
    if (!ret_type) {
      lex.error("asm function must specify return type");
    }
    v_body = parse_asm_func_body(lex, v_param_list);
  } else {
    lex.unexpected("{ function body }");
  }

  auto f_declaration = createV<ast_function_declaration>(loc, v_ident, v_param_list, v_body);
  f_declaration->ret_type = ret_type ? ret_type : TypeExpr::new_hole();
  f_declaration->is_entrypoint = is_entrypoint;
  f_declaration->genericsT_list = genericsT_list;
  f_declaration->marked_as_get_method = is_get_method;
  f_declaration->marked_as_builtin = v_body->type == ast_empty;
  f_declaration->accepts_self = accepts_self;
  f_declaration->returns_self = returns_self;

  for (auto v_annotation : annotations) {
    switch (v_annotation->kind) {
      case AnnotationKind::inline_simple:
        f_declaration->marked_as_inline = true;
        break;
      case AnnotationKind::inline_ref:
        f_declaration->marked_as_inline_ref = true;
        break;
      case AnnotationKind::pure:
        f_declaration->marked_as_pure = true;
        break;
      case AnnotationKind::method_id:
        if (is_get_method || genericsT_list || is_entrypoint || n_mutate_params || accepts_self) {
          v_annotation->error("@method_id can be specified only for regular functions");
        }
        f_declaration->method_id = v_annotation->get_arg()->get_item(0)->as<ast_int_const>();
        break;
      case AnnotationKind::deprecated:
        // no special handling
        break;

      default:
        v_annotation->error("this annotation is not applicable to functions");
    }
  }

  return f_declaration;
}

static AnyV parse_tolk_required_version(Lexer& lex) {
  SrcLocation loc = lex.cur_location();
  lex.next_special(tok_semver, "semver");   // syntax: "tolk 0.6"
  std::string semver = static_cast<std::string>(lex.cur_str());
  lex.next();

  // for simplicity, there is no syntax ">= version" and so on, just strict compare
  if (TOLK_VERSION != semver && TOLK_VERSION != semver + ".0") {    // 0.6 = 0.6.0
    loc.show_warning("the contract is written in Tolk v" + semver + ", but you use Tolk compiler v" + TOLK_VERSION + "; probably, it will lead to compilation errors or hash changes");
  }

  return createV<ast_tolk_required_version>(loc, tok_eq, semver);  // semicolon is not necessary
}

static AnyV parse_import_statement(Lexer& lex) {
  SrcLocation loc = lex.cur_location();
  lex.expect(tok_import, "`import`");
  lex.check(tok_string_const, "source file name");
  std::string_view rel_filename = lex.cur_str();
  if (rel_filename.empty()) {
    lex.error("imported file name is an empty string");
  }
  auto v_str = createV<ast_string_const>(lex.cur_location(), rel_filename, 0);
  lex.next();
  return createV<ast_import_statement>(loc, v_str); // semicolon is not necessary
}

// the main (exported) function
AnyV parse_src_file_to_ast(const SrcFile* file) {
  std::vector<AnyV> toplevel_declarations;
  std::vector<V<ast_annotation>> annotations;
  Lexer lex(file);

  while (!lex.is_eof()) {
    switch (lex.tok()) {
      case tok_tolk:
        if (!annotations.empty()) {
          lex.unexpected("declaration after @annotations");
        }
        toplevel_declarations.push_back(parse_tolk_required_version(lex));
        break;
      case tok_import:
        if (!annotations.empty()) {
          lex.unexpected("declaration after @annotations");
        }
        toplevel_declarations.push_back(parse_import_statement(lex));
        break;
      case tok_semicolon:
        if (!annotations.empty()) {
          lex.unexpected("declaration after @annotations");
        }
        lex.next();  // don't add ast_empty, no need
        break;

      case tok_annotation_at:
        annotations.push_back(parse_annotation(lex));
        break;
      case tok_global:
        toplevel_declarations.push_back(parse_global_var_declaration(lex, annotations));
        annotations.clear();
        break;
      case tok_const:
        toplevel_declarations.push_back(parse_constant_declaration(lex, annotations));
        annotations.clear();
        break;
      case tok_fun:
      case tok_get:
        toplevel_declarations.push_back(parse_function_declaration(lex, annotations));
        annotations.clear();
        break;

      case tok_export:
      case tok_struct:
      case tok_enum:
      case tok_operator:
      case tok_infix:
        lex.error("`" + static_cast<std::string>(lex.cur_str()) +"` is not supported yet");

      default:
        lex.unexpected("fun or get");
    }
  }

  return createV<ast_tolk_file>(file, std::move(toplevel_declarations));
}

}  // namespace tolk
