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
// (in Tolk, they are used as logical ones due to absence of a boolean type and && || operators)
static bool is_bitwise_binary_op(TokenType tok) {
  return tok == tok_bitwise_and || tok == tok_bitwise_or || tok == tok_bitwise_xor;
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
static void fire_error_mix_bitwise_and_or(SrcLocation loc, std::string_view op1, std::string_view op2) {
  std::string name1 = static_cast<std::string>(op1);
  std::string name2 = static_cast<std::string>(op2);
  throw ParseError(loc, "mixing " + name1 + " with " + name2 + " without parenthesis"
                                 ", probably this code won't work as you expected.  "
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

  // handle "arg1 & arg2 | arg3" (lhs = "arg1 & arg2")
  if (lhs->type == ast_binary_operator && is_bitwise_binary_op(lhs->as<ast_binary_operator>()->tok) && lhs->as<ast_binary_operator>()->operator_name != operator_name) {
    fire_error_mix_bitwise_and_or(loc, lhs->as<ast_binary_operator>()->operator_name, operator_name);
  }
}

// diagnose "a << 8 + 1" (equivalent to "a << 9", probably unexpected)
static void diagnose_addition_in_bitshift(SrcLocation loc, std::string_view bitshift_operator_name, AnyV rhs) {
  if (rhs->type == ast_binary_operator && is_add_or_sub_binary_op(rhs->as<ast_binary_operator>()->tok)) {
    fire_error_lower_precedence(loc, bitshift_operator_name, rhs->as<ast_binary_operator>()->operator_name);
  }
}

/*
 *
 *   PARSE SOURCE
 *
 */

// TE ::= TA | TA -> TE
// TA ::= int | ... | cont | var | _ | () | ( TE { , TE } ) | [ TE { , TE } ]
TypeExpr* parse_type(Lexer& lex, V<ast_forall_list> forall_list);

TypeExpr* parse_type1(Lexer& lex, V<ast_forall_list> forall_list) {
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
      if (int idx = forall_list ? forall_list->lookup_idx(lex.cur_str()) : -1; idx != -1) {
        lex.next();
        return forall_list->get_item(idx)->created_type;
      }
      lex.error("Is not a type identifier");
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
  auto t1 = parse_type(lex, forall_list);
  if (lex.tok() == tok_clpar) {
    lex.expect(c, c == tok_clpar ? "')'" : "']'");
    return t1;
  }
  std::vector<TypeExpr*> tlist{1, t1};
  while (lex.tok() == tok_comma) {
    lex.next();
    tlist.push_back(parse_type(lex, forall_list));
  }
  lex.expect(c, c == tok_clpar ? "')'" : "']'");
  return c == tok_clpar ? TypeExpr::new_tensor(std::move(tlist)) : TypeExpr::new_tuple(std::move(tlist));
}

TypeExpr* parse_type(Lexer& lex, V<ast_forall_list> forall_list) {
  TypeExpr* res = parse_type1(lex, forall_list);
  if (lex.tok() == tok_mapsto) {
    lex.next();
    TypeExpr* to = parse_type(lex, forall_list);
    return TypeExpr::new_map(res, to);
  }
  return res;
}

AnyV parse_argument(Lexer& lex, V<ast_forall_list> forall_list) {
  TypeExpr* arg_type = nullptr;
  SrcLocation loc = lex.cur_location();
  if (lex.tok() == tok_underscore) {
    lex.next();
    if (lex.tok() == tok_comma || lex.tok() == tok_clpar) {
      return createV<ast_argument>(loc, "", TypeExpr::new_hole());
    }
    arg_type = TypeExpr::new_hole();
    loc = lex.cur_location();
  } else if (lex.tok() != tok_identifier) { // int, cell, [X], etc.
    arg_type = parse_type(lex, forall_list);
  } else if (lex.tok() == tok_identifier) {
    if (forall_list && forall_list->lookup_idx(lex.cur_str()) != -1) {
      arg_type = parse_type(lex, forall_list);
    } else {
      arg_type = TypeExpr::new_hole();
    }
  } else {
    lex.error("Is not a type identifier");
  }
  if (lex.tok() == tok_underscore || lex.tok() == tok_comma || lex.tok() == tok_clpar) {
    if (lex.tok() == tok_underscore) {
      loc = lex.cur_location();
      lex.next();
    }
    return createV<ast_argument>(loc, "", arg_type);
  }
  lex.check(tok_identifier, "parameter name");
  loc = lex.cur_location();
  std::string_view arg_name = lex.cur_str();
  lex.next();
  return createV<ast_argument>(loc, arg_name, arg_type);
}

AnyV parse_global_var_declaration(Lexer& lex) {
  TypeExpr* declared_type = nullptr;
  SrcLocation loc = lex.cur_location();
  if (lex.tok() == tok_underscore) {
    lex.next();
    declared_type = TypeExpr::new_hole();
    loc = lex.cur_location();
  } else if (lex.tok() != tok_identifier) {
    declared_type = parse_type(lex, nullptr);
  }
  lex.check(tok_identifier, "global variable name");
  std::string_view var_name = lex.cur_str();
  lex.next();
  return createV<ast_global_var_declaration>(loc, var_name, declared_type);
}

AnyV parse_expr(Lexer& lex);

AnyV parse_constant_declaration(Lexer& lex) {
  TypeExpr *declared_type = nullptr;
  if (lex.tok() == tok_int) {
    declared_type = TypeExpr::new_atomic(TypeExpr::_Int);
    lex.next();
  } else if (lex.tok() == tok_slice) {
    declared_type = TypeExpr::new_atomic(TypeExpr::_Slice);
    lex.next();
  }
  lex.check(tok_identifier, "constant name");
  SrcLocation loc = lex.cur_location();
  std::string_view const_name = lex.cur_str();
  lex.next();
  lex.expect(tok_assign, "'='");
  AnyV init_value = parse_expr(lex);
  return createV<ast_constant_declaration>(loc, const_name, declared_type, init_value);
}

AnyV parse_argument_list(Lexer& lex, V<ast_forall_list> forall_list) {
  SrcLocation loc = lex.cur_location();
  std::vector<AnyV> args;
  lex.expect(tok_oppar, "argument list");
  if (lex.tok() != tok_clpar) {
    args.push_back(parse_argument(lex, forall_list));
    while (lex.tok() == tok_comma) {
      lex.next();
      args.push_back(parse_argument(lex, forall_list));
    }
  }
  lex.expect(tok_clpar, "')'");
  return createV<ast_argument_list>(loc, std::move(args));
}

AnyV parse_constant_declaration_list(Lexer& lex) {
  std::vector<AnyV> consts;
  SrcLocation loc = lex.cur_location();
  lex.expect(tok_const, "'const'");
  while (true) {
    consts.push_back(parse_constant_declaration(lex));
    if (lex.tok() != tok_comma) {
      break;
    }
    lex.expect(tok_comma, "','");
  }
  lex.expect(tok_semicolon, "';'");
  return createV<ast_constant_declaration_list>(loc, std::move(consts));
}

AnyV parse_global_var_declaration_list(Lexer& lex) {
  std::vector<AnyV> globals;
  SrcLocation loc = lex.cur_location();
  lex.expect(tok_global, "'global'");
  while (true) {
    globals.push_back(parse_global_var_declaration(lex));
    if (lex.tok() != tok_comma) {
      break;
    }
    lex.expect(tok_comma, "','");
  }
  lex.expect(tok_semicolon, "';'");
  return createV<ast_global_var_declaration_list>(loc, std::move(globals));
}

// parse ( E { , E } ) | () | [ E { , E } ] | [] | id | num | _
AnyV parse_expr100(Lexer& lex) {
  SrcLocation loc = lex.cur_location();
  if (lex.tok() == tok_oppar) {
    lex.next();
    if (lex.tok() == tok_clpar) {
      lex.next();
      return createV<ast_tensor>(loc, {});
    }
    AnyV res = parse_expr(lex);
    if (lex.tok() == tok_clpar) {
      lex.next();
      return createV<ast_parenthesized_expr>(loc, res);
    }
    std::vector<AnyV> items;
    bool is_type_expression = res->type == ast_type_expression; // to differ `(a,b)` and `(int,slice)`
    items.emplace_back(res);
    while (lex.tok() == tok_comma) {
      lex.next();
      AnyV item = parse_expr(lex);
      if (is_type_expression != (item->type == ast_type_expression)) {
        lex.error("mixing type and non-type expressions inside the same tuple");
      }
      items.emplace_back(item);
    }
    lex.expect(tok_clpar, "')'");
    if (is_type_expression) {
      std::vector<TypeExpr*> types;
      types.reserve(items.size());
      for (AnyV item : items) {
        types.emplace_back(item->as<ast_type_expression>()->declared_type);
      }
      return createV<ast_type_expression>(loc, TypeExpr::new_tensor(std::move(types)));
    }
    return createV<ast_tensor>(loc, std::move(items));
  }
  if (lex.tok() == tok_opbracket) {
    lex.next();
    if (lex.tok() == tok_clbracket) {
      lex.next();
      return createV<ast_tensor_square>(loc, {});
    }
    AnyV res = parse_expr(lex);
    std::vector<AnyV> items;
    bool is_type_expression = res->type == ast_type_expression; // to differ `(a,b)` and `(int,slice)`
    items.emplace_back(res);
    while (lex.tok() == tok_comma) {
      lex.next();
      AnyV item = parse_expr(lex);
      if (is_type_expression != (item->type == ast_type_expression)) {
        lex.error("mixing type and non-type expressions inside the same tuple");
      }
      items.emplace_back(item);
    }
    lex.expect(tok_clbracket, "']'");
    if (is_type_expression) {
      std::vector<TypeExpr*> types;
      types.reserve(items.size());
      for (AnyV item : items) {
        types.emplace_back(item->as<ast_type_expression>()->declared_type);
      }
      return createV<ast_type_expression>(loc, TypeExpr::new_tuple(TypeExpr::new_tensor(std::move(types))));
    }
    return createV<ast_tensor_square>(loc, std::move(items));
  }
  TokenType t = lex.tok();
  if (t == tok_int_const) {
    std::string_view int_val = lex.cur_str();
    lex.next();
    return createV<ast_int_const>(loc, int_val);
  }
  if (t == tok_string_const) {
    std::string_view str_val = lex.cur_str();
    lex.next();
    char modifier = 0;
    if (lex.tok() == tok_string_modifier) {
      modifier = lex.cur_str()[0];
      lex.next();
    }
    return createV<ast_string_const>(loc, str_val, modifier);
  }
  if (t == tok_underscore) {
    lex.next();
    return createV<ast_underscore>(loc);
  }
  if (t == tok_var) {
    lex.next();
    return createV<ast_type_expression>(loc, TypeExpr::new_hole());
  }
  if (t == tok_int || t == tok_cell || t == tok_slice || t == tok_builder || t == tok_cont || t == tok_tuple) {
    lex.next();
    return createV<ast_type_expression>(loc, TypeExpr::new_atomic(t));
  }
  if (t == tok_true || t == tok_false) {
    lex.next();
    return createV<ast_bool_const>(loc, t == tok_true);
  }
  if (t == tok_nil) {
    lex.next();
    return createV<ast_nil_tuple>(loc);
  }
  if (t == tok_identifier) {
    std::string_view str_val = lex.cur_str();
    lex.next();
    return createV<ast_identifier>(loc, str_val);
  }
  lex.expect(tok_identifier, "identifier");
  return nullptr;
}

// parse E { E }
AnyV parse_expr90(Lexer& lex) {
  AnyV res = parse_expr100(lex);
  while (lex.tok() == tok_oppar || lex.tok() == tok_opbracket || (lex.tok() == tok_identifier && lex.cur_str()[0] != '.' && lex.cur_str()[0] != '~')) {
    if (const auto* v_type_expr = res->try_as<ast_type_expression>()) {
      AnyV dest = parse_expr100(lex);
      return createV<ast_variable_declaration>(v_type_expr->loc, v_type_expr->declared_type, dest);
    } else {
      AnyV arg = parse_expr100(lex);
      return createV<ast_function_call>(res->loc, res, arg);
    }
  }
  return res;
}

// parse E { .method E | ~method E }
AnyV parse_expr80(Lexer& lex) {
  AnyV lhs = parse_expr90(lex);
  while (lex.tok() == tok_identifier && (lex.cur_str()[0] == '.' || lex.cur_str()[0] == '~')) {
    std::string_view method_name = lex.cur_str();
    SrcLocation loc = lex.cur_location();
    lex.next();
    const ASTNodeBase *arg = parse_expr100(lex);
    lhs = createV<ast_dot_tilde_call>(loc, method_name, lhs, arg);
  }
  return lhs;
}

// parse [ ~ | - | + ] E
AnyV parse_expr75(Lexer& lex) {
  TokenType t = lex.tok();
  if (t == tok_bitwise_not || t == tok_minus || t == tok_plus) {
    SrcLocation loc = lex.cur_location();
    std::string_view operator_name = lex.cur_str();
    lex.next();
    AnyV rhs = parse_expr75(lex);
    return createV<ast_unary_operator>(loc, operator_name, t, rhs);
  } else {
    return parse_expr80(lex);
  }
}

// parse E { (* | / | % | /% | ^/ | ~/ | ^% | ~% ) E }
AnyV parse_expr30(Lexer& lex) {
  AnyV lhs = parse_expr75(lex);
  TokenType t = lex.tok();
  while (t == tok_mul || t == tok_div || t == tok_mod || t == tok_divmod || t == tok_divC ||
         t == tok_divR || t == tok_modC || t == tok_modR) {
    SrcLocation loc = lex.cur_location();
    std::string_view operator_name = lex.cur_str();
    lex.next();
    AnyV rhs = parse_expr75(lex);
    lhs = createV<ast_binary_operator>(loc, operator_name, t, lhs, rhs);
    t = lex.tok();
  }
  return lhs;
}

// parse E { (+ | -) E }
AnyV parse_expr20(Lexer& lex) {
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

// parse E { ( << | >> | ~>> | ^>> ) E }
AnyV parse_expr17(Lexer& lex) {
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

// parse E [ (== | < | > | <= | >= | != | <=> ) E ]
AnyV parse_expr15(Lexer& lex) {
  AnyV lhs = parse_expr17(lex);
  TokenType t = lex.tok();
  if (t == tok_eq || t == tok_lt || t == tok_gt || t == tok_leq || t == tok_geq || t == tok_neq || t == tok_spaceship) {
    SrcLocation loc = lex.cur_location();
    std::string_view operator_name = lex.cur_str();
    lex.next();
    AnyV rhs = parse_expr17(lex);
    lhs = createV<ast_binary_operator>(loc, operator_name, t, lhs, rhs);
  }
  return lhs;
}

// parse E { ( & | `|` | ^ ) E }
AnyV parse_expr14(Lexer& lex) {
  AnyV lhs = parse_expr15(lex);
  TokenType t = lex.tok();
  while (t == tok_bitwise_and || t == tok_bitwise_or || t == tok_bitwise_xor) {
    SrcLocation loc = lex.cur_location();
    std::string_view operator_name = lex.cur_str();
    lex.next();
    AnyV rhs = parse_expr15(lex);
    diagnose_bitwise_precedence(loc, operator_name, lhs, rhs);
    lhs = createV<ast_binary_operator>(loc, operator_name, t, lhs, rhs);
    t = lex.tok();
  }
  return lhs;
}

// parse E [ ? E : E ]
AnyV parse_expr13(Lexer& lex) {
  AnyV res = parse_expr14(lex);
  if (lex.tok() == tok_question) {
    SrcLocation loc = lex.cur_location();
    lex.next();
    AnyV when_true = parse_expr(lex);
    lex.expect(tok_colon, "':'");
    AnyV when_false = parse_expr13(lex);
    return createV<ast_ternary_operator>(loc, res, when_true, when_false);
  }
  return res;
}

// parse LE1 (= | += | -= | ... ) E2
AnyV parse_expr10(Lexer& lex) {
  AnyV lhs = parse_expr13(lex);
  TokenType t = lex.tok();
  if (t == tok_set_plus || t == tok_set_minus || t == tok_set_mul || t == tok_set_div || t == tok_set_divR || t == tok_set_divC ||
      t == tok_set_mod || t == tok_set_modC || t == tok_set_modR || t == tok_set_lshift || t == tok_set_rshift || t == tok_set_rshiftC ||
      t == tok_set_rshiftR || t == tok_set_bitwise_and || t == tok_set_bitwise_or || t == tok_set_bitwise_xor ||
      t == tok_assign) {
    SrcLocation loc = lex.cur_location();
    std::string_view operator_name = lex.cur_str();
    lex.next();
    AnyV rhs = parse_expr10(lex);
    return createV<ast_binary_operator>(loc, operator_name, t, lhs, rhs);
  }
  return lhs;
}

AnyV parse_expr(Lexer& lex) {
  return parse_expr10(lex);
}

AnyV parse_return_stmt(Lexer& lex) {
  SrcLocation loc = lex.cur_location();
  lex.expect(tok_return, "'return'");
  AnyV child = parse_expr(lex);
  lex.expect(tok_semicolon, "';'");
  return createV<ast_return_statement>(loc, child);
}

AnyV parse_statement(Lexer& lex);

V<ast_sequence> parse_sequence(Lexer& lex) {
  SrcLocation loc = lex.cur_location();
  lex.expect(tok_opbrace, "'{'");
  std::vector<AnyV> items;
  while (lex.tok() != tok_clbrace) {
    items.push_back(parse_statement(lex));
  }
  SrcLocation loc_end = lex.cur_location();
  lex.expect(tok_clbrace, "'}'");
  return createV<ast_sequence>(loc, loc_end, items);
}

AnyV parse_repeat_statement(Lexer& lex) {
  SrcLocation loc = lex.cur_location();
  lex.expect(tok_repeat, "'repeat'");
  AnyV cond = parse_expr(lex);
  V<ast_sequence> body = parse_sequence(lex);
  return createV<ast_repeat_statement>(loc, cond, body);
}

AnyV parse_while_statement(Lexer& lex) {
  SrcLocation loc = lex.cur_location();
  lex.expect(tok_while, "'while'");
  AnyV cond = parse_expr(lex);
  V<ast_sequence> body = parse_sequence(lex);
  return createV<ast_while_statement>(loc, cond, body);
}

ASTNodeBase* parse_do_until_statement(Lexer& lex) {
  SrcLocation loc = lex.cur_location();
  lex.expect(tok_do, "'do'");
  V<ast_sequence> body = parse_sequence(lex);
  lex.expect(tok_until, "'until'");
  AnyV cond = parse_expr(lex);
  return createV<ast_do_until_statement>(loc, body, cond);
}

AnyV parse_try_catch_statement(Lexer& lex) {
  SrcLocation loc = lex.cur_location();
  lex.expect(tok_try, "'try'");
  V<ast_sequence> try_body = parse_sequence(lex);
  lex.expect(tok_catch, "'catch'");
  AnyV catch_expr = parse_expr(lex);
  V<ast_sequence> catch_body = parse_sequence(lex);
  return createV<ast_try_catch_statement>(loc, try_body, catch_expr, catch_body);
}

AnyV parse_if_statement(Lexer& lex, bool is_ifnot) {
  SrcLocation loc = lex.cur_location();
  lex.next();
  AnyV cond = parse_expr(lex);
  V<ast_sequence> if_body = parse_sequence(lex);
  V<ast_sequence> else_body = nullptr;
  if (lex.tok() == tok_else) {
    lex.next();
    else_body = parse_sequence(lex);
  } else if (lex.tok() == tok_elseif) {
    AnyV v_inner_if = parse_if_statement(lex, false);
    else_body = createV<ast_sequence>(v_inner_if->loc, lex.cur_location(), {v_inner_if});
  } else if (lex.tok() == tok_elseifnot) {
    AnyV v_inner_if = parse_if_statement(lex, true);
    else_body = createV<ast_sequence>(v_inner_if->loc, lex.cur_location(), {v_inner_if});
  } else {
    else_body = createV<ast_sequence>(lex.cur_location(), lex.cur_location(), {});
  }
  return createV<ast_if_statement>(loc, is_ifnot, cond, if_body, else_body);
}

AnyV parse_statement(Lexer& lex) {
  switch (lex.tok()) {
    case tok_return:
      return parse_return_stmt(lex);
    case tok_opbrace:
      return parse_sequence(lex);
    case tok_repeat:
      return parse_repeat_statement(lex);
    case tok_if:
      return parse_if_statement(lex, false);
    case tok_ifnot:
      return parse_if_statement(lex, true);
    case tok_do:
      return parse_do_until_statement(lex);
    case tok_while:
      return parse_while_statement(lex);
    case tok_try:
      return parse_try_catch_statement(lex);
    case tok_semicolon: {
      lex.next();
      return createV<ast_empty>;
    }
    default: {
      AnyV expr = parse_expr(lex);
      lex.expect(tok_semicolon, "';'");
      return expr;
    }
  }
}

AnyV parse_func_body(Lexer& lex) {
  return parse_sequence(lex);
}

AnyV parse_asm_func_body(Lexer& lex, V<ast_argument_list> arg_list) {
  SrcLocation loc = lex.cur_location();
  lex.expect(tok_asm, "'asm'");
  size_t n_args = arg_list->size();
  if (n_args > 16) {
    throw ParseError{loc, "assembler built-in function can have at most 16 arguments"};
  }
  std::vector<int> arg_order, ret_order;
  if (lex.tok() == tok_oppar) {
    lex.next();
    while (lex.tok() == tok_identifier || lex.tok() == tok_int_const) {
      int arg_idx = arg_list->lookup_idx(lex.cur_str());
      if (arg_idx == -1) {
        lex.error("argument name expected");
      }
      arg_order.push_back(arg_idx);
      lex.next();
    }
    if (lex.tok() == tok_mapsto) {
      lex.next();
      while (lex.tok() == tok_int_const) {
        int ret_idx = std::atoi(static_cast<std::string>(lex.cur_str()).c_str());
        ret_order.push_back(ret_idx);
        lex.next();
      }
    }
    lex.expect(tok_clpar, "')'");
  }
  std::vector<AnyV> asm_commands;
  lex.check(tok_string_const, "\"ASM COMMAND\"");
  while (lex.tok() == tok_string_const) {
    std::string_view asm_command = lex.cur_str();
    asm_commands.push_back(createV<ast_string_const>(lex.cur_location(), asm_command, 0));
    lex.next();
  }
  lex.expect(tok_semicolon, "';'");
  return createV<ast_asm_body>(loc, std::move(arg_order), std::move(ret_order), std::move(asm_commands));
}

AnyV parse_forall(Lexer& lex) {
  SrcLocation loc = lex.cur_location();
  std::vector<AnyV> forall_items;
  lex.expect(tok_forall, "'forall'");
  int idx = 0;
  while (true) {
    lex.check(tok_identifier, "T expected");
    std::string_view nameT = lex.cur_str();
    TypeExpr* type = TypeExpr::new_var(idx++);
    forall_items.emplace_back(createV<ast_forall_item>(lex.cur_location(), type, static_cast<std::string>(nameT)));
    lex.next();
    if (lex.tok() != tok_comma) {
      break;
    }
    lex.next();
  }
  lex.expect(tok_mapsto, "'->'");
  return createV<ast_forall_list>{loc, std::move(forall_items)};
}

AnyV parse_function_declaration(Lexer& lex) {
  SrcLocation loc = lex.cur_location();
  V<ast_forall_list> forall_list = nullptr;
  bool is_get_method = false;
  bool is_builtin = false;
  bool marked_as_inline = false;
  bool marked_as_inline_ref = false;
  if (lex.tok() == tok_forall) {
    forall_list = parse_forall(lex)->as<ast_forall_list>();
  } else if (lex.tok() == tok_get) {
    is_get_method = true;
    lex.next();
  }
  TypeExpr* ret_type = parse_type(lex, forall_list);
  lex.check(tok_identifier, "function name identifier expected");
  std::string func_name = static_cast<std::string>(lex.cur_str());
  lex.next();
  V<ast_argument_list> arg_list = parse_argument_list(lex, forall_list)->as<ast_argument_list>();
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
  if (lex.tok() == tok_inline) {
    marked_as_inline = true;
    lex.next();
  } else if (lex.tok() == tok_inlineref) {
    marked_as_inline_ref = true;
    lex.next();
  }
  V<ast_int_const> method_id = nullptr;
  if (lex.tok() == tok_method_id) {
    if (is_get_method) {
      lex.error("both `get` and `method_id` are not allowed");
    }
    lex.next();
    if (lex.tok() == tok_oppar) {  // method_id(N)
      lex.next();
      lex.check(tok_int_const, "number");
      std::string_view int_val = lex.cur_str();
      method_id = createV<ast_int_const>(lex.cur_location(), int_val);
      lex.next();
      lex.expect(tok_clpar, "')'");
    } else {
      static bool warning_shown = false;
      if (!warning_shown) {
        lex.cur_location().show_warning("`method_id` specifier is deprecated, use `get` keyword.\nExample: `get int seqno() { ... }`");
        warning_shown = true;
      }
      is_get_method = true;
    }
  }

  AnyV body = nullptr;

  if (lex.tok() == tok_builtin) {
    is_builtin = true;
    body = createV<ast_empty>;
    lex.next();
    lex.expect(tok_semicolon, "';'");
  } else if (lex.tok() == tok_semicolon) {
    // todo this is just a prototype, remove this "feature" in the future
    lex.next();
    body = createV<ast_empty>;
  } else if (lex.tok() == tok_opbrace) {
    body = parse_func_body(lex);
  } else if (lex.tok() == tok_asm) {
    body = parse_asm_func_body(lex, arg_list);
  } else {
    lex.expect(tok_opbrace, "function body block");
  }

  auto f_declaration = createV<ast_function_declaration>(loc, func_name, arg_list, body);
  f_declaration->ret_type = ret_type;
  f_declaration->forall_list = forall_list;
  f_declaration->marked_as_pure = marked_as_pure;
  f_declaration->marked_as_get_method = is_get_method;
  f_declaration->marked_as_builtin = is_builtin;
  f_declaration->marked_as_inline = marked_as_inline;
  f_declaration->marked_as_inline_ref = marked_as_inline_ref;
  f_declaration->method_id = method_id;
  return f_declaration;
}

AnyV parse_pragma(Lexer& lex) {
  SrcLocation loc = lex.cur_location();
  lex.next_special(tok_pragma_name, "pragma name");
  std::string_view pragma_name = lex.cur_str();
  if (pragma_name == "version") {
    lex.next();
    TokenType cmp_tok = lex.tok();
    bool valid = cmp_tok == tok_gt || cmp_tok == tok_geq || cmp_tok == tok_lt || cmp_tok == tok_leq || cmp_tok == tok_eq || cmp_tok == tok_bitwise_xor;
    if (!valid) {
      lex.error("invalid comparison operator");
    }
    lex.next_special(tok_semver, "semver");
    std::string_view semver = lex.cur_str();
    lex.next();
    lex.expect(tok_semicolon, "';'");
    return createV<ast_pragma_version>(loc, cmp_tok, semver);
  }
  lex.next();
  lex.expect(tok_semicolon, "';'");
  return createV<ast_pragma_no_arg>(loc, pragma_name);
}

AnyV parse_include_statement(Lexer& lex) {
  SrcLocation loc = lex.cur_location();
  lex.expect(tok_include, "#include");
  lex.check(tok_string_const, "source file name");
  std::string_view rel_filename = lex.cur_str();
  if (rel_filename.empty()) {
    lex.error("imported file name is an empty string");
  }
  lex.next();
  lex.expect(tok_semicolon, "';'");
  return createV<ast_include_statement>(loc, rel_filename);
}

// the main (exported) function
AnyV parse_src_file_to_ast(SrcFile* file) {
  file->was_parsed = true;

  std::vector<AnyV> toplevel_declarations;
  Lexer lex(file);
  while (!lex.is_eof()) {
    if (lex.tok() == tok_pragma) {
      toplevel_declarations.push_back(parse_pragma(lex));
    } else if (lex.tok() == tok_include) {
      toplevel_declarations.push_back(parse_include_statement(lex));
    } else if (lex.tok() == tok_global) {
      toplevel_declarations.push_back(parse_global_var_declaration_list(lex));
    } else if (lex.tok() == tok_const) {
      toplevel_declarations.push_back(parse_constant_declaration_list(lex));
    } else {
      toplevel_declarations.push_back(parse_function_declaration(lex));
    }
  }
  return createV<ast_tolk_file>(file, std::move(toplevel_declarations));
}

}  // namespace tolk
