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
#pragma once

#include <string>
#include "fwd-declarations.h"
#include "platform-utils.h"
#include "src-file.h"
#include "type-expr.h"
#include "lexer.h"
#include "symtable.h"

/*
 *   Here we introduce AST representation of Tolk source code.
 *   Historically, in FunC, there was no AST: while lexing, symbols were registered, types were inferred, and so on.
 * There was no way to perform any more or less semantic analysis.
 *   In Tolk, I've implemented parsing .tolk files into AST at first, and then converting this AST
 * into legacy representation (see pipe-ast-to-legacy.cpp).
 *   In the future, more and more code analysis will be moved out of legacy to AST-level.
 *
 *   From the user's point of view, all AST vertices are constant. All API is based on constancy.
 * Even though fields of vertex structs are public, they can't be modified, since vertices are accepted by const ref.
 *   Generally, there are three ways of accepting a vertex:
 *   * AnyV (= const ASTNodeBase*)
 *     the only you can do with this vertex is to see v->type (ASTNodeType) and to cast via v->as<node_type>()
 *   * AnyExprV (= const ASTNodeExpressionBase*)
 *     in contains expression-specific properties (lvalue/rvalue, inferred type)
 *   * V<node_type> (= const Vertex<node_type>*)
 *     a specific type of vertex, you can use its fields and methods
 *   There is one way of creating a vertex:
 *   * createV<node_type>(...constructor_args)   (= new Vertex<node_type>(...))
 *     vertices are currently created on a heap, without any custom memory arena, just allocated and never deleted
 *   The only way to modify a field is to use "mutate()" method (drops constancy, the only point of mutation)
 *   and then to call "assign_*" method, like "assign_sym", "assign_src_file", etc.
 *
 *   Having AnyV and knowing its node_type, a call
 *     v->as<node_type>()
 *   will return a typed vertex.
 *   There is also a shorthand v->try_as<node_type>() which returns V<node_type> or nullptr if types don't match:
 *     if (auto v_int = v->try_as<ast_int_const>())
 *   Note, that there casts are NOT DYNAMIC. ASTNode is not a virtual base, it has no vtable.
 *   So, as<...>() is just a compile-time casting, without any runtime overhead.
 *
 *   Note, that ASTNodeBase doesn't store any vector of children. That's why there is no way to loop over
 * a random (unknown) vertex. Only a concrete Vertex<node_type> stores its children (if any).
 *   Hence, to iterate over a custom vertex (e.g., a function body), one should inherit some kind of ASTVisitor.
 *   Besides read-only visiting, there is a "visit and replace" pattern.
 *   See ast-visitor.h and ast-replacer.h.
 */

namespace tolk {

enum ASTNodeType {
  ast_empty_statement,
  ast_empty_expression,
  ast_parenthesized_expression,
  ast_tensor,
  ast_tensor_square,
  ast_identifier,
  ast_int_const,
  ast_string_const,
  ast_bool_const,
  ast_null_keyword,
  ast_self_keyword,
  ast_argument,
  ast_argument_list,
  ast_function_call,
  ast_dot_method_call,
  ast_global_var_declaration,
  ast_constant_declaration,
  ast_underscore,
  ast_unary_operator,
  ast_binary_operator,
  ast_ternary_operator,
  ast_return_statement,
  ast_sequence,
  ast_repeat_statement,
  ast_while_statement,
  ast_do_while_statement,
  ast_throw_statement,
  ast_assert_statement,
  ast_try_catch_statement,
  ast_if_statement,
  ast_genericsT_item,
  ast_genericsT_list,
  ast_parameter,
  ast_parameter_list,
  ast_asm_body,
  ast_annotation,
  ast_function_declaration,
  ast_local_var,
  ast_local_vars_declaration,
  ast_tolk_required_version,
  ast_import_statement,
  ast_tolk_file,
};

enum class AnnotationKind {
  inline_simple,
  inline_ref,
  method_id,
  pure,
  deprecated,
  unknown,
};

template<ASTNodeType node_type>
struct Vertex;

template<ASTNodeType node_type>
using V = const Vertex<node_type>*;

#define createV new Vertex

struct UnexpectedASTNodeType final : std::exception {
  AnyV v_unexpected;
  std::string message;

  explicit UnexpectedASTNodeType(AnyV v_unexpected, const char* place_where);

  const char* what() const noexcept override {
    return message.c_str();
  }
};

// ---------------------------------------------------------

struct ASTNodeBase {
  const ASTNodeType type;
  const SrcLocation loc;

  ASTNodeBase(ASTNodeType type, SrcLocation loc) : type(type), loc(loc) {}

  template<ASTNodeType node_type>
  V<node_type> as() const {
#ifdef TOLK_DEBUG
    if (type != node_type) {
      throw Fatal("v->as<...> to wrong node_type");
    }
#endif
    return static_cast<V<node_type>>(this);
  }

  template<ASTNodeType node_type>
  V<node_type> try_as() const {
    return type == node_type ? static_cast<V<node_type>>(this) : nullptr;
  }

#ifdef TOLK_DEBUG
  std::string to_debug_string() const { return to_debug_string(false); }
  std::string to_debug_string(bool colored) const;
  void debug_print() const;
#endif

  GNU_ATTRIBUTE_NORETURN GNU_ATTRIBUTE_COLD
  void error(const std::string& err_msg) const;
};

struct ASTNodeExpressionBase : ASTNodeBase {
  TypeExpr* inferred_type = nullptr;  // todo make it const
  bool is_rvalue: 1 = false;
  bool is_lvalue: 1 = false;

  ASTNodeExpressionBase* mutate() const { return const_cast<ASTNodeExpressionBase*>(this); }
  void assign_inferred_type(TypeExpr* type);
  void assign_rvalue_true();
  void assign_lvalue_true();

  ASTNodeExpressionBase(ASTNodeType type, SrcLocation loc) : ASTNodeBase(type, loc) {}
};

struct ASTNodeStatementBase : ASTNodeBase {
  ASTNodeStatementBase(ASTNodeType type, SrcLocation loc) : ASTNodeBase(type, loc) {}
};

struct ASTExprLeaf : ASTNodeExpressionBase {
  friend class ASTVisitor;
  friend class ASTReplacer;

protected:
  ASTExprLeaf(ASTNodeType type, SrcLocation loc)
    : ASTNodeExpressionBase(type, loc) {}
};

struct ASTExprUnary : ASTNodeExpressionBase {
  friend class ASTVisitor;
  friend class ASTReplacer;

protected:
  AnyExprV child;

  ASTExprUnary(ASTNodeType type, SrcLocation loc, AnyExprV child)
    : ASTNodeExpressionBase(type, loc), child(child) {}
};

struct ASTExprBinary : ASTNodeExpressionBase {
  friend class ASTVisitor;
  friend class ASTReplacer;

protected:
  AnyExprV lhs;
  AnyExprV rhs;

  ASTExprBinary(ASTNodeType type, SrcLocation loc, AnyExprV lhs, AnyExprV rhs)
    : ASTNodeExpressionBase(type, loc), lhs(lhs), rhs(rhs) {}
};

struct ASTExprVararg : ASTNodeExpressionBase {
  friend class ASTVisitor;
  friend class ASTReplacer;

protected:
  std::vector<AnyExprV> children;

  ASTExprVararg(ASTNodeType type, SrcLocation loc, std::vector<AnyExprV> children)
    : ASTNodeExpressionBase(type, loc), children(std::move(children)) {}

public:
  int size() const { return static_cast<int>(children.size()); }
  bool empty() const { return children.empty(); }
};

struct ASTStatementUnary : ASTNodeStatementBase {
  friend class ASTVisitor;
  friend class ASTReplacer;

protected:
  AnyV child;

  AnyExprV child_as_expr() const { return reinterpret_cast<AnyExprV>(child); }

  ASTStatementUnary(ASTNodeType type, SrcLocation loc, AnyV child)
    : ASTNodeStatementBase(type, loc), child(child) {}
};

struct ASTStatementVararg : ASTNodeStatementBase {
  friend class ASTVisitor;
  friend class ASTReplacer;

protected:
  std::vector<AnyV> children;

  AnyV child(int i) const { return children.at(i); }
  AnyExprV child_as_expr(int i) const { return reinterpret_cast<AnyExprV>(children.at(i)); }

  ASTStatementVararg(ASTNodeType type, SrcLocation loc, std::vector<AnyV> children)
    : ASTNodeStatementBase(type, loc), children(std::move(children)) {}

public:
  int size() const { return static_cast<int>(children.size()); }
  bool empty() const { return children.empty(); }
};

struct ASTOtherLeaf : ASTNodeBase {
  friend class ASTVisitor;
  friend class ASTReplacer;

protected:
  ASTOtherLeaf(ASTNodeType type, SrcLocation loc)
    : ASTNodeBase(type, loc) {}
};

struct ASTOtherVararg : ASTNodeBase {
  friend class ASTVisitor;
  friend class ASTReplacer;

protected:
  std::vector<AnyV> children;

  AnyV child(int i) const { return children.at(i); }

  ASTOtherVararg(ASTNodeType type, SrcLocation loc, std::vector<AnyV> children)
    : ASTNodeBase(type, loc), children(std::move(children)) {}

public:
  int size() const { return static_cast<int>(children.size()); }
  bool empty() const { return children.empty(); }
};

// ---------------------------------------------------------

template<>
struct Vertex<ast_empty_statement> final : ASTStatementVararg {
  explicit Vertex(SrcLocation loc)
    : ASTStatementVararg(ast_empty_statement, loc, {}) {}
};

template<>
struct Vertex<ast_empty_expression> final : ASTExprLeaf {
  explicit Vertex(SrcLocation loc)
    : ASTExprLeaf(ast_empty_expression, loc) {}
};

template<>
struct Vertex<ast_parenthesized_expression> final : ASTExprUnary {
  AnyExprV get_expr() const { return child; }

  Vertex(SrcLocation loc, AnyExprV expr)
    : ASTExprUnary(ast_parenthesized_expression, loc, expr) {}
};

template<>
struct Vertex<ast_tensor> final : ASTExprVararg {
  const std::vector<AnyExprV>& get_items() const { return children; }
  AnyExprV get_item(int i) const { return children.at(i); }

  Vertex(SrcLocation loc, std::vector<AnyExprV> items)
    : ASTExprVararg(ast_tensor, loc, std::move(items)) {}
};

template<>
struct Vertex<ast_tensor_square> final : ASTExprVararg {
  const std::vector<AnyExprV>& get_items() const { return children; }
  AnyExprV get_item(int i) const { return children.at(i); }

  Vertex(SrcLocation loc, std::vector<AnyExprV> items)
    : ASTExprVararg(ast_tensor_square, loc, std::move(items)) {}
};

template<>
struct Vertex<ast_identifier> final : ASTExprLeaf {
  const Symbol* sym = nullptr;    // always filled (after resolved); points to local / global / function / constant
  std::string_view name;

  Vertex* mutate() const { return const_cast<Vertex*>(this); }
  void assign_sym(const Symbol* sym);

  Vertex(SrcLocation loc, std::string_view name)
    : ASTExprLeaf(ast_identifier, loc)
    , name(name) {}
};

template<>
struct Vertex<ast_int_const> final : ASTExprLeaf {
  td::RefInt256 intval;         // parsed value, 255 for "0xFF"
  std::string_view orig_str;    // original "0xFF"; empty for nodes generated by compiler (e.g. in constant folding)

  Vertex(SrcLocation loc, td::RefInt256 intval, std::string_view orig_str)
    : ASTExprLeaf(ast_int_const, loc)
    , intval(std::move(intval))
    , orig_str(orig_str) {}
};

template<>
struct Vertex<ast_string_const> final : ASTExprLeaf {
  std::string_view str_val;
  char modifier;

  bool is_bitslice() const {
    char m = modifier;
    return m == 0 || m == 's' || m == 'a';
  }
  bool is_intval() const {
    char m = modifier;
    return m == 'u' || m == 'h' || m == 'H' || m == 'c';
  }

  Vertex(SrcLocation loc, std::string_view str_val, char modifier)
    : ASTExprLeaf(ast_string_const, loc)
    , str_val(str_val), modifier(modifier) {}
};

template<>
struct Vertex<ast_bool_const> final : ASTExprLeaf {
  bool bool_val;

  Vertex(SrcLocation loc, bool bool_val)
    : ASTExprLeaf(ast_bool_const, loc)
    , bool_val(bool_val) {}
};

template<>
struct Vertex<ast_null_keyword> final : ASTExprLeaf {
  explicit Vertex(SrcLocation loc)
    : ASTExprLeaf(ast_null_keyword, loc) {}
};

template<>
struct Vertex<ast_self_keyword> final : ASTExprLeaf {
  const LocalVarData* param_ref = nullptr;  // filled after resolve identifiers, points to `self` parameter

  Vertex* mutate() const { return const_cast<Vertex*>(this); }
  void assign_param_ref(const LocalVarData* self_param);

  explicit Vertex(SrcLocation loc)
    : ASTExprLeaf(ast_self_keyword, loc) {}
};

template<>
struct Vertex<ast_argument> final : ASTExprUnary {
  bool passed_as_mutate;      // when called `f(mutate arg)`, not `f(arg)`

  AnyExprV get_expr() const { return child; }

  Vertex(SrcLocation loc, AnyExprV expr, bool passed_as_mutate)
    : ASTExprUnary(ast_argument, loc, expr)
    , passed_as_mutate(passed_as_mutate) {}
};

template<>
struct Vertex<ast_argument_list> final : ASTExprVararg {
  const std::vector<AnyExprV>& get_arguments() const { return children; }
  auto get_arg(int i) const { return children.at(i)->as<ast_argument>(); }

  Vertex(SrcLocation loc, std::vector<AnyExprV> arguments)
    : ASTExprVararg(ast_argument_list, loc, std::move(arguments)) {}
};

template<>
struct Vertex<ast_function_call> final : ASTExprBinary {
  const FunctionData* fun_maybe = nullptr;  // filled after resolve; remains nullptr for `localVar()` / `getF()()`

  AnyExprV get_called_f() const { return lhs; }
  auto get_arg_list() const { return rhs->as<ast_argument_list>(); }
  int get_num_args() const { return rhs->as<ast_argument_list>()->size(); }
  auto get_arg(int i) const { return rhs->as<ast_argument_list>()->get_arg(i); }

  Vertex* mutate() const { return const_cast<Vertex*>(this); }
  void assign_fun_ref(const FunctionData* fun_ref);

  Vertex(SrcLocation loc, AnyExprV lhs_f, V<ast_argument_list> arguments)
    : ASTExprBinary(ast_function_call, loc, lhs_f, arguments) {}
};

template<>
struct Vertex<ast_dot_method_call> final : ASTExprBinary {
  const FunctionData* fun_ref = nullptr;  // points to global function (after resolve)
  std::string_view method_name;

  AnyExprV get_obj() const { return lhs; }
  auto get_arg_list() const { return rhs->as<ast_argument_list>(); }
  int get_num_args() const { return rhs->as<ast_argument_list>()->size(); }
  auto get_arg(int i) const { return rhs->as<ast_argument_list>()->get_arg(i); }

  Vertex* mutate() const { return const_cast<Vertex*>(this); }
  void assign_fun_ref(const FunctionData* fun_ref);

  Vertex(SrcLocation loc, std::string_view method_name, AnyExprV lhs, V<ast_argument_list> arguments)
    : ASTExprBinary(ast_dot_method_call, loc, lhs, arguments)
    , method_name(method_name) {}
};

template<>
struct Vertex<ast_global_var_declaration> final : ASTStatementUnary {
  const GlobalVarData* var_ref = nullptr;  // filled after register
  TypeExpr* declared_type;

  auto get_identifier() const { return child->as<ast_identifier>(); }

  Vertex* mutate() const { return const_cast<Vertex*>(this); }
  void assign_var_ref(const GlobalVarData* var_ref);

  Vertex(SrcLocation loc, V<ast_identifier> name_identifier, TypeExpr* declared_type)
    : ASTStatementUnary(ast_global_var_declaration, loc, name_identifier)
    , declared_type(declared_type) {}
};

template<>
struct Vertex<ast_constant_declaration> final : ASTStatementVararg {
  const GlobalConstData* const_ref = nullptr;  // filled after register
  TypeExpr* declared_type;      // may be nullptr

  auto get_identifier() const { return child(0)->as<ast_identifier>(); }
  AnyExprV get_init_value() const { return child_as_expr(1); }

  Vertex* mutate() const { return const_cast<Vertex*>(this); }
  void assign_const_ref(const GlobalConstData* const_ref);

  Vertex(SrcLocation loc, V<ast_identifier> name_identifier, TypeExpr* declared_type, AnyExprV init_value)
    : ASTStatementVararg(ast_constant_declaration, loc, {name_identifier, init_value})
    , declared_type(declared_type) {}
};

template<>
struct Vertex<ast_underscore> final : ASTExprLeaf {
  explicit Vertex(SrcLocation loc)
    : ASTExprLeaf(ast_underscore, loc) {}
};

template<>
struct Vertex<ast_unary_operator> final : ASTExprUnary {
  std::string_view operator_name;
  TokenType tok;

  AnyExprV get_rhs() const { return child; }

  Vertex(SrcLocation loc, std::string_view operator_name, TokenType tok, AnyExprV rhs)
    : ASTExprUnary(ast_unary_operator, loc, rhs)
    , operator_name(operator_name), tok(tok) {}
};

template<>
struct Vertex<ast_binary_operator> final : ASTExprBinary {
  std::string_view operator_name;
  TokenType tok;

  AnyExprV get_lhs() const { return lhs; }
  AnyExprV get_rhs() const { return rhs; }

  bool is_set_assign() const {
    TokenType t = tok;
    return t == tok_set_plus || t == tok_set_minus || t == tok_set_mul || t == tok_set_div ||
           t == tok_set_mod || t == tok_set_lshift || t == tok_set_rshift ||
           t == tok_set_bitwise_and || t == tok_set_bitwise_or || t == tok_set_bitwise_xor;
  }

  bool is_assign() const {
    return tok == tok_assign;
  }

  Vertex(SrcLocation loc, std::string_view operator_name, TokenType tok, AnyExprV lhs, AnyExprV rhs)
    : ASTExprBinary(ast_binary_operator, loc, lhs, rhs)
    , operator_name(operator_name), tok(tok) {}
};

template<>
struct Vertex<ast_ternary_operator> final : ASTExprVararg {
  AnyExprV get_cond() const { return children.at(0); }
  AnyExprV get_when_true() const { return children.at(1); }
  AnyExprV get_when_false() const { return children.at(2); }

  Vertex(SrcLocation loc, AnyExprV cond, AnyExprV when_true, AnyExprV when_false)
    : ASTExprVararg(ast_ternary_operator, loc, {cond, when_true, when_false}) {}
};

template<>
struct Vertex<ast_return_statement> : ASTStatementUnary {
  AnyExprV get_return_value() const { return child_as_expr(); }

  Vertex(SrcLocation loc, AnyExprV child)
    : ASTStatementUnary(ast_return_statement, loc, child) {}
};

template<>
struct Vertex<ast_sequence> final : ASTStatementVararg {
  SrcLocation loc_end;

  const std::vector<AnyV>& get_items() const { return children; }
  AnyV get_item(int i) const { return children.at(i); }

  Vertex(SrcLocation loc, SrcLocation loc_end, std::vector<AnyV> items)
    : ASTStatementVararg(ast_sequence, loc, std::move(items))
    , loc_end(loc_end) {}
};

template<>
struct Vertex<ast_repeat_statement> final : ASTStatementVararg {
  AnyExprV get_cond() const { return child_as_expr(0); }
  auto get_body() const { return child(1)->as<ast_sequence>(); }

  Vertex(SrcLocation loc, AnyExprV cond, V<ast_sequence> body)
    : ASTStatementVararg(ast_repeat_statement, loc, {cond, body}) {}
};

template<>
struct Vertex<ast_while_statement> final : ASTStatementVararg {
  AnyExprV get_cond() const { return child_as_expr(0); }
  auto get_body() const { return child(1)->as<ast_sequence>(); }

  Vertex(SrcLocation loc, AnyExprV cond, V<ast_sequence> body)
    : ASTStatementVararg(ast_while_statement, loc, {cond, body}) {}
};

template<>
struct Vertex<ast_do_while_statement> final : ASTStatementVararg {
  auto get_body() const { return child(0)->as<ast_sequence>(); }
  AnyExprV get_cond() const { return child_as_expr(1); }

  Vertex(SrcLocation loc, V<ast_sequence> body, AnyExprV cond)
    : ASTStatementVararg(ast_do_while_statement, loc, {body, cond}) {}
};

template<>
struct Vertex<ast_throw_statement> final : ASTStatementVararg {
  AnyExprV get_thrown_code() const { return child_as_expr(0); }
  AnyExprV get_thrown_arg() const { return child_as_expr(1); }    // may be ast_empty
  bool has_thrown_arg() const { return child_as_expr(1)->type != ast_empty_expression; }

  Vertex(SrcLocation loc, AnyExprV thrown_code, AnyExprV thrown_arg)
    : ASTStatementVararg(ast_throw_statement, loc, {thrown_code, thrown_arg}) {}
};

template<>
struct Vertex<ast_assert_statement> final : ASTStatementVararg {
  AnyExprV get_cond() const { return child_as_expr(0); }
  AnyExprV get_thrown_code() const { return child_as_expr(1); }

  Vertex(SrcLocation loc, AnyExprV cond, AnyExprV thrown_code)
    : ASTStatementVararg(ast_assert_statement, loc, {cond, thrown_code}) {}
};

template<>
struct Vertex<ast_try_catch_statement> final : ASTStatementVararg {
  auto get_try_body() const { return children.at(0)->as<ast_sequence>(); }
  auto get_catch_expr() const { return children.at(1)->as<ast_tensor>(); }    // (excNo, arg), always len 2
  auto get_catch_body() const { return children.at(2)->as<ast_sequence>(); }

  Vertex(SrcLocation loc, V<ast_sequence> try_body, V<ast_tensor> catch_expr, V<ast_sequence> catch_body)
    : ASTStatementVararg(ast_try_catch_statement, loc, {try_body, catch_expr, catch_body}) {}
};

template<>
struct Vertex<ast_if_statement> final : ASTStatementVararg {
  bool is_ifnot;  // if(!cond), to generate more optimal fift code

  AnyExprV get_cond() const { return child_as_expr(0); }
  auto get_if_body() const { return child(1)->as<ast_sequence>(); }
  auto get_else_body() const { return child(2)->as<ast_sequence>(); }    // always exists (when else omitted, it's empty)

  Vertex(SrcLocation loc, bool is_ifnot, AnyExprV cond, V<ast_sequence> if_body, V<ast_sequence> else_body)
    : ASTStatementVararg(ast_if_statement, loc, {cond, if_body, else_body})
    , is_ifnot(is_ifnot) {}
};

template<>
struct Vertex<ast_genericsT_item> final : ASTOtherLeaf {
  TypeExpr* created_type;   // used to keep same pointer, since TypeExpr::new_var(i) always allocates
  std::string_view nameT;

  Vertex(SrcLocation loc, TypeExpr* created_type, std::string_view nameT)
    : ASTOtherLeaf(ast_genericsT_item, loc)
    , created_type(created_type), nameT(nameT) {}
};

template<>
struct Vertex<ast_genericsT_list> final : ASTOtherVararg {
  std::vector<AnyV> get_items() const { return children; }
  auto get_item(int i) const { return children.at(i)->as<ast_genericsT_item>(); }

  Vertex(SrcLocation loc, std::vector<AnyV> genericsT_items)
    : ASTOtherVararg(ast_genericsT_list, loc, std::move(genericsT_items)) {}

  int lookup_idx(std::string_view nameT) const;
};

template<>
struct Vertex<ast_parameter> final : ASTOtherLeaf {
  const LocalVarData* param_ref = nullptr;    // filled after resolved
  std::string_view param_name;
  TypeExpr* declared_type;
  bool declared_as_mutate;      // declared as `mutate param_name`

  bool is_underscore() const { return param_name.empty(); }

  Vertex* mutate() const { return const_cast<Vertex*>(this); }
  void assign_param_ref(const LocalVarData* param_ref);

  Vertex(SrcLocation loc, std::string_view param_name, TypeExpr* declared_type, bool declared_as_mutate)
    : ASTOtherLeaf(ast_parameter, loc)
    , param_name(param_name), declared_type(declared_type), declared_as_mutate(declared_as_mutate) {}
};

template<>
struct Vertex<ast_parameter_list> final : ASTOtherVararg {
  const std::vector<AnyV>& get_params() const { return children; }
  auto get_param(int i) const { return children.at(i)->as<ast_parameter>(); }

  Vertex(SrcLocation loc, std::vector<AnyV> params)
    : ASTOtherVararg(ast_parameter_list, loc, std::move(params)) {}

  int lookup_idx(std::string_view param_name) const;
  int get_mutate_params_count() const;
  bool has_mutate_params() const { return get_mutate_params_count() > 0; }
};

template<>
struct Vertex<ast_asm_body> final : ASTStatementVararg {
  std::vector<int> arg_order;
  std::vector<int> ret_order;

  const std::vector<AnyV>& get_asm_commands() const { return children; }    // ast_string_const[]

  Vertex(SrcLocation loc, std::vector<int> arg_order, std::vector<int> ret_order, std::vector<AnyV> asm_commands)
    : ASTStatementVararg(ast_asm_body, loc, std::move(asm_commands))
    , arg_order(std::move(arg_order)), ret_order(std::move(ret_order)) {}
};

template<>
struct Vertex<ast_annotation> final : ASTOtherVararg {
  AnnotationKind kind;

  auto get_arg() const { return child(0)->as<ast_tensor>(); }

  static AnnotationKind parse_kind(std::string_view name);

  Vertex(SrcLocation loc, AnnotationKind kind, V<ast_tensor> arg_probably_empty)
    : ASTOtherVararg(ast_annotation, loc, {arg_probably_empty})
    , kind(kind) {}
};

template<>
struct Vertex<ast_local_var> final : ASTExprUnary {
  const Symbol* var_maybe = nullptr;    // typically local var; can be global var if `var g_v redef`; remains nullptr for underscore
  TypeExpr* declared_type;
  bool is_immutable;       // declared via 'val', not 'var'
  bool marked_as_redef;    // var (existing_var redef, new_var: int) = ...

  AnyExprV get_identifier() const { return child; } // ast_identifier / ast_underscore

  Vertex* mutate() const { return const_cast<Vertex*>(this); }
  void assign_var_ref(const Symbol* var_ref);

  Vertex(SrcLocation loc, AnyExprV name_identifier, TypeExpr* declared_type, bool is_immutable, bool marked_as_redef)
    : ASTExprUnary(ast_local_var, loc, name_identifier), declared_type(declared_type), is_immutable(is_immutable), marked_as_redef(marked_as_redef) {}
};

template<>
struct Vertex<ast_local_vars_declaration> final : ASTStatementVararg {
  AnyExprV get_lhs() const { return child_as_expr(0); } // ast_local_var / ast_tensor / ast_tensor_square
  AnyExprV get_assigned_val() const { return child_as_expr(1); }

  Vertex(SrcLocation loc, AnyExprV lhs, AnyExprV assigned_val)
    : ASTStatementVararg(ast_local_vars_declaration, loc, {lhs, assigned_val}) {}
};

template<>
struct Vertex<ast_function_declaration> final : ASTOtherVararg {
  auto get_identifier() const { return child(0)->as<ast_identifier>(); }
  int get_num_params() const { return child(1)->as<ast_parameter_list>()->size(); }
  auto get_param_list() const { return child(1)->as<ast_parameter_list>(); }
  auto get_param(int i) const { return child(1)->as<ast_parameter_list>()->get_param(i); }
  AnyV get_body() const { return child(2); }   // ast_sequence / ast_asm_body

  const FunctionData* fun_ref = nullptr;  // filled after register
  TypeExpr* ret_type = nullptr;
  V<ast_genericsT_list> genericsT_list = nullptr;
  bool is_entrypoint = false;
  bool marked_as_pure = false;
  bool marked_as_builtin = false;
  bool marked_as_get_method = false;
  bool marked_as_inline = false;
  bool marked_as_inline_ref = false;
  bool accepts_self = false;
  bool returns_self = false;
  V<ast_int_const> method_id = nullptr;

  bool is_asm_function() const { return children.at(2)->type == ast_asm_body; }
  bool is_regular_function() const { return children.at(2)->type == ast_sequence; }
  bool is_builtin_function() const { return marked_as_builtin; }

  Vertex* mutate() const { return const_cast<Vertex*>(this); }
  void assign_fun_ref(const FunctionData* fun_ref);

  Vertex(SrcLocation loc, V<ast_identifier> name_identifier, V<ast_parameter_list> parameters, AnyV body)
    : ASTOtherVararg(ast_function_declaration, loc, {name_identifier, parameters, body}) {}
};

template<>
struct Vertex<ast_tolk_required_version> final : ASTOtherLeaf {
  std::string_view semver;

  Vertex(SrcLocation loc, std::string_view semver)
    : ASTOtherLeaf(ast_tolk_required_version, loc)
    , semver(semver) {}
};

template<>
struct Vertex<ast_import_statement> final : ASTOtherVararg {
  const SrcFile* file = nullptr;    // assigned after imports have been resolved

  auto get_file_leaf() const { return child(0)->as<ast_string_const>(); }

  std::string get_file_name() const { return static_cast<std::string>(child(0)->as<ast_string_const>()->str_val); }

  Vertex* mutate() const { return const_cast<Vertex*>(this); }
  void assign_src_file(const SrcFile* file);

  Vertex(SrcLocation loc, V<ast_string_const> file_name)
    : ASTOtherVararg(ast_import_statement, loc, {file_name}) {}
};

template<>
struct Vertex<ast_tolk_file> final : ASTOtherVararg {
  const SrcFile* const file;

  const std::vector<AnyV>& get_toplevel_declarations() const { return children; }

  Vertex(const SrcFile* file, std::vector<AnyV> toplevel_declarations)
    : ASTOtherVararg(ast_tolk_file, SrcLocation(file), std::move(toplevel_declarations))
    , file(file) {}
};

} // namespace tolk
