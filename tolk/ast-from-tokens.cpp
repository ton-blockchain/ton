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
#include "ast.h"
#include "compilation-errors.h"
#include "type-system.h"
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

// make an error for a case "flags & 0xFF != 0" (equivalent to "flags & 1", probably unexpected)
// it would better be a warning, but we decided to make it a strict error
static Error err_lower_precedence(std::string_view op_lower, std::string_view op_higher) {
  return err("{} has lower precedence than {}"
              ", probably this code won't work as you expected.  "
              "Use parenthesis: either (... {} ...) to evaluate it first, or (... {} ...) to suppress this error.",
              op_lower, op_higher, op_lower, op_higher);
}

// make an error for a case "arg1 & arg2 | arg3"
static Error err_mix_and_or_no_parenthesis(std::string_view op1, std::string_view op2) {
  return err("mixing {} with {} without parenthesis may lead to accidental errors.  "
              "Use parenthesis to emphasize operator precedence.",
              op1, op2);
}

// make an error "Tolk does not have ++i operator"
static Error err_no_increment_operator() {
  return err("Tolk has no increment operator\n""hint: use `i += 1`, not `i++`");
}

// make an error "Tolk does not have --i operator"
static Error err_no_decrement_operator() {
  return err("Tolk has no decrement operator\n""hint: use `i -= 1`, not `i--`");
}

// diagnose when bitwise operators are used in a probably wrong way due to tricky precedence
// example: "flags & 0xFF != 0" is equivalent to "flags & 1", most likely it's unexpected
// the only way to suppress this error for the programmer is to use parenthesis
// (how do we detect presence of parenthesis? simple: (0!=1) is ast_parenthesized_expr{ast_binary_operator},
//  that's why if rhs->type == ast_binary_operator, it's not surrounded by parenthesis)
static void diagnose_bitwise_precedence(SrcRange range, std::string_view operator_name, AnyExprV lhs, AnyExprV rhs) {
  // handle "flags & 0xFF != 0" (rhs = "0xFF != 0")
  if (rhs->kind == ast_binary_operator && is_comparison_binary_op(rhs->as<ast_binary_operator>()->tok)) {
    err_lower_precedence(operator_name, rhs->as<ast_binary_operator>()->operator_name).fire(range);
  }

  // handle "0 != flags & 0xFF" (lhs = "0 != flags")
  if (lhs->kind == ast_binary_operator && is_comparison_binary_op(lhs->as<ast_binary_operator>()->tok)) {
    err_lower_precedence(operator_name, lhs->as<ast_binary_operator>()->operator_name).fire(range);
  }
}

// similar to above, but detect potentially invalid usage of && and ||
// since anyway, using parenthesis when both && and || occur in the same expression,
// && and || have equal operator precedence in Tolk
static void diagnose_and_or_precedence(SrcRange range, AnyExprV lhs, TokenType rhs_tok, std::string_view rhs_operator_name) {
  if (auto lhs_op = lhs->try_as<ast_binary_operator>()) {
    // handle "arg1 & arg2 | arg3" (lhs = "arg1 & arg2")
    if (is_bitwise_binary_op(lhs_op->tok) && is_bitwise_binary_op(rhs_tok) && lhs_op->tok != rhs_tok) {
      err_mix_and_or_no_parenthesis(lhs_op->operator_name, rhs_operator_name).fire(range);
    }

    // handle "arg1 && arg2 || arg3" (lhs = "arg1 && arg2")
    if (is_logical_binary_op(lhs_op->tok) && is_logical_binary_op(rhs_tok) && lhs_op->tok != rhs_tok) {
      err_mix_and_or_no_parenthesis(lhs_op->operator_name, rhs_operator_name).fire(range);
    }
  }
}

// diagnose "a << 8 + 1" (equivalent to "a << 9", probably unexpected)
static void diagnose_addition_in_bitshift(SrcRange range, std::string_view bitshift_operator_name, AnyExprV rhs) {
  if (rhs->kind == ast_binary_operator && is_add_or_sub_binary_op(rhs->as<ast_binary_operator>()->tok)) {
    err_lower_precedence(bitshift_operator_name, rhs->as<ast_binary_operator>()->operator_name).fire(range);
  }
}

// replace (a == null) and similar to ast_is_type_operator(a, null) (as if `a is null` was written)
static AnyExprV maybe_replace_eq_null_with_isNull_check(V<ast_binary_operator> v) {
  bool lhs_is_null = v->get_lhs()->kind == ast_null_keyword;
  bool rhs_is_null = v->get_rhs()->kind == ast_null_keyword;
  bool replace = (lhs_is_null || rhs_is_null) && (v->tok == tok_eq || v->tok == tok_neq);
  if (!replace) {
    return v;
  }

  AnyExprV v_null_kw = lhs_is_null ? v->get_lhs() : v->get_rhs();
  AnyExprV v_nullable = lhs_is_null ? v->get_rhs() : v->get_lhs();
  AnyTypeV rhs_null_type = createV<ast_type_leaf_text>(v_null_kw->range, "null");
  return createV<ast_is_type_operator>(v->range, v_nullable, rhs_null_type, v->tok == tok_neq);
}

// parse `123` / `0xFF` / `0b10001` to td::RefInt256
static td::RefInt256 parse_tok_int_const(std::string_view text, SrcRange cur_range) {
  bool bin = text[0] == '0' && text[1] == 'b';
  if (!bin) {
    // this function parses decimal and hex numbers
    td::RefInt256 intval = td::string_to_int256(static_cast<std::string>(text));
    if (intval.is_null() || !intval->signed_fits_bits(257)) {
      err("invalid integer constant").fire(cur_range);
    }
    return intval;
  }
  // parse a binary number; to make it simpler, don't allow too long numbers, it's impractical
  if (text.size() > 64 + 2) {
    return {};
  }
  uint64_t result = 0;
  for (char c : text.substr(2)) { // skip "0b"
    result = (result << 1) | static_cast<uint64_t>(c - '0');
  }
  return td::make_refint(result);
}



// --------------------------------------------
//    parsing type from tokens
//
// here we implement parsing types (mostly after colon) to AnyTypeV
// example: `var v: int` is leaf "int"
// example: `var v: (User?, [cell])` is tensor(nullable(leaf "User"), brackets(leaf "cell"))
//
// later, after all symbols are registered, types are resolved to TypePtr, see pipe-resolve-types.cpp
//

static AnyTypeV parse_type_expression(Lexer& lex);

static std::vector<AnyTypeV> parse_nested_type_list(Lexer& lex, TokenType tok_op, const char* s_op, TokenType tok_cl, const char* s_cl, SrcRange& out_range) {
  lex.expect(tok_op, s_op);
  std::vector<AnyTypeV> sub_types;
  while (true) {
    if (lex.tok() == tok_cl) {  // empty lists allowed
      out_range.end(lex.cur_range());
      lex.next();
      break;
    }

    sub_types.emplace_back(parse_type_expression(lex));
    if (lex.tok() == tok_comma) {
      lex.next();
    } else if (lex.tok() != tok_cl) {
      // overcome the `>>` problem, like `Wrapper<Wrapper<int>>`:
      // treat token `>>` like two `>`; consume one here doing nothing (break) and leave the second `>` in a lexer
      if (tok_cl == tok_gt && lex.tok() == tok_rshift) {
        lex.hack_replace_rshift_with_one_triangle();
        out_range.end(lex.cur_range());
        break;
      }
      lex.unexpected(s_cl);
    }
  }
  return sub_types;
}

static AnyTypeV parse_simple_type(Lexer& lex) {
  switch (lex.tok()) {
    case tok_self:
    case tok_identifier:
    case tok_null: {
      SrcRange range = lex.cur_range();
      std::string_view text = lex.cur_str();
      lex.next();
      return createV<ast_type_leaf_text>(range, text);
    }
    case tok_oppar: {
      SrcRange range = lex.range_start();
      std::vector tensor_items = parse_nested_type_list(lex, tok_oppar, "`(`", tok_clpar, "`)` or `,`", range);
      return createV<ast_type_parenthesis_tensor>(range, std::move(tensor_items));
    }
    case tok_opbracket: {
      SrcRange range = lex.range_start();
      std::vector tuple_items = parse_nested_type_list(lex, tok_opbracket, "`[`", tok_clbracket, "`]` or `,`", range);
      return createV<ast_type_bracket_tuple>(range, std::move(tuple_items));
    }
    default:
      lex.unexpected("<type>");
  }
}

static AnyTypeV parse_type_nullable(Lexer& lex) {
  AnyTypeV result = parse_simple_type(lex);

  if (lex.tok() == tok_lt) {    // T1<T2, T3, ...>
    SrcRange range = SrcRange::empty_at_start(result->range);
    auto args = parse_nested_type_list(lex, tok_lt, "`<`", tok_gt, "`>` or `,`", range);
    std::vector<AnyTypeV> outer_and_args;
    outer_and_args.reserve(1 + args.size());
    outer_and_args.push_back(result);
    outer_and_args.insert(outer_and_args.end(), args.begin(), args.end());
    result = createV<ast_type_triangle_args>(range, std::move(outer_and_args));
  }

  if (lex.tok() == tok_question) {
    SrcRange range = SrcRange::overlap(result->range, lex.cur_range());
    lex.next();
    result = createV<ast_type_question_nullable>(range, result);
  }

  return result;
}

static AnyTypeV parse_type_expression(Lexer& lex) {
  if (lex.tok() == tok_bitwise_or) {    // allow leading `|`, like in TypeScript
    lex.next();
  }
  AnyTypeV result = parse_type_nullable(lex);

  if (lex.tok() == tok_bitwise_or) {  // `int | slice`, `Pair2 | (Pair3 | null)`
    std::vector<AnyTypeV> items;
    items.emplace_back(result);
    while (lex.tok() == tok_bitwise_or) {
      lex.next();
      items.emplace_back(parse_type_nullable(lex));
    }
    SrcRange range = SrcRange::overlap(items.front()->range, items.back()->range);
    result = createV<ast_type_vertical_bar_union>(range, std::move(items));
  }

  if (lex.tok() == tok_arrow) {   // `int -> int`, `(cell, slice) -> void`, `int -> int -> int`, `int | cell -> void`
    lex.next();
    std::vector<AnyTypeV> params_and_return;
    if (auto p_tensor = result->try_as<ast_type_parenthesis_tensor>()) {
      params_and_return.reserve(p_tensor->get_items().size());
      params_and_return.insert(params_and_return.begin(), p_tensor->get_items().begin(), p_tensor->get_items().end());
    } else {
      params_and_return.reserve(2);
      params_and_return.push_back(result);
    }
    params_and_return.push_back(parse_type_expression(lex));
    SrcRange range = SrcRange::overlap(params_and_return.front()->range, params_and_return.back()->range);
    result = createV<ast_type_arrow_callable>(range, std::move(params_and_return));
  }

  return result;
}

static AnyTypeV parse_type_from_tokens(Lexer& lex) {
  return parse_type_expression(lex);
}



// --------------------------------------------
//    parsing expressions and statements
//


AnyExprV parse_expr(Lexer& lex);
AnyV parse_statement(Lexer& lex);

static V<ast_identifier> parse_identifier(Lexer& lex, const char* str_expected) {
  lex.check(tok_identifier, str_expected);
  SrcRange range = lex.cur_range();
  std::string_view name = lex.cur_str();
  lex.next();
  return createV<ast_identifier>(range, name);
}

static V<ast_genericsT_list> parse_genericsT_list(Lexer& lex) {
  SrcRange range = lex.range_start();
  std::vector<AnyV> genericsT_items;
  lex.expect(tok_lt, "`<`");
  while (true) {
    lex.check(tok_identifier, "T");
    SrcRange rangeT = lex.cur_range();
    std::string_view nameT = lex.cur_str();
    lex.next();
    AnyTypeV default_type = nullptr;
    if (lex.tok() == tok_assign) {          // <T = int?>
      lex.next();
      default_type = parse_type_expression(lex);
      rangeT.end(default_type->range);
    }
    genericsT_items.emplace_back(createV<ast_genericsT_item>(rangeT, nameT, default_type));
    if (lex.tok() != tok_comma) {
      break;
    }
    lex.next();
  }

  lex.check(tok_gt, "`>`");
  range.end(lex.cur_range());
  lex.next();
  return createV<ast_genericsT_list>(range, std::move(genericsT_items));
}

static AnyV parse_parameter(Lexer& lex, AnyTypeV self_type, bool in_lambda) {
  SrcRange range = lex.range_start();

  // optional keyword `mutate` meaning that a function will mutate a passed argument (like passed by reference)
  bool declared_as_mutate = false;
  if (lex.tok() == tok_mutate) {
    if (in_lambda) {
      lex.error("`mutate` is not available in lambdas");
    }
    lex.next();
    declared_as_mutate = true;
  }

  // parameter name (or underscore for an unnamed parameter)
  V<ast_identifier> v_ident = nullptr;
  bool is_self = false;
  if (lex.tok() == tok_identifier) {
    v_ident = parse_identifier(lex, "parameter name");
  } else if (lex.tok() == tok_self) {
    if (!self_type) {
      lex.error("`self` can only be the first parameter of a method");
    }
    is_self = true;
    v_ident = createV<ast_identifier>(lex.cur_range(), "self");
    lex.next();
  } else if (lex.tok() == tok_underscore) {
    v_ident = createV<ast_identifier>(lex.cur_range(), "");
    lex.next();
  } else {
    lex.unexpected("parameter name");
  }
  range.end(v_ident->range);

  // parameter type after colon is mandatory in declarations, but optional for lambdas
  AnyTypeV param_type = self_type;
  if (lex.tok() == tok_colon) {
    if (is_self) {
      err("`self` parameter should not have a type").fire(v_ident);
    }
    lex.next();
    param_type = parse_type_from_tokens(lex);
    range.end(param_type->range);
  } else if (!is_self && !in_lambda) {
    err("specify a type for a parameter: `{}: <type>`", v_ident->name).fire(v_ident);
  }

  // optional default value
  AnyExprV default_value = nullptr;
  if (lex.tok() == tok_assign && !is_self) {      // `a: int = 0`
    if (declared_as_mutate) {
      lex.error("`mutate` parameter can't have a default value");
    }
    lex.next();
    default_value = parse_expr(lex);
    range.end(default_value->range);
  }

  return createV<ast_parameter>(range, v_ident, param_type, default_value, declared_as_mutate);
}

static AnyV parse_global_var_declaration(Lexer& lex, const std::vector<V<ast_annotation>>& annotations) {
  SrcRange range = lex.range_start();
  lex.expect(tok_global, "`global`");
  auto v_ident = parse_identifier(lex, "global variable name");
  lex.expect(tok_colon, "`:`");
  AnyTypeV declared_type = parse_type_from_tokens(lex);
  range.end(declared_type->range);
  if (lex.tok() == tok_comma) {
    lex.error("multiple declarations are not allowed, split globals on separate lines");
  }
  if (lex.tok() == tok_assign) {
    lex.error("assigning to a global is not allowed at declaration");
  }

  for (auto v_annotation : annotations) {
    switch (v_annotation->kind) {
      case AnnotationKind::deprecated:
      case AnnotationKind::custom:
        break;
      default:
        err("this annotation is not applicable to global").fire(v_annotation);
    }
  }

  return createV<ast_global_var_declaration>(range, v_ident, declared_type);
}

static AnyV parse_constant_declaration(Lexer& lex, const std::vector<V<ast_annotation>>& annotations) {
  SrcRange range = lex.range_start();
  lex.expect(tok_const, "`const`");
  auto v_ident = parse_identifier(lex, "constant name");
  AnyTypeV declared_type = nullptr;
  if (lex.tok() == tok_colon) {
    lex.next();
    declared_type = parse_type_from_tokens(lex);
  }
  lex.expect(tok_assign, "`=`");
  AnyExprV init_value = parse_expr(lex);
  if (lex.tok() == tok_comma) {
    lex.error("multiple declarations are not allowed, split constants on separate lines");
  }

  for (auto v_annotation : annotations) {
    switch (v_annotation->kind) {
      case AnnotationKind::deprecated:
      case AnnotationKind::custom:
        break;
      default:
        err("this annotation is not applicable to constant").fire(v_annotation);
    }
  }

  range.end(init_value->range);
  return createV<ast_constant_declaration>(range, v_ident, declared_type, init_value);
}

static AnyV parse_type_alias_declaration(Lexer& lex, const std::vector<V<ast_annotation>>& annotations) {
  SrcRange range = lex.range_start();
  lex.expect(tok_type, "`type`");
  auto v_ident = parse_identifier(lex, "type name");

  V<ast_genericsT_list> genericsT_list = nullptr;
  if (lex.tok() == tok_lt) {    // 'type Response<TResult, TError>'
    genericsT_list = parse_genericsT_list(lex);
  }

  lex.expect(tok_assign, "`=`");
  if (lex.tok() == tok_builtin) {   // type map<K, V> = builtin
    range.end(lex.cur_range());
    lex.next();
    return createV<ast_empty_statement>(range);
  }

  AnyTypeV underlying_type = parse_type_from_tokens(lex);
  range.end(underlying_type->range);

  for (auto v_annotation : annotations) {
    switch (v_annotation->kind) {
      case AnnotationKind::deprecated:
      case AnnotationKind::custom:
        break;
      default:
        err("this annotation is not applicable to type alias").fire(v_annotation);
    }
  }

  return createV<ast_type_alias_declaration>(range, v_ident, genericsT_list, underlying_type);
}

static AnyExprV parse_var_declaration_lhs(Lexer& lex, bool is_immutable, bool allow_lateinit) {
  if (lex.tok() == tok_oppar) {
    SrcRange range = lex.range_start();
    lex.next();
    AnyExprV first = parse_var_declaration_lhs(lex, is_immutable, false);
    if (lex.tok() == tok_clpar) {
      lex.next();
      return first;
    }
    std::vector<AnyExprV> args(1, first);
    while (lex.tok() == tok_comma) {
      lex.next();
      if (lex.tok() == tok_clpar) {     // trailing comma
        break;
      }
      args.push_back(parse_var_declaration_lhs(lex, is_immutable, false));
    }
    lex.check(tok_clpar, "`)`");
    range.end(lex.cur_range());
    lex.next();
    return createV<ast_tensor>(range, std::move(args));
  }
  if (lex.tok() == tok_opbracket) {
    SrcRange range = lex.range_start();
    lex.next();
    std::vector<AnyExprV> args(1, parse_var_declaration_lhs(lex, is_immutable, false));
    while (lex.tok() == tok_comma) {
      lex.next();
      if (lex.tok() == tok_clbracket) {     // trailing comma
        break;
      }
      args.push_back(parse_var_declaration_lhs(lex, is_immutable, false));
    }
    lex.check(tok_clbracket, "`]`");
    range.end(lex.cur_range());
    lex.next();
    return createV<ast_bracket_tuple>(range, std::move(args));
  }
  if (lex.tok() == tok_identifier) {
    SrcRange range = lex.range_start();
    auto v_ident = parse_identifier(lex, "variable name");
    range.end(v_ident->range);
    AnyTypeV declared_type = nullptr;
    bool marked_as_redef = false;
    bool is_lateinit = false;
    if (lex.tok() == tok_colon) {
      lex.next();
      declared_type = parse_type_from_tokens(lex);
      range.end(declared_type->range);
    } else if (lex.tok() == tok_redef) {
      lex.next();
      marked_as_redef = true;
    }
    if (lex.tok() == tok_semicolon && allow_lateinit) {
      if (declared_type == nullptr) {
        lex.error("provide a type for a variable, because its default value is omitted:\n> var " + static_cast<std::string>(v_ident->name) + ": <type>;");
      }
      is_lateinit = true;
    }
    return createV<ast_local_var_lhs>(range, v_ident, declared_type, is_immutable, is_lateinit, marked_as_redef);
  }
  if (lex.tok() == tok_underscore) {
    SrcRange range = lex.cur_range();
    AnyTypeV declared_type = nullptr;
    lex.next();
    if (lex.tok() == tok_colon) {
      lex.next();
      declared_type = parse_type_from_tokens(lex);
      range.end(declared_type->range);
    }
    return createV<ast_local_var_lhs>(range, createV<ast_identifier>(range, ""), declared_type, true, false, false);
  }
  lex.unexpected("variable name");
}

static AnyExprV parse_local_vars_declaration(Lexer& lex, bool allow_lateinit) {
  SrcRange range = lex.range_start();
  bool is_immutable = lex.tok() == tok_val;
  lex.next();

  AnyExprV lhs = parse_var_declaration_lhs(lex, is_immutable, allow_lateinit);
  if (lex.tok() != tok_assign) {
    if (auto lhs_var = lhs->try_as<ast_local_var_lhs>(); lhs_var && lhs_var->is_lateinit) {
      return lhs;   // just ast_local_var_lhs inside AST tree
    }
    lex.error("variables declaration must be followed by assignment: `var xxx = ...`");
  }
  lex.next();
  AnyExprV rhs = parse_expr(lex);

  if (lex.tok() == tok_comma) {
    lex.error("multiple declarations are not allowed, split variables on separate lines");
  }
  range.end(rhs->range);
  return createV<ast_assign>(range, createV<ast_local_vars_declaration>(range, lhs), rhs);
}

// "parameters" are at function declaration: `fun f(param1: int, mutate param2: slice)`
// for methods like `fun builder.storeUint(self, i: int)`, receiver_type = builder (type of self)
static V<ast_parameter_list> parse_parameter_list(Lexer& lex, AnyTypeV receiver_type, bool in_lambda) {
  SrcRange range = lex.range_start();
  std::vector<AnyV> params;
  lex.expect(tok_oppar, "parameter list");
  if (lex.tok() != tok_clpar) {
    params.push_back(parse_parameter(lex, receiver_type, in_lambda));
    while (lex.tok() == tok_comma) {
      lex.next();
      if (lex.tok() == tok_clpar) {     // trailing comma
        break;
      }
      params.push_back(parse_parameter(lex, nullptr, in_lambda));
    }
  }
  lex.check(tok_clpar, "`)`");
  range.end(lex.cur_range());
  lex.next();
  return createV<ast_parameter_list>(range, std::move(params));
}

// "arguments" are at function call: `f(arg1, mutate arg2)`
static AnyExprV parse_argument(Lexer& lex) {
  SrcRange range = lex.range_start();

  // keyword `mutate` is necessary when a parameter is declared `mutate` (to make mutation obvious for the reader)
  bool passed_as_mutate = false;
  if (lex.tok() == tok_mutate) {
    lex.next();
    passed_as_mutate = true;
  }

  AnyExprV expr = parse_expr(lex);
  range.end(expr->range);
  return createV<ast_argument>(range, expr, passed_as_mutate);
}

static V<ast_argument_list> parse_argument_list(Lexer& lex) {
  SrcRange range = lex.range_start();
  std::vector<AnyExprV> args;
  lex.expect(tok_oppar, "`(`");
  if (lex.tok() != tok_clpar) {
    args.push_back(parse_argument(lex));
    while (lex.tok() == tok_comma) {
      lex.next();
      if (lex.tok() == tok_clpar) {   // trailing comma
        break;
      }
      args.push_back(parse_argument(lex));
    }
  }

  lex.check(tok_clpar, "`)`");
  range.end(lex.cur_range());
  lex.next();
  return createV<ast_argument_list>(range, std::move(args));
}

static V<ast_instantiationT_list> parse_maybe_instantiationTs_after_identifier(Lexer& lex) {
  lex.check(tok_lt, "`<`");
  Lexer::SavedPositionForLookahead backup = lex.save_parsing_position();
  try {
    SrcRange range = lex.range_start();
    lex.next();
    std::vector<AnyV> instantiationTs;
    AnyTypeV type_node = parse_type_from_tokens(lex);
    instantiationTs.push_back(createV<ast_instantiationT_item>(type_node->range, type_node));
    while (lex.tok() == tok_comma) {
      lex.next();
      type_node = parse_type_from_tokens(lex);
      instantiationTs.push_back(createV<ast_instantiationT_item>(type_node->range, type_node));
    }
    lex.check(tok_gt, "`>`");
    range.end(lex.cur_range());
    lex.next();
    return createV<ast_instantiationT_list>(range, std::move(instantiationTs));
  } catch (const ThrownParseError&) {
    lex.restore_position(backup);
    return nullptr;
  }
}

static V<ast_block_statement> parse_block_statement(Lexer& lex) {
  SrcRange range = lex.range_start();
  lex.expect(tok_opbrace, "`{`");
  std::vector<AnyV> items;
  while (lex.tok() != tok_clbrace) {
    AnyV v = parse_statement(lex);
    items.push_back(v);
    if (lex.tok() == tok_clbrace) {
      break;
    }
    bool does_end_with_brace =
             v->kind == ast_if_statement || v->kind == ast_while_statement || v->kind == ast_match_expression
          || v->kind == ast_try_catch_statement || v->kind == ast_repeat_statement || v->kind == ast_block_statement;
    if (!does_end_with_brace) {
      lex.expect(tok_semicolon, "`;`");
    }
  }
  lex.check(tok_clbrace, "`}`");
  range.end(lex.cur_range());
  lex.next();
  return createV<ast_block_statement>(range, std::move(items));
}

static V<ast_object_field> parse_object_field(Lexer& lex) {
  SrcRange range = lex.range_start();
  auto v_ident = parse_identifier(lex, "field name");
  range.end(v_ident->range);

  if (lex.tok() == tok_comma || lex.tok() == tok_clbrace) {
    auto v_same_ident = createV<ast_identifier>(v_ident->range, v_ident->name);
    auto v_same_expr = createV<ast_reference>(v_ident->range, v_same_ident, nullptr);
    return createV<ast_object_field>(range, v_ident, v_same_expr);
  }

  lex.expect(tok_colon, "`:`");
  AnyExprV init_val = parse_expr(lex);
  range.end(init_val->range);
  return createV<ast_object_field>(range, v_ident, init_val);
}

static V<ast_object_body> parse_object_body(Lexer& lex) {
  SrcRange range = lex.range_start();
  lex.expect(tok_opbrace, "`{`");

  std::vector<AnyExprV> fields;
  while (lex.tok() != tok_clbrace) {
    fields.push_back(parse_object_field(lex));
    if (lex.tok() == tok_comma) {
      lex.next();
    } else if (lex.tok() != tok_clbrace) {
      lex.unexpected("`,`");
    }
  }

  lex.check(tok_clbrace, "`}`");
  range.end(lex.cur_range());
  lex.next();
  return createV<ast_object_body>(range, std::move(fields));
}

// `throw code` / `throw (code)` / `throw (code, arg)`
// it's technically a statement (can't occur "in any place of expression"),
// but inside `match` arm it can appear without semicolon: `pattern => throw 123`
static AnyV parse_throw_expression(Lexer& lex) {
  SrcRange range = lex.range_start();
  lex.expect(tok_throw, "`throw`");

  AnyExprV thrown_code, thrown_arg;
  if (lex.tok() == tok_oppar) {   // throw (code) or throw (code, arg)
    lex.next();
    thrown_code = parse_expr(lex);
    if (lex.tok() == tok_comma) {
      lex.next();
      thrown_arg = parse_expr(lex);
    } else {
      thrown_arg = createV<ast_empty_expression>(SrcRange::empty_at_start(range));
    }
    lex.check(tok_clpar, "`)`");
    range.end(lex.cur_range());
    lex.next();
  } else {   // throw code
    thrown_code = parse_expr(lex);
    range.end(thrown_code->range);
    thrown_arg = createV<ast_empty_expression>(SrcRange::empty_at_start(range));
  }

  return createV<ast_throw_statement>(range, thrown_code, thrown_arg);
}

// `pattern => body` inside `match`
static V<ast_match_arm> parse_match_arm(Lexer& lex) {
  SrcRange range = lex.range_start();
  MatchArmKind pattern_kind = static_cast<MatchArmKind>(-1);
  AnyTypeV exact_type = nullptr;
  AnyExprV pattern_expr = nullptr;

  Lexer::SavedPositionForLookahead backup = lex.save_parsing_position();
  try {
    exact_type = parse_type_from_tokens(lex);
    pattern_kind = MatchArmKind::exact_type;
  } catch (const ThrownParseError&) {
  }
  if (!exact_type || lex.tok() != tok_double_arrow) {
    exact_type = nullptr;
    pattern_kind = static_cast<MatchArmKind>(-1);
    lex.restore_position(backup);
    try {
      pattern_expr = parse_expr(lex);
      pattern_kind = MatchArmKind::const_expression;    // any expr at parsing, should result in const int/bool
    } catch (const ThrownParseError&) {
    }
  }
  if (!exact_type && !pattern_expr && lex.tok() == tok_else) {
    lex.next();
    pattern_kind = MatchArmKind::else_branch;
  }

  if (pattern_kind == static_cast<MatchArmKind>(-1)) {
    lex.restore_position(backup);
    lex.error("expected <type> or <expression> in `match` before `=>`");
  }
  lex.expect(tok_double_arrow, "`=>`");

  V<ast_braced_expression> body = nullptr;
  if (lex.tok() == tok_opbrace) {         // pattern => { ... }
    AnyV v_block = parse_statement(lex);
    body = createV<ast_braced_expression>(v_block->range, v_block);
  } else if (lex.tok() == tok_throw) {    // pattern => throw 123 (allow without braces)
    AnyV v_throw = parse_throw_expression(lex);
    AnyV v_block = createV<ast_block_statement>(v_throw->range, {v_throw});
    body = createV<ast_braced_expression>(v_block->range, v_block);
  } else if (lex.tok() == tok_return) {   // pattern => return 123 (allow without braces, like throw)
    SrcRange block_range = lex.range_start();
    lex.next();
    AnyExprV return_value = parse_expr(lex);
    block_range.end(return_value->range);
    AnyV v_return = createV<ast_return_statement>(block_range, return_value);
    AnyV v_block = createV<ast_block_statement>(v_return->range, {v_return});
    body = createV<ast_braced_expression>(v_block->range, v_block);
  } else {
    AnyExprV unbraced_expr = parse_expr(lex);
    AnyV v_block = createV<ast_block_statement>(unbraced_expr->range, {createV<ast_braced_yield_result>(unbraced_expr->range, unbraced_expr)});
    body = createV<ast_braced_expression>(unbraced_expr->range, v_block);
  }

  range.end(body->range);
  if (pattern_expr == nullptr) {  // for match by type / default case, empty vertex, not nullptr
    pattern_expr = createV<ast_empty_expression>(SrcRange::empty_at_start(range));
  }
  return createV<ast_match_arm>(range, pattern_kind, exact_type, pattern_expr, body);
}

static V<ast_match_expression> parse_match_expression(Lexer& lex) {
  SrcRange range = lex.range_start();
  lex.expect(tok_match, "`match`");

  lex.expect(tok_oppar, "`(`");
  AnyExprV subject = lex.tok() == tok_var || lex.tok() == tok_val       // `match (var x = rhs)`
                ? parse_local_vars_declaration(lex, false)
                : parse_expr(lex);
  lex.expect(tok_clpar, "`)`");

  std::vector<AnyExprV> subject_and_arms = {subject};
  lex.expect(tok_opbrace, "`{`");
  while (lex.tok() != tok_clbrace) {
    auto v_arm = parse_match_arm(lex);
    subject_and_arms.push_back(v_arm);

    // after `pattern => { ... }` comma is optional, after `pattern => expr` mandatory
    bool was_comma = lex.tok() == tok_comma;    // trailing comma is allowed always
    bool was_unbraced = v_arm->get_body()->get_block_statement()->size() == 1 && v_arm->get_body()->get_block_statement()->get_item(0)->kind == ast_braced_yield_result;
    if (was_comma) {
      lex.next();
    }
    if (lex.tok() == tok_clbrace) {
      break;
    }
    if (!was_comma && was_unbraced) {
      lex.unexpected("`,`");
    }
  }

  lex.check(tok_clbrace, "`}`");
  range.end(lex.cur_range());
  lex.next();
  return createV<ast_match_expression>(range, std::move(subject_and_arms));
}

static V<ast_lambda_fun> parse_lambda_fun_expression(Lexer& lex) {
  SrcRange range = lex.range_start();
  lex.expect(tok_fun, "`fun`");
  
  V<ast_parameter_list> v_param_list = parse_parameter_list(lex, nullptr, true);

  AnyTypeV ret_type = nullptr;
  if (lex.tok() == tok_colon) {   // fun(...): <ret_type>
    lex.next();
    ret_type = parse_type_from_tokens(lex);
  }

  auto v_body = parse_block_statement(lex);
  range.end(v_body->range);
  return createV<ast_lambda_fun>(range, v_param_list, v_body, ret_type); 
}

static V<ast_lazy_operator> parse_lazy_operator(Lexer& lex) {
  SrcRange range = lex.range_start();
  lex.expect(tok_lazy, "`lazy`");

  AnyExprV expr = parse_expr(lex);
  range.end(expr->range);
  return createV<ast_lazy_operator>(range, expr);
}

// parse (expr) / [expr] / identifier / number
static AnyExprV parse_expr100(Lexer& lex) {
  switch (lex.tok()) {
    case tok_oppar: {
      SrcRange range = lex.range_start();
      lex.next();
      if (lex.tok() == tok_clpar) {
        range.end(lex.cur_range());
        lex.next();
        return createV<ast_tensor>(range, {});
      }
      AnyExprV first = parse_expr(lex);
      if (lex.tok() == tok_clpar) {
        range.end(lex.cur_range());
        lex.next();
        return createV<ast_parenthesized_expression>(range, first);
      }
      std::vector<AnyExprV> items(1, first);
      while (lex.tok() == tok_comma) {
        lex.next();
        if (lex.tok() == tok_clpar) {   // trailing comma
          break;
        }
        items.emplace_back(parse_expr(lex));
      }
      lex.check(tok_clpar, "`)`");
      range.end(lex.cur_range());
      lex.next();
      if (items.size() == 1) {      // we can reach here for 1 element with trailing comma: `(item, )`
        return items[0];            // then just return item, not a 1-element tensor,
      }                             // since 1-element tensors won't be type compatible with item's type
      return createV<ast_tensor>(range, std::move(items));
    }
    case tok_opbracket: {
      SrcRange range = lex.range_start();
      lex.next();
      if (lex.tok() == tok_clbracket) {
        range.end(lex.cur_range());
        lex.next();
        return createV<ast_bracket_tuple>(range, {});
      }
      std::vector<AnyExprV> items(1, parse_expr(lex));
      while (lex.tok() == tok_comma) {
        lex.next();
        if (lex.tok() == tok_clbracket) {   // trailing comma
          break;
        }
        items.emplace_back(parse_expr(lex));
      }
      lex.check(tok_clbracket, "`]`");
      range.end(lex.cur_range());
      lex.next();
      return createV<ast_bracket_tuple>(range, std::move(items));
    }
    case tok_int_const: {
      SrcRange range = lex.cur_range();
      std::string_view orig_str = lex.cur_str();
      td::RefInt256 intval = parse_tok_int_const(orig_str, lex.cur_range());
      lex.next();
      return createV<ast_int_const>(range, std::move(intval), orig_str);
    }
    case tok_string_const: {
      SrcRange range = lex.cur_range();
      std::string_view str_val = lex.cur_str();     // with surrounding quotes, remove them
      if (str_val.starts_with(R"(""")")) {        // multi-line literal: begins-ends with 3 quotes
        str_val = str_val.substr(3, str_val.size() - 6);
      } else {
        str_val = str_val.substr(1, str_val.size() - 2);
      }
      lex.next();
      return createV<ast_string_const>(range, str_val);
    }
    case tok_underscore: {
      SrcRange range = lex.cur_range();
      lex.next();
      return createV<ast_underscore>(range);
    }
    case tok_true: {
      SrcRange range = lex.cur_range();
      lex.next();
      return createV<ast_bool_const>(range, true);
    }
    case tok_false: {
      SrcRange range = lex.cur_range();
      lex.next();
      return createV<ast_bool_const>(range, false);
    }
    case tok_null: {
      SrcRange range = lex.cur_range();
      lex.next();
      return createV<ast_null_keyword>(range);
    }
    case tok_self: {
      SrcRange range = lex.cur_range();
      lex.next();
      auto v_ident = createV<ast_identifier>(range, "self");
      return createV<ast_reference>(range, v_ident, nullptr);
    }
    case tok_identifier: {
      auto v_ident = parse_identifier(lex, "identifier");
      SrcRange range = v_ident->range;
      V<ast_instantiationT_list> v_instantiationTs = nullptr;
      if (lex.tok() == tok_lt) {
        v_instantiationTs = parse_maybe_instantiationTs_after_identifier(lex);
        if (v_instantiationTs) {
          range.end(v_instantiationTs->range);
        }
      }
      if (lex.tok() == tok_opbrace) {
        AnyTypeV type_node = createV<ast_type_leaf_text>(v_ident->range, v_ident->name);  // `Pair { ... }`
        if (v_instantiationTs) {                                                          // `Pair<int> { ... }`
          std::vector<AnyTypeV> ident_and_args;
          ident_and_args.reserve(1 + v_instantiationTs->size());
          ident_and_args.push_back(type_node);
          for (int i = 0; i < v_instantiationTs->size(); ++i) {
            ident_and_args.push_back(v_instantiationTs->get_item(i)->type_node);
          }
          SrcRange tri_range = SrcRange::overlap(v_ident->range, v_instantiationTs->range);
          type_node = createV<ast_type_triangle_args>(tri_range, std::move(ident_and_args));
        }
        auto body = parse_object_body(lex);
        range.end(body->range);
        return createV<ast_object_literal>(range, type_node, body);
      }
      return createV<ast_reference>(range, v_ident, v_instantiationTs);
    }
    case tok_opbrace: {
      auto body = parse_object_body(lex);
      return createV<ast_object_literal>(body->range, nullptr, body);
    }
    case tok_match:
      return parse_match_expression(lex);
    case tok_fun:
      return parse_lambda_fun_expression(lex);
    case tok_lazy:
      return parse_lazy_operator(lex);
    default:
      lex.unexpected("<expression>");
  }
}

// parse E(...) / E! / E++ / E-- having parsed E already (left-to-right)
static AnyExprV parse_fun_call_postfix(Lexer& lex, AnyExprV lhs) {
  while (true) {
    if (lex.tok() == tok_oppar) {
      auto argument_list = parse_argument_list(lex);
      SrcRange range = SrcRange::overlap(lhs->range, argument_list->range);
      lhs = createV<ast_function_call>(range, lhs, argument_list);
    } else if (lex.tok() == tok_logical_not) {
      SrcRange range = SrcRange::overlap(lhs->range, lex.cur_range());
      lex.next();
      lhs = createV<ast_not_null_operator>(range, lhs);
    } else if (lex.tok() == tok_double_plus || lex.tok() == tok_double_minus) {
      lex.tok() == tok_double_plus ? err_no_increment_operator().fire(lex.cur_range()) : err_no_decrement_operator().fire(lex.cur_range());
    } else {
      break;
    }
  }
  return lhs;
}

// parse E(...) and E! (left-to-right)
static AnyExprV parse_expr90(Lexer& lex) {
  AnyExprV res = parse_expr100(lex);
  if (lex.tok() == tok_oppar || lex.tok() == tok_logical_not || lex.tok() == tok_double_plus || lex.tok() == tok_double_minus) {
    res = parse_fun_call_postfix(lex, res);
  }
  return res;
}

// parse E.field and E.method(...) and E.field! (left-to-right)
static AnyExprV parse_expr80(Lexer& lex) {
  AnyExprV lhs = parse_expr90(lex);
  while (lex.tok() == tok_dot) {
    SrcRange range(lhs->range);
    lex.next();
    V<ast_identifier> v_ident = nullptr;
    V<ast_instantiationT_list> v_instantiationTs = nullptr;
    if (lex.tok() == tok_identifier) {    // obj.field / obj.method
      v_ident = parse_identifier(lex, "field name");
      range.end(v_ident->range);
      if (lex.tok() == tok_lt) {          // obj.method<int>
        v_instantiationTs = parse_maybe_instantiationTs_after_identifier(lex);
        if (v_instantiationTs) {
          range.end(v_instantiationTs->range);
        }
      }
    } else if (lex.tok() == tok_int_const) {  // obj.0 (indexed access)
      SrcRange idx_range = lex.cur_range();
      std::string_view idx_name = lex.cur_str();
      v_ident = createV<ast_identifier>(idx_range, idx_name);
      range.end(v_ident->range);
      lex.next();
    } else {
      lex.unexpected("method name");
    }
    lhs = createV<ast_dot_access>(range, lhs, v_ident, v_instantiationTs);
    if (lex.tok() == tok_oppar || lex.tok() == tok_logical_not || lex.tok() == tok_double_plus || lex.tok() == tok_double_minus) {
      lhs = parse_fun_call_postfix(lex, lhs);
    }
  }
  return lhs;
}

// parse ! ~ - + E (unary)
static AnyExprV parse_expr75(Lexer& lex) {
  TokenType t = lex.tok();
  if (t == tok_logical_not || t == tok_bitwise_not || t == tok_minus || t == tok_plus) {
    SrcRange range = lex.range_start();
    SrcRange operator_range = lex.cur_range();
    std::string_view operator_name = lex.cur_str();
    lex.next();
    AnyExprV rhs = parse_expr75(lex);
    range.end(rhs->range);

    // convert `-1` to `int(-1)`, not to a tree `unary(-) > int(1)` right here 
    if (auto rhs_int = rhs->try_as<ast_int_const>(); rhs_int && (t == tok_minus || t == tok_plus)) {
      td::RefInt256 intval = rhs_int->intval;
      tolk_assert(!intval.is_null());
      if (t == tok_minus) {
        intval = -intval; // negation (and multiple consecutive negations) always fits 257 bits if originally fits
      }
      return createV<ast_int_const>(range, std::move(intval), rhs_int->orig_str);
    }
    return createV<ast_unary_operator>(range, operator_range, operator_name, t, rhs);
  }
  if (t == tok_double_minus || t == tok_double_plus) {
    SrcRange range = lex.cur_range();
    lex.next();
    parse_expr75(lex);
    t == tok_double_plus ? err_no_increment_operator().fire(range) : err_no_decrement_operator().fire(range);
  }
  return parse_expr80(lex);
}

// parse E as / is / !is <type> (left-to-right)
static AnyExprV parse_expr40(Lexer& lex) {
  AnyExprV lhs = parse_expr75(lex);
  TokenType t = lex.tok();
  while (t == tok_as || t == tok_is) {
    lex.next();
    AnyTypeV rhs_type = parse_type_from_tokens(lex);
    SrcRange range = SrcRange::overlap(lhs->range, rhs_type->range);
    if (t == tok_as) {
      lhs = createV<ast_cast_as_operator>(range, lhs, rhs_type);
    } else {
      bool is_negated = lhs->kind == ast_not_null_operator;   // `a !is ...`, now lhs = `a!`
      if (is_negated) {
        lhs = lhs->as<ast_not_null_operator>()->get_expr();
      }
      lhs = createV<ast_is_type_operator>(range, lhs, rhs_type, is_negated);
    }
    t = lex.tok();
  }
  return lhs;
}

// parse E * / % ^/ ~/ E (left-to-right)
static AnyExprV parse_expr30(Lexer& lex) {
  AnyExprV lhs = parse_expr40(lex);
  TokenType t = lex.tok();
  while (t == tok_mul || t == tok_div || t == tok_mod || t == tok_divC || t == tok_divR) {
    SrcRange operator_range = lex.cur_range();
    std::string_view operator_name = lex.cur_str();
    lex.next();
    AnyExprV rhs = parse_expr40(lex);
    SrcRange range = SrcRange::overlap(lhs->range, rhs->range);
    lhs = createV<ast_binary_operator>(range, operator_range, operator_name, t, lhs, rhs);
    t = lex.tok();
  }
  return lhs;
}

// parse E + - E (left-to-right)
static AnyExprV parse_expr20(Lexer& lex) {
  AnyExprV lhs = parse_expr30(lex);
  TokenType t = lex.tok();
  while (t == tok_minus || t == tok_plus) {
    SrcRange operator_range = lex.cur_range();
    std::string_view operator_name = lex.cur_str();
    lex.next();
    AnyExprV rhs = parse_expr30(lex);
    SrcRange range = SrcRange::overlap(lhs->range, rhs->range);
    lhs = createV<ast_binary_operator>(range, operator_range, operator_name, t, lhs, rhs);
    t = lex.tok();
  }
  return lhs;
}

// parse E << >> ~>> ^>> E (left-to-right)
static AnyExprV parse_expr17(Lexer& lex) {
  AnyExprV lhs = parse_expr20(lex);
  TokenType t = lex.tok();
  while (t == tok_lshift || t == tok_rshift || t == tok_rshiftC || t == tok_rshiftR) {
    SrcRange operator_range = lex.cur_range();
    std::string_view operator_name = lex.cur_str();
    lex.next();
    AnyExprV rhs = parse_expr20(lex);
    SrcRange range = SrcRange::overlap(lhs->range, rhs->range);
    diagnose_addition_in_bitshift(range, operator_name, rhs);
    lhs = createV<ast_binary_operator>(range, operator_range, operator_name, t, lhs, rhs);
    t = lex.tok();
  }
  return lhs;
}

// parse E == < > <= >= != <=> E (left-to-right)
static AnyExprV parse_expr15(Lexer& lex) {
  AnyExprV lhs = parse_expr17(lex);
  TokenType t = lex.tok();
  if (t == tok_eq || t == tok_lt || t == tok_gt || t == tok_leq || t == tok_geq || t == tok_neq || t == tok_spaceship) {
    SrcRange operator_range = lex.cur_range();
    std::string_view operator_name = lex.cur_str();
    lex.next();
    AnyExprV rhs = parse_expr17(lex);
    SrcRange range = SrcRange::overlap(lhs->range, rhs->range);
    lhs = createV<ast_binary_operator>(range, operator_range, operator_name, t, lhs, rhs);
    if (t == tok_eq || t == tok_neq) {
      lhs = maybe_replace_eq_null_with_isNull_check(lhs->as<ast_binary_operator>());
    }
  }
  return lhs;
}

// parse E & | ^ E (left-to-right)
static AnyExprV parse_expr14(Lexer& lex) {
  AnyExprV lhs = parse_expr15(lex);
  TokenType t = lex.tok();
  while (t == tok_bitwise_and || t == tok_bitwise_or || t == tok_bitwise_xor) {
    SrcRange operator_range = lex.cur_range();
    std::string_view operator_name = lex.cur_str();
    lex.next();
    AnyExprV rhs = parse_expr15(lex);
    SrcRange range = SrcRange::overlap(lhs->range, rhs->range);
    diagnose_bitwise_precedence(range, operator_name, lhs, rhs);
    diagnose_and_or_precedence(range, lhs, t, operator_name);
    lhs = createV<ast_binary_operator>(range, operator_range, operator_name, t, lhs, rhs);
    t = lex.tok();
  }
  return lhs;
}

// parse E && || E (left-to-right)
static AnyExprV parse_expr13(Lexer& lex) {
  AnyExprV lhs = parse_expr14(lex);
  TokenType t = lex.tok();
  while (t == tok_logical_and || t == tok_logical_or) {
    SrcRange operator_range = lex.cur_range();
    std::string_view operator_name = lex.cur_str();
    lex.next();
    AnyExprV rhs = parse_expr14(lex);
    SrcRange range = SrcRange::overlap(lhs->range, rhs->range);
    diagnose_and_or_precedence(range, lhs, t, operator_name);
    lhs = createV<ast_binary_operator>(range, operator_range, operator_name, t, lhs, rhs);
    t = lex.tok();
  }
  return lhs;
}

// parse E = += -= E and E ? E : E (right-to-left)
static AnyExprV parse_expr10(Lexer& lex) {
  AnyExprV lhs = parse_expr13(lex);
  TokenType t = lex.tok();
  if (t == tok_assign) {
    lex.next();
    AnyExprV rhs = parse_expr10(lex);
    SrcRange range = SrcRange::overlap(lhs->range, rhs->range);
    return createV<ast_assign>(range, lhs, rhs);
  }
  if (t == tok_set_plus || t == tok_set_minus || t == tok_set_mul || t == tok_set_div ||
      t == tok_set_mod || t == tok_set_lshift || t == tok_set_rshift ||
      t == tok_set_bitwise_and || t == tok_set_bitwise_or || t == tok_set_bitwise_xor) {
    SrcRange operator_range = lex.cur_range();
    std::string_view operator_name = lex.cur_str().substr(0, lex.cur_str().size() - 1);   // "+" for +=
    lex.next();
    AnyExprV rhs = parse_expr10(lex);
    SrcRange range = SrcRange::overlap(lhs->range, rhs->range);
    return createV<ast_set_assign>(range, operator_range, operator_name, t, lhs, rhs);
  }
  if (t == tok_question) {
    lex.next();
    AnyExprV when_true = parse_expr10(lex);
    lex.expect(tok_colon, "`:`");
    AnyExprV when_false = parse_expr10(lex);
    SrcRange range = SrcRange::overlap(lhs->range, when_false->range);
    return createV<ast_ternary_operator>(range, lhs, when_true, when_false);
  }
  return lhs;
}

AnyExprV parse_expr(Lexer& lex) {
  return parse_expr10(lex);
}

static AnyV parse_return_statement(Lexer& lex) {
  lex.check(tok_return, "`return`");
  SrcRange range = lex.cur_range();
  lex.next();

  AnyExprV child = nullptr;
  if (lex.tok() == tok_semicolon || lex.tok() == tok_clbrace) {
    child = createV<ast_empty_expression>(SrcRange::empty_at_end(range));
  } else {
    child = parse_expr(lex);
    range.end(child->range);
  }
  return createV<ast_return_statement>(range, child);
}

static AnyV parse_if_statement(Lexer& lex) {
  SrcRange range = lex.range_start();
  lex.expect(tok_if, "`if`");

  lex.expect(tok_oppar, "`(`");
  AnyExprV cond = parse_expr(lex);
  lex.expect(tok_clpar, "`)`");

  V<ast_block_statement> if_body = parse_block_statement(lex);
  V<ast_block_statement> else_body = nullptr;
  if (lex.tok() == tok_else) {  // else if(e) { } or else { }
    lex.next();
    if (lex.tok() == tok_if) {
      AnyV v_inner_if = parse_if_statement(lex);
      else_body = createV<ast_block_statement>(v_inner_if->range, {v_inner_if});
    } else {
      else_body = parse_block_statement(lex);
    }
  } else {  // no 'else', create empty block
    else_body = createV<ast_block_statement>(SrcRange::empty_at_end(if_body->range), {});
  }
  range.end(else_body->range);
  return createV<ast_if_statement>(range, false, cond, if_body, else_body);
}

static AnyV parse_repeat_statement(Lexer& lex) {
  SrcRange range = lex.range_start();
  lex.expect(tok_repeat, "`repeat`");
  lex.expect(tok_oppar, "`(`");
  AnyExprV cond = parse_expr(lex);
  lex.expect(tok_clpar, "`)`");
  V<ast_block_statement> body = parse_block_statement(lex);
  range.end(body->range);
  return createV<ast_repeat_statement>(range, cond, body);
}

static AnyV parse_while_statement(Lexer& lex) {
  SrcRange range = lex.range_start();
  lex.expect(tok_while, "`while`");
  lex.expect(tok_oppar, "`(`");
  AnyExprV cond = parse_expr(lex);
  lex.expect(tok_clpar, "`)`");
  V<ast_block_statement> body = parse_block_statement(lex);
  range.end(body->range);
  return createV<ast_while_statement>(range, cond, body);
}

static AnyV parse_do_while_statement(Lexer& lex) {
  SrcRange range = lex.range_start();
  lex.expect(tok_do, "`do`");
  V<ast_block_statement> body = parse_block_statement(lex);
  lex.expect(tok_while, "`while`");
  lex.expect(tok_oppar, "`(`");
  AnyExprV cond = parse_expr(lex);

  lex.check(tok_clpar, "`)`");
  range.end(lex.cur_range());
  lex.next();
  return createV<ast_do_while_statement>(range, body, cond);
}

static AnyExprV parse_catch_variable(Lexer& lex) {
  if (lex.tok() == tok_identifier) {
    auto v_ident = parse_identifier(lex, "catch variable");
    return createV<ast_reference>(v_ident->range, v_ident, nullptr);
  }
  if (lex.tok() == tok_underscore) {
    auto v_ident = createV<ast_identifier>(lex.cur_range(), "");
    lex.next();
    return createV<ast_reference>(v_ident->range, v_ident, nullptr);
  }
  lex.unexpected("identifier");
}

static AnyExprV create_catch_underscore_variable(const Lexer& lex) {
  auto v_ident = createV<ast_identifier>(SrcRange::empty_at_start(lex.cur_range()), "");
  return createV<ast_reference>(v_ident->range, v_ident, nullptr);
}

static AnyV parse_assert_statement(Lexer& lex) {
  SrcRange range = lex.range_start();
  lex.expect(tok_assert, "`assert`");

  lex.expect(tok_oppar, "`(`");
  AnyExprV cond = parse_expr(lex);
  AnyExprV thrown_code;
  if (lex.tok() == tok_comma) {   // assert(cond, code)
    lex.next();
    thrown_code = parse_expr(lex);
    lex.check(tok_clpar, "`)`");
    range.end(lex.cur_range());
    lex.next();
  } else {  // assert(cond) throw code
    lex.expect(tok_clpar, "`)`");
    lex.expect(tok_throw, "`throw excNo` after assert");
    thrown_code = parse_expr(lex);
    range.end(thrown_code->range);
  }

  return createV<ast_assert_statement>(range, cond, thrown_code);
}

static AnyV parse_try_catch_statement(Lexer& lex) {
  SrcRange range = lex.range_start();
  lex.expect(tok_try, "`try`");
  V<ast_block_statement> try_body = parse_block_statement(lex);

  std::vector<AnyExprV> catch_args;
  lex.expect(tok_catch, "`catch`");
  SrcRange catch_range = lex.range_start();
  if (lex.tok() == tok_oppar) {
    lex.next();
    catch_args.push_back(parse_catch_variable(lex));
    if (lex.tok() == tok_comma) { // catch (excNo, arg)
      lex.next();
      catch_args.push_back(parse_catch_variable(lex));
    } else {  // catch (excNo) -> catch (excNo, _)
      catch_args.push_back(create_catch_underscore_variable(lex));
    }
    lex.check(tok_clpar, "`)`");
    catch_range.end(lex.cur_range());
    lex.next();
  } else {  // catch -> catch (_, _)
    catch_args.push_back(create_catch_underscore_variable(lex));
    catch_args.push_back(create_catch_underscore_variable(lex));
    catch_range = SrcRange::empty_at_start(lex.cur_range());
  }
  V<ast_tensor> catch_expr = createV<ast_tensor>(catch_range, std::move(catch_args));

  V<ast_block_statement> catch_body = parse_block_statement(lex);
  range.end(catch_body->range);
  return createV<ast_try_catch_statement>(range, try_body, catch_expr, catch_body);
}

AnyV parse_statement(Lexer& lex) {
  switch (lex.tok()) {
    case tok_var:   // `var x = 0` is technically an expression, but can not appear in "any place",
    case tok_val:   // only as a separate declaration
      return parse_local_vars_declaration(lex, true);
    case tok_opbrace:
      return parse_block_statement(lex);
    case tok_return:
      return parse_return_statement(lex);
    case tok_if:
      return parse_if_statement(lex);
    case tok_repeat:
      return parse_repeat_statement(lex);
    case tok_do:
      return parse_do_while_statement(lex);
    case tok_while:
      return parse_while_statement(lex);
    case tok_throw:
      return parse_throw_expression(lex);
    case tok_assert:
      return parse_assert_statement(lex);
    case tok_try:
      return parse_try_catch_statement(lex);
    case tok_semicolon:
      return createV<ast_empty_statement>(lex.cur_range());
    case tok_break:
    case tok_continue:
      lex.error("break/continue from loops are not supported yet");
    default:
      return parse_expr(lex);
  }
}


// --------------------------------------------
//    parsing top-level declarations
//


static AnyV parse_asm_func_body(Lexer& lex, V<ast_identifier> name_ident, V<ast_parameter_list> param_list) {
  SrcRange range = lex.range_start();
  lex.expect(tok_asm, "`asm`");
  size_t n_params = param_list->size();
  if (n_params > 16) {
    err("assembler built-in function can have at most 16 arguments").fire(name_ident);
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
      while (lex.tok() == tok_int_const && lex.cur_str().size() < 6) {
        int ret_idx = std::stoi(static_cast<std::string>(lex.cur_str()));
        ret_order.push_back(ret_idx);
        lex.next();
      }
    }
    lex.expect(tok_clpar, "`)`");
  }
  std::vector<AnyV> asm_commands;
  lex.check(tok_string_const, "\"ASM COMMAND\"");
  while (lex.tok() == tok_string_const) {
    asm_commands.push_back(parse_expr(lex));
  }
  range.end(asm_commands.back()->range);
  return createV<ast_asm_body>(range, std::move(arg_order), std::move(ret_order), std::move(asm_commands));
}

static V<ast_annotation> parse_annotation(Lexer& lex) {
  SrcRange range = lex.cur_range();
  lex.check(tok_annotation_at, "`@`");
  std::string_view name = lex.cur_str();
  AnnotationKind kind = Vertex<ast_annotation>::parse_kind(name);
  lex.next();

  V<ast_tensor> v_arg = nullptr;
  if (lex.tok() == tok_oppar) {
    SrcRange range_args = lex.range_start();
    lex.next();
    std::vector<AnyExprV> args;
    args.push_back(parse_expr(lex));
    while (lex.tok() == tok_comma) {
      lex.next();
      if (lex.tok() == tok_clpar) {   // trailing comma
        break;
      }
      args.push_back(parse_expr(lex));
    }
    lex.check(tok_clpar, "`)`");
    range_args.end(lex.cur_range());
    v_arg = createV<ast_tensor>(range_args, std::move(args));
    lex.next();
  }

  switch (kind) {
    case AnnotationKind::unknown:
      err("unknown annotation {}", name).fire(range);
    case AnnotationKind::inline_simple:
    case AnnotationKind::inline_ref:
    case AnnotationKind::noinline:
    case AnnotationKind::pure:
      if (v_arg) {
        err("arguments aren't allowed for {}", name).fire(range);
      }
      v_arg = createV<ast_tensor>(range, {});
      break;
    case AnnotationKind::deprecated:
    case AnnotationKind::custom:
      // allowed with and without arguments; it's IDE-only, the compiler doesn't analyze @deprecated
      break;
    case AnnotationKind::method_id:
      if (!v_arg || v_arg->size() != 1 || v_arg->get_item(0)->kind != ast_int_const) {
        err("expecting `(number)` after {}", name).fire(range);
      }
      break;
    case AnnotationKind::overflow1023_policy:
    case AnnotationKind::on_bounced_policy: {
      if (!v_arg || v_arg->size() != 1 || v_arg->get_item(0)->kind != ast_string_const) {
        err("expecting `(\"policy_name\")` after {}", name).fire(range);
      }
      break;
    }
  }

  if (v_arg == nullptr) {
    v_arg = createV<ast_tensor>(SrcRange::empty_at_end(range), {});
  }
  range.end(v_arg->range);
  return createV<ast_annotation>(range, name, kind, v_arg);
}

static AnyV parse_function_declaration(Lexer& lex, const std::vector<V<ast_annotation>>& annotations, bool is_contract_getter) {
  SrcRange range = lex.range_start();
  lex.expect(tok_fun, "`fun`");

  AnyTypeV receiver_type = nullptr;
  auto backup = lex.save_parsing_position();
  try {
    receiver_type = parse_type_expression(lex);
    lex.expect(tok_dot, "");
  } catch (const ThrownParseError&) {
    receiver_type = nullptr;
    lex.restore_position(backup);
  }

  lex.check(tok_identifier, "function name identifier");

  std::string_view f_name = lex.cur_str();
  bool is_entrypoint = !receiver_type && (
        f_name == "main" || f_name == "onInternalMessage" || f_name == "onExternalMessage" ||
        f_name == "onRunTickTock" || f_name == "onSplitPrepare" || f_name == "onSplitInstall" ||
        f_name == "onBouncedMessage");
  bool is_FunC_entrypoint = !receiver_type && (
        f_name == "recv_internal" || f_name == "recv_external" ||
        f_name == "run_ticktock" || f_name == "split_prepare" || f_name == "split_install");
  if (is_FunC_entrypoint) {
    lex.error("this is a reserved FunC/Fift identifier; you need `onInternalMessage`");
  }

  auto v_ident = createV<ast_identifier>(lex.cur_range(), f_name);
  lex.next();

  V<ast_genericsT_list> genericsT_list = nullptr;
  if (lex.tok() == tok_lt) {    // 'fun f<T1,T2>'
    genericsT_list = parse_genericsT_list(lex);
  }

  V<ast_parameter_list> v_param_list = parse_parameter_list(lex, receiver_type, false);
  bool accepts_self = !v_param_list->empty() && v_param_list->get_param(0)->get_name() == "self";
  int n_mutate_params = v_param_list->get_mutate_params_count();

  AnyTypeV ret_type = nullptr;
  bool returns_self = false;
  if (lex.tok() == tok_colon) {   // : <ret_type> (if absent, it means "auto infer", not void)
    lex.next();
    if (lex.tok() == tok_self) {
      if (!accepts_self) {
        lex.error("only a member function can return `self` (which accepts `self` first parameter)");
      }
      returns_self = true;
      ret_type = createV<ast_type_leaf_text>(lex.cur_range(), "void");
      lex.next();
    } else {
      ret_type = parse_type_from_tokens(lex);
    }
  }

  if (is_entrypoint && (is_contract_getter || genericsT_list || n_mutate_params)) {
    err("invalid declaration of a reserved function").fire(range);
  }
  if (is_contract_getter && (genericsT_list || n_mutate_params || receiver_type)) {
    err("invalid declaration of a get method").fire(range);
  }

  AnyV v_body = nullptr;

  if (lex.tok() == tok_builtin) {
    v_body = createV<ast_empty_statement>(lex.cur_range());
    lex.next();
  } else if (lex.tok() == tok_opbrace) {
    v_body = parse_block_statement(lex);
  } else if (lex.tok() == tok_asm) {
    if (!ret_type) {
      lex.error("asm function must specify return type");
    }
    v_body = parse_asm_func_body(lex, v_ident, v_param_list);
  } else {
    lex.unexpected("{ function body }");
  }

  int flags = 0;
  if (is_entrypoint) {
    flags |= FunctionData::flagIsEntrypoint;
  }
  if (is_contract_getter) {
    flags |= FunctionData::flagContractGetter;
  }
  if (accepts_self) {
    flags |= FunctionData::flagAcceptsSelf;
  }
  if (returns_self) {
    flags |= FunctionData::flagReturnsSelf;
  }

  int tvm_method_id = FunctionData::EMPTY_TVM_METHOD_ID;
  FunctionInlineMode inline_mode = FunctionInlineMode::notCalculated;
  for (auto v_annotation : annotations) {
    switch (v_annotation->kind) {
      case AnnotationKind::inline_simple:
        inline_mode = FunctionInlineMode::inlineViaFif;   // maybe will be replaced by inlineInPlace later
        break;
      case AnnotationKind::inline_ref:
        inline_mode = FunctionInlineMode::inlineRef;
        break;
      case AnnotationKind::noinline:
        inline_mode = FunctionInlineMode::noInline;
        break;
      case AnnotationKind::pure:
        flags |= FunctionData::flagMarkedAsPure;
        break;
      case AnnotationKind::method_id: {
        if (is_contract_getter || genericsT_list || receiver_type || is_entrypoint || n_mutate_params || accepts_self) {
          err("@method_id can be specified only for regular functions").fire(v_annotation);
        }
        auto v_int = v_annotation->get_arg()->get_item(0)->as<ast_int_const>();
        if (v_int->intval.is_null() || !v_int->intval->signed_fits_bits(32)) {
          err("invalid integer constant").fire(v_int);
        }
        tvm_method_id = static_cast<int>(v_int->intval->to_long());
        break;
      }
      case AnnotationKind::on_bounced_policy: {
        std::string_view str = v_annotation->get_arg()->get_item(0)->as<ast_string_const>()->str_val;
        if (str == "manual") {
          flags |= FunctionData::flagManualOnBounce;
        } else {
          err("incorrect value for {}", v_annotation->name).fire(v_annotation);
        }
        if (f_name != "onInternalMessage") {
          err("this annotation is applicable only to onInternalMessage()").fire(v_annotation);
        }
        break;
      }
      case AnnotationKind::deprecated:
      case AnnotationKind::custom:
        break;

      default:
        err("this annotation is not applicable to functions").fire(v_annotation);
    }
  }

  range.end(v_body->range);
  return createV<ast_function_declaration>(range, v_ident, v_param_list, v_body, receiver_type, ret_type, genericsT_list, tvm_method_id, flags, inline_mode);
}

static AnyV parse_struct_field(Lexer& lex) {
  SrcRange range = lex.range_start();

  bool is_private = false;
  if (lex.tok() == tok_private) {
    lex.next();
    is_private = true;
  }

  bool is_readonly = false;
  if (lex.tok() == tok_readonly) {    // `private readonly` ok, `readonly private` not
    lex.next();
    is_readonly = true;
  }
  
  auto v_ident = parse_identifier(lex, "field name");
  lex.expect(tok_colon, "`: <type>`");
  AnyTypeV declared_type = parse_type_from_tokens(lex);
  range.end(declared_type->range);

  AnyExprV default_value = nullptr;
  if (lex.tok() == tok_assign) {    // `id: int = 3`
    lex.next();
    default_value = parse_expr(lex);
    range.end(default_value->range);
  }

  return createV<ast_struct_field>(range, v_ident, is_private, is_readonly, default_value, declared_type);
}

static V<ast_struct_body> parse_struct_body(Lexer& lex, V<ast_identifier> name_ident) {
  SrcRange range = lex.range_start();
  std::vector<AnyV> fields;

  if (lex.tok() == tok_opbrace) {   // `struct A` equal to `struct A {}`
    lex.next();
    while (lex.tok() != tok_clbrace) {
      fields.push_back(parse_struct_field(lex));
      if (lex.tok() == tok_comma || lex.tok() == tok_semicolon) {
        lex.next();
      }
    }
    lex.check(tok_clbrace, "`}`");
    range.end(lex.cur_range());
    lex.next();
  } else {
    range = name_ident->range;
  }

  return createV<ast_struct_body>(range, std::move(fields));
}

static AnyV parse_struct_declaration(Lexer& lex, const std::vector<V<ast_annotation>>& annotations) {
  SrcRange range = lex.range_start();
  lex.expect(tok_struct, "`struct`");

  AnyExprV opcode = nullptr;
  if (lex.tok() == tok_oppar) {     // struct(0x0012) CounterIncrement
    lex.next();
    lex.check(tok_int_const, "opcode `0x...` or `0b...`");
    std::string_view opcode_str = lex.cur_str();
    if (!opcode_str.starts_with("0x") && !opcode_str.starts_with("0b")) {
      lex.unexpected("opcode `0x...` or `0b...`");
    }
    SrcRange opcode_range = lex.cur_range();
    td::RefInt256 intval = parse_tok_int_const(opcode_str, opcode_range);
    opcode = createV<ast_int_const>(opcode_range, std::move(intval), opcode_str);
    lex.next();
    lex.expect(tok_clpar, "`)`");
  } else {
    opcode = createV<ast_empty_expression>(SrcRange::empty_at_start(range));
  }

  auto v_ident = parse_identifier(lex, "identifier");

  V<ast_genericsT_list> genericsT_list = nullptr;
  if (lex.tok() == tok_lt) {    // 'struct Wrapper<T>'
    genericsT_list = parse_genericsT_list(lex);
  }

  StructData::Overflow1023Policy overflow1023_policy = StructData::Overflow1023Policy::not_specified;
  for (auto v_annotation : annotations) {
    switch (v_annotation->kind) {
      case AnnotationKind::deprecated:
      case AnnotationKind::custom:
        break;
      case AnnotationKind::overflow1023_policy: {
        std::string_view str = v_annotation->get_arg()->get_item(0)->as<ast_string_const>()->str_val;
        if (str == "suppress") {
          overflow1023_policy = StructData::Overflow1023Policy::suppress;
        } else {
          err("incorrect value for {}", v_annotation->name).fire(v_annotation);
        }
        break;
      }
      default:
        err("this annotation is not applicable to struct").fire(v_annotation);
    }
  }

  auto body = parse_struct_body(lex, v_ident);
  range.end(body->range);
  return createV<ast_struct_declaration>(range, v_ident, genericsT_list, overflow1023_policy, opcode, body);
}

static AnyV parse_enum_member(Lexer& lex) {
  SrcRange range = lex.range_start();
  auto v_ident = parse_identifier(lex, "member name");
  range.end(v_ident->range);

  AnyExprV init_value = nullptr;
  if (lex.tok() == tok_assign) {    // `Red = 1`
    lex.next();
    init_value = parse_expr(lex);
    range.end(init_value->range);
  }

  return createV<ast_enum_member>(range, v_ident, init_value);  
}

static V<ast_enum_body> parse_enum_body(Lexer& lex) {
  SrcRange range = lex.range_start();
  lex.expect(tok_opbrace, "`{`");

  std::vector<AnyV> members;
  while (lex.tok() != tok_clbrace) {
    members.push_back(parse_enum_member(lex));
    if (lex.tok() == tok_comma || lex.tok() == tok_semicolon) {
      lex.next();
    }
  }

  lex.check(tok_clbrace, "`}`");
  range.end(lex.cur_range());
  lex.next();
  return createV<ast_enum_body>(range, std::move(members));
}

static AnyV parse_enum_declaration(Lexer& lex, const std::vector<V<ast_annotation>>& annotations) {
  SrcRange range = lex.range_start();
  lex.expect(tok_enum, "`enum`");

  auto v_ident = parse_identifier(lex, "identifier");

  AnyTypeV colon_type = nullptr;
  if (lex.tok() == tok_colon) {   // enum Role: int8
    lex.next();
    colon_type = parse_type_expression(lex);
  }

  for (auto v_annotation : annotations) {
    switch (v_annotation->kind) {
      case AnnotationKind::deprecated:
      case AnnotationKind::custom:
        break;
      default:
        err("this annotation is not applicable to enum").fire(v_annotation);
    }
  }

  auto body = parse_enum_body(lex);
  range.end(body->range);
  return createV<ast_enum_declaration>(range, v_ident, colon_type, body);
}

static AnyV parse_tolk_required_version(Lexer& lex) {
  SrcRange range = lex.range_start();
  lex.next_special(tok_semver, "semver");   // syntax: "tolk 0.6"
  std::string semver = static_cast<std::string>(lex.cur_str());
  range.end(lex.cur_range());
  lex.next();

  // for simplicity, there is no syntax ">= version" and so on, just strict compare
  if (TOLK_VERSION != semver && TOLK_VERSION != semver + ".0") {    // 0.6 = 0.6.0
    err("the contract is written in Tolk v{}, but you use Tolk compiler v{}; probably, it will lead to compilation errors or hash changes", semver, TOLK_VERSION).warning(range, nullptr);
  }

  return createV<ast_tolk_required_version>(range, semver);  // semicolon is not necessary
}

static AnyV parse_import_directive(Lexer& lex) {
  SrcRange range = lex.range_start();
  lex.expect(tok_import, "`import`");
  lex.check(tok_string_const, "source file name");
  auto v_str = parse_expr(lex)->as<ast_string_const>();
  std::string_view rel_filename = v_str->str_val;
  if (rel_filename.empty()) {
    lex.error("imported file name is an empty string");
  }
  range.end(v_str->range);
  return createV<ast_import_directive>(range, v_str);
}


// --------------------------------------------
//    parse .tolk source file to AST
//    (the main, exported, function)
//

AnyV parse_src_file_to_ast(const SrcFile* file) {
  std::vector<AnyV> toplevel_declarations;
  std::vector<V<ast_annotation>> annotations;
  Lexer lex(file);
  SrcRange range = lex.range_start();

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
        toplevel_declarations.push_back(parse_import_directive(lex));
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
      case tok_type:
        toplevel_declarations.push_back(parse_type_alias_declaration(lex, annotations));
        annotations.clear();
        break;
      case tok_fun:
        toplevel_declarations.push_back(parse_function_declaration(lex, annotations, false));
        annotations.clear();
        break;
      case tok_struct:
        toplevel_declarations.push_back(parse_struct_declaration(lex, annotations));
        annotations.clear();
        break;
      case tok_enum:
        toplevel_declarations.push_back(parse_enum_declaration(lex, annotations));
        annotations.clear();
        break;

      case tok_export:
      case tok_operator:
      case tok_infix:
        err("`{}` is not supported yet", lex.cur_str()).fire(lex.cur_range());

      case tok_identifier:
        if (lex.cur_str() == "get") {     // top-level "get", contract getter
          lex.next();
          toplevel_declarations.push_back(parse_function_declaration(lex, annotations, true));
          annotations.clear();
          break;
        }
        // fallthrough
      default:
        lex.unexpected("top-level declaration");
    }
  }

  range.end(toplevel_declarations.empty() ? lex.cur_range() : toplevel_declarations.back()->range);
  return createV<ast_tolk_file>(file, range, std::move(toplevel_declarations));
}

}  // namespace tolk
