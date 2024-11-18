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
#include "platform-utils.h"
#include "src-file.h"
#include "type-expr.h"
#include "lexer.h"

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
 *   Generally, there are two ways of accepting a vertex:
 *   * AnyV (= const ASTNodeBase*)
 *     the only you can do with this vertex is to see v->type (ASTNodeType) and to cast via v->as<node_type>()
 *   * V<node_type> (= const Vertex<node_type>*)
 *     a specific type of vertex, you can use its fields and methods
 *   There is one way of creating a vertex:
 *   * createV<node_type>(...constructor_args)   (= new Vertex<node_type>(...))
 *     vertices are currently created on a heap, without any custom memory arena, just allocated and never deleted
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
  ast_empty,
  ast_parenthesized_expr,
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

struct ASTNodeBase;

using AnyV = const ASTNodeBase*;

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

struct ASTNodeLeaf : ASTNodeBase {
  friend class ASTVisitor;
  friend class ASTReplacer;

protected:
  ASTNodeLeaf(ASTNodeType type, SrcLocation loc)
    : ASTNodeBase(type, loc) {}
};

struct ASTNodeUnary : ASTNodeBase {
  friend class ASTVisitor;
  friend class ASTReplacer;

protected:
  AnyV child;

  ASTNodeUnary(ASTNodeType type, SrcLocation loc, AnyV child)
    : ASTNodeBase(type, loc), child(child) {}
};

struct ASTNodeBinary : ASTNodeBase {
  friend class ASTVisitor;
  friend class ASTReplacer;

protected:
  AnyV lhs;
  AnyV rhs;

  ASTNodeBinary(ASTNodeType type, SrcLocation loc, AnyV lhs, AnyV rhs)
    : ASTNodeBase(type, loc), lhs(lhs), rhs(rhs) {}
};

struct ASTNodeVararg : ASTNodeBase {
  friend class ASTVisitor;
  friend class ASTReplacer;

protected:
  std::vector<AnyV> children;

  ASTNodeVararg(ASTNodeType type, SrcLocation loc, std::vector<AnyV> children)
    : ASTNodeBase(type, loc), children(std::move(children)) {}

public:
  int size() const { return static_cast<int>(children.size()); }
  bool empty() const { return children.empty(); }
};

// ---------------------------------------------------------

template<>
struct Vertex<ast_empty> final : ASTNodeLeaf {
  explicit Vertex(SrcLocation loc)
    : ASTNodeLeaf(ast_empty, loc) {}
};

template<>
struct Vertex<ast_parenthesized_expr> final : ASTNodeUnary {
  AnyV get_expr() const { return child; }

  Vertex(SrcLocation loc, AnyV expr)
    : ASTNodeUnary(ast_parenthesized_expr, loc, expr) {}
};

template<>
struct Vertex<ast_tensor> final : ASTNodeVararg {
  const std::vector<AnyV>& get_items() const { return children; }
  AnyV get_item(int i) const { return children.at(i); }

  Vertex(SrcLocation loc, std::vector<AnyV> items)
    : ASTNodeVararg(ast_tensor, loc, std::move(items)) {}
};

template<>
struct Vertex<ast_tensor_square> final : ASTNodeVararg {
  const std::vector<AnyV>& get_items() const { return children; }
  AnyV get_item(int i) const { return children.at(i); }

  Vertex(SrcLocation loc, std::vector<AnyV> items)
    : ASTNodeVararg(ast_tensor_square, loc, std::move(items)) {}
};

template<>
struct Vertex<ast_identifier> final : ASTNodeLeaf {
  std::string_view name;

  Vertex(SrcLocation loc, std::string_view name)
    : ASTNodeLeaf(ast_identifier, loc), name(name) {}
};

template<>
struct Vertex<ast_int_const> final : ASTNodeLeaf {
  std::string_view int_val;

  Vertex(SrcLocation loc, std::string_view int_val)
    : ASTNodeLeaf(ast_int_const, loc), int_val(int_val) {}
};

template<>
struct Vertex<ast_string_const> final : ASTNodeLeaf {
  std::string_view str_val;
  char modifier;

  Vertex(SrcLocation loc, std::string_view str_val, char modifier)
    : ASTNodeLeaf(ast_string_const, loc), str_val(str_val), modifier(modifier) {}
};

template<>
struct Vertex<ast_bool_const> final : ASTNodeLeaf {
  bool bool_val;

  Vertex(SrcLocation loc, bool bool_val)
    : ASTNodeLeaf(ast_bool_const, loc), bool_val(bool_val) {}
};

template<>
struct Vertex<ast_null_keyword> final : ASTNodeLeaf {
  explicit Vertex(SrcLocation loc)
    : ASTNodeLeaf(ast_null_keyword, loc) {}
};

template<>
struct Vertex<ast_self_keyword> final : ASTNodeLeaf {
  explicit Vertex(SrcLocation loc)
    : ASTNodeLeaf(ast_self_keyword, loc) {}
};

template<>
struct Vertex<ast_argument> final : ASTNodeUnary {
  bool passed_as_mutate;      // when called `f(mutate arg)`, not `f(arg)`

  AnyV get_expr() const { return child; }

  explicit Vertex(SrcLocation loc, AnyV expr, bool passed_as_mutate)
    : ASTNodeUnary(ast_argument, loc, expr), passed_as_mutate(passed_as_mutate) {}
};

template<>
struct Vertex<ast_argument_list> final : ASTNodeVararg {
  const std::vector<AnyV>& get_arguments() const { return children; }
  auto get_arg(int i) const { return children.at(i)->as<ast_argument>(); }

  explicit Vertex(SrcLocation loc, std::vector<AnyV> arguments)
    : ASTNodeVararg(ast_argument_list, loc, std::move(arguments)) {}
};

template<>
struct Vertex<ast_function_call> final : ASTNodeBinary {
  AnyV get_called_f() const { return lhs; }
  auto get_arg_list() const { return rhs->as<ast_argument_list>(); }
  int get_num_args() const { return rhs->as<ast_argument_list>()->size(); }
  auto get_arg(int i) const { return rhs->as<ast_argument_list>()->get_arg(i); }

  Vertex(SrcLocation loc, AnyV lhs_f, V<ast_argument_list> arguments)
    : ASTNodeBinary(ast_function_call, loc, lhs_f, arguments) {}
};

template<>
struct Vertex<ast_dot_method_call> final : ASTNodeBinary {
  std::string_view method_name;

  AnyV get_obj() const { return lhs; }
  auto get_arg_list() const { return rhs->as<ast_argument_list>(); }

  Vertex(SrcLocation loc, std::string_view method_name, AnyV lhs, V<ast_argument_list> arguments)
    : ASTNodeBinary(ast_dot_method_call, loc, lhs, arguments), method_name(method_name) {}
};

template<>
struct Vertex<ast_global_var_declaration> final : ASTNodeUnary {
  TypeExpr* declared_type;      // may be nullptr

  auto get_identifier() const { return child->as<ast_identifier>(); }

  Vertex(SrcLocation loc, V<ast_identifier> name_identifier, TypeExpr* declared_type)
    : ASTNodeUnary(ast_global_var_declaration, loc, name_identifier), declared_type(declared_type) {}
};

template<>
struct Vertex<ast_constant_declaration> final : ASTNodeBinary {
  TypeExpr* declared_type;      // may be nullptr

  auto get_identifier() const { return lhs->as<ast_identifier>(); }
  AnyV get_init_value() const { return rhs; }

  Vertex(SrcLocation loc, V<ast_identifier> name_identifier, TypeExpr* declared_type, AnyV init_value)
    : ASTNodeBinary(ast_constant_declaration, loc, name_identifier, init_value), declared_type(declared_type) {}
};

template<>
struct Vertex<ast_underscore> final : ASTNodeLeaf {
  explicit Vertex(SrcLocation loc)
    : ASTNodeLeaf(ast_underscore, loc) {}
};

template<>
struct Vertex<ast_unary_operator> final : ASTNodeUnary {
  std::string_view operator_name;
  TokenType tok;

  AnyV get_rhs() const { return child; }

  Vertex(SrcLocation loc, std::string_view operator_name, TokenType tok, AnyV rhs)
    : ASTNodeUnary(ast_unary_operator, loc, rhs), operator_name(operator_name), tok(tok) {}
};

template<>
struct Vertex<ast_binary_operator> final : ASTNodeBinary {
  std::string_view operator_name;
  TokenType tok;

  AnyV get_lhs() const { return lhs; }
  AnyV get_rhs() const { return rhs; }

  Vertex(SrcLocation loc, std::string_view operator_name, TokenType tok, AnyV lhs, AnyV rhs)
    : ASTNodeBinary(ast_binary_operator, loc, lhs, rhs), operator_name(operator_name), tok(tok) {}
};

template<>
struct Vertex<ast_ternary_operator> final : ASTNodeVararg {
  AnyV get_cond() const { return children.at(0); }
  AnyV get_when_true() const { return children.at(1); }
  AnyV get_when_false() const { return children.at(2); }

  Vertex(SrcLocation loc, AnyV cond, AnyV when_true, AnyV when_false)
    : ASTNodeVararg(ast_ternary_operator, loc, {cond, when_true, when_false}) {}
};

template<>
struct Vertex<ast_return_statement> : ASTNodeUnary {
  AnyV get_return_value() const { return child; }

  Vertex(SrcLocation loc, AnyV child)
    : ASTNodeUnary(ast_return_statement, loc, child) {}
};

template<>
struct Vertex<ast_sequence> final : ASTNodeVararg {
  SrcLocation loc_end;

  const std::vector<AnyV>& get_items() const { return children; }
  AnyV get_item(int i) const { return children.at(i); }

  Vertex(SrcLocation loc, SrcLocation loc_end, std::vector<AnyV> items)
    : ASTNodeVararg(ast_sequence, loc, std::move(items)), loc_end(loc_end) {}
};

template<>
struct Vertex<ast_repeat_statement> final : ASTNodeBinary {
  AnyV get_cond() const { return lhs; }
  auto get_body() const { return rhs->as<ast_sequence>(); }

  Vertex(SrcLocation loc, AnyV cond, V<ast_sequence> body)
    : ASTNodeBinary(ast_repeat_statement, loc, cond, body) {}
};

template<>
struct Vertex<ast_while_statement> final : ASTNodeBinary {
  AnyV get_cond() const { return lhs; }
  auto get_body() const { return rhs->as<ast_sequence>(); }

  Vertex(SrcLocation loc, AnyV cond, V<ast_sequence> body)
    : ASTNodeBinary(ast_while_statement, loc, cond, body) {}
};

template<>
struct Vertex<ast_do_while_statement> final : ASTNodeBinary {
  auto get_body() const { return lhs->as<ast_sequence>(); }
  AnyV get_cond() const { return rhs; }

  Vertex(SrcLocation loc, V<ast_sequence> body, AnyV cond)
    : ASTNodeBinary(ast_do_while_statement, loc, body, cond) {}
};

template<>
struct Vertex<ast_throw_statement> final : ASTNodeBinary {
  AnyV get_thrown_code() const { return lhs; }
  AnyV get_thrown_arg() const { return rhs; }    // may be ast_empty
  bool has_thrown_arg() const { return rhs->type != ast_empty; }

  Vertex(SrcLocation loc, AnyV thrown_code, AnyV thrown_arg)
    : ASTNodeBinary(ast_throw_statement, loc, thrown_code, thrown_arg) {}
};

template<>
struct Vertex<ast_assert_statement> final : ASTNodeBinary {
  AnyV get_cond() const { return lhs; }
  AnyV get_thrown_code() const { return rhs; }

  Vertex(SrcLocation loc, AnyV cond, AnyV thrown_code)
    : ASTNodeBinary(ast_assert_statement, loc, cond, thrown_code) {}
};

template<>
struct Vertex<ast_try_catch_statement> final : ASTNodeVararg {
  auto get_try_body() const { return children.at(0)->as<ast_sequence>(); }
  auto get_catch_expr() const { return children.at(1)->as<ast_tensor>(); }    // (excNo, arg), always len 2
  auto get_catch_body() const { return children.at(2)->as<ast_sequence>(); }

  Vertex(SrcLocation loc, V<ast_sequence> try_body, V<ast_tensor> catch_expr, V<ast_sequence> catch_body)
    : ASTNodeVararg(ast_try_catch_statement, loc, {try_body, catch_expr, catch_body}) {}
};

template<>
struct Vertex<ast_if_statement> final : ASTNodeVararg {
  bool is_ifnot;  // if(!cond), to generate more optimal fift code

  AnyV get_cond() const { return children.at(0); }
  auto get_if_body() const { return children.at(1)->as<ast_sequence>(); }
  auto get_else_body() const { return children.at(2)->as<ast_sequence>(); }    // always exists (when else omitted, it's empty)

  Vertex(SrcLocation loc, bool is_ifnot, AnyV cond, V<ast_sequence> if_body, V<ast_sequence> else_body)
    : ASTNodeVararg(ast_if_statement, loc, {cond, if_body, else_body}), is_ifnot(is_ifnot) {}
};

template<>
struct Vertex<ast_genericsT_item> final : ASTNodeLeaf {
  TypeExpr* created_type;   // used to keep same pointer, since TypeExpr::new_var(i) always allocates
  std::string_view nameT;

  Vertex(SrcLocation loc, TypeExpr* created_type, std::string_view nameT)
    : ASTNodeLeaf(ast_genericsT_item, loc), created_type(created_type), nameT(nameT) {}
};

template<>
struct Vertex<ast_genericsT_list> final : ASTNodeVararg {
  std::vector<AnyV> get_items() const { return children; }
  auto get_item(int i) const { return children.at(i)->as<ast_genericsT_item>(); }

  Vertex(SrcLocation loc, std::vector<AnyV> genericsT_items)
    : ASTNodeVararg(ast_genericsT_list, loc, std::move(genericsT_items)) {}

  int lookup_idx(std::string_view nameT) const;
};

template<>
struct Vertex<ast_parameter> final : ASTNodeUnary {
  TypeExpr* param_type;
  bool declared_as_mutate;      // declared as `mutate param_name`

  auto get_identifier() const { return child->as<ast_identifier>(); } // for underscore, name is empty
  bool is_underscore() const { return child->as<ast_identifier>()->name.empty(); }

  Vertex(SrcLocation loc, V<ast_identifier> name_identifier, TypeExpr* param_type, bool declared_as_mutate)
    : ASTNodeUnary(ast_parameter, loc, name_identifier), param_type(param_type), declared_as_mutate(declared_as_mutate) {}
};

template<>
struct Vertex<ast_parameter_list> final : ASTNodeVararg {
  const std::vector<AnyV>& get_params() const { return children; }
  auto get_param(int i) const { return children.at(i)->as<ast_parameter>(); }

  Vertex(SrcLocation loc, std::vector<AnyV> params)
    : ASTNodeVararg(ast_parameter_list, loc, std::move(params)) {}

  int lookup_idx(std::string_view param_name) const;
  int get_mutate_params_count() const;
  bool has_mutate_params() const { return get_mutate_params_count() > 0; }
};

template<>
struct Vertex<ast_asm_body> final : ASTNodeVararg {
  std::vector<int> arg_order;
  std::vector<int> ret_order;

  const std::vector<AnyV>& get_asm_commands() const { return children; }    // ast_string_const[]

  Vertex(SrcLocation loc, std::vector<int> arg_order, std::vector<int> ret_order, std::vector<AnyV> asm_commands)
    : ASTNodeVararg(ast_asm_body, loc, std::move(asm_commands)), arg_order(std::move(arg_order)), ret_order(std::move(ret_order)) {}
};

template<>
struct Vertex<ast_annotation> final : ASTNodeUnary {
  AnnotationKind kind;

  auto get_arg() const { return child->as<ast_tensor>(); }

  static AnnotationKind parse_kind(std::string_view name);

  Vertex(SrcLocation loc, AnnotationKind kind, V<ast_tensor> arg_probably_empty)
    : ASTNodeUnary(ast_annotation, loc, arg_probably_empty), kind(kind) {}
};

template<>
struct Vertex<ast_local_var> final : ASTNodeUnary {
  TypeExpr* declared_type;
  bool is_immutable;       // declared via 'val', not 'var'
  bool marked_as_redef;    // var (existing_var redef, new_var: int) = ...

  AnyV get_identifier() const { return child; } // ast_identifier / ast_underscore

  Vertex(SrcLocation loc, AnyV name_identifier, TypeExpr* declared_type, bool is_immutable, bool marked_as_redef)
    : ASTNodeUnary(ast_local_var, loc, name_identifier), declared_type(declared_type), is_immutable(is_immutable), marked_as_redef(marked_as_redef) {}
};

template<>
struct Vertex<ast_local_vars_declaration> final : ASTNodeBinary {
  AnyV get_lhs() const { return lhs; } // ast_local_var / ast_tensor / ast_tensor_square
  AnyV get_assigned_val() const { return rhs; }

  Vertex(SrcLocation loc, AnyV lhs, AnyV assigned_val)
    : ASTNodeBinary(ast_local_vars_declaration, loc, lhs, assigned_val) {}
};

template<>
struct Vertex<ast_function_declaration> final : ASTNodeVararg {
  auto get_identifier() const { return children.at(0)->as<ast_identifier>(); }
  int get_num_params() const { return children.at(1)->as<ast_parameter_list>()->size(); }
  auto get_param_list() const { return children.at(1)->as<ast_parameter_list>(); }
  auto get_param(int i) const { return children.at(1)->as<ast_parameter_list>()->get_param(i); }
  AnyV get_body() const { return children.at(2); }   // ast_sequence / ast_asm_body

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

  Vertex(SrcLocation loc, V<ast_identifier> name_identifier, V<ast_parameter_list> parameters, AnyV body)
    : ASTNodeVararg(ast_function_declaration, loc, {name_identifier, parameters, body}) {}
};

template<>
struct Vertex<ast_tolk_required_version> final : ASTNodeLeaf {
  TokenType cmp_tok;
  std::string_view semver;

  Vertex(SrcLocation loc, TokenType cmp_tok, std::string_view semver)
    : ASTNodeLeaf(ast_tolk_required_version, loc), cmp_tok(cmp_tok), semver(semver) {}
};

template<>
struct Vertex<ast_import_statement> final : ASTNodeUnary {
  const SrcFile* file = nullptr;    // assigned after includes have been resolved

  auto get_file_leaf() const { return child->as<ast_string_const>(); }

  std::string get_file_name() const { return static_cast<std::string>(child->as<ast_string_const>()->str_val); }

  void mutate_set_src_file(const SrcFile* file) const;

  Vertex(SrcLocation loc, V<ast_string_const> file_name)
    : ASTNodeUnary(ast_import_statement, loc, file_name) {}
};

template<>
struct Vertex<ast_tolk_file> final : ASTNodeVararg {
  const SrcFile* const file;

  const std::vector<AnyV>& get_toplevel_declarations() const { return children; }

  Vertex(const SrcFile* file, std::vector<AnyV> toplevel_declarations)
    : ASTNodeVararg(ast_tolk_file, SrcLocation(file), std::move(toplevel_declarations)), file(file) {}
};

} // namespace tolk
