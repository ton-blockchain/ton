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
  ast_identifier,
  // expressions
  ast_empty_expression,
  ast_parenthesized_expression,
  ast_tensor,
  ast_typed_tuple,
  ast_reference,
  ast_local_var_lhs,
  ast_local_vars_declaration,
  ast_int_const,
  ast_string_const,
  ast_bool_const,
  ast_null_keyword,
  ast_argument,
  ast_argument_list,
  ast_dot_access,
  ast_function_call,
  ast_underscore,
  ast_assign,
  ast_set_assign,
  ast_unary_operator,
  ast_binary_operator,
  ast_ternary_operator,
  ast_cast_as_operator,
  // statements
  ast_empty_statement,
  ast_sequence,
  ast_return_statement,
  ast_if_statement,
  ast_repeat_statement,
  ast_while_statement,
  ast_do_while_statement,
  ast_throw_statement,
  ast_assert_statement,
  ast_try_catch_statement,
  ast_asm_body,
  // other
  ast_genericsT_item,
  ast_genericsT_list,
  ast_instantiationT_item,
  ast_instantiationT_list,
  ast_parameter,
  ast_parameter_list,
  ast_annotation,
  ast_function_declaration,
  ast_global_var_declaration,
  ast_constant_declaration,
  ast_tolk_required_version,
  ast_import_directive,
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
  ASTNodeBase(const ASTNodeBase&) = delete;

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
  friend class ASTDuplicatorFunction;

  TypePtr inferred_type = nullptr;
  bool is_rvalue: 1 = false;
  bool is_lvalue: 1 = false;

  ASTNodeExpressionBase* mutate() const { return const_cast<ASTNodeExpressionBase*>(this); }
  void assign_inferred_type(TypePtr type);
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

  AnyExprV child(int i) const { return children.at(i); }

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

  AnyExprV child_as_expr(int i) const { return reinterpret_cast<AnyExprV>(children.at(i)); }

  ASTOtherVararg(ASTNodeType type, SrcLocation loc, std::vector<AnyV> children)
    : ASTNodeBase(type, loc), children(std::move(children)) {}

public:
  int size() const { return static_cast<int>(children.size()); }
  bool empty() const { return children.empty(); }
};


template<>
// ast_identifier is "a name" in AST structure
// it's NOT a standalone expression, it's "implementation details" of other AST vertices
// example: `var x = 5` then "x" is identifier (inside local var declaration)
// example: `global g: int` then "g" is identifier
// example: `someF` is a reference, which contains identifier
// example: `someF<int>` is a reference which contains identifier and generics instantiation
// example: `fun f<T>()` then "f" is identifier, "<T>" is a generics declaration
struct Vertex<ast_identifier> final : ASTOtherLeaf {
  std::string_view name;    // empty for underscore

  Vertex(SrcLocation loc, std::string_view name)
    : ASTOtherLeaf(ast_identifier, loc)
    , name(name) {}
};


//
// ---------------------------------------------------------
//     expressions
//


template<>
// ast_empty_expression is "nothing" in context of expression, it has "unknown" type
// example: `throw 123;` then "throw arg" is empty expression (opposed to `throw (123, arg)`)
struct Vertex<ast_empty_expression> final : ASTExprLeaf {
  explicit Vertex(SrcLocation loc)
    : ASTExprLeaf(ast_empty_expression, loc) {}
};


template<>
// ast_parenthesized_expression is something surrounded embraced by (parenthesis)
// example: `(1)`, `((f()))` (two nested)
struct Vertex<ast_parenthesized_expression> final : ASTExprUnary {
  AnyExprV get_expr() const { return child; }

  Vertex(SrcLocation loc, AnyExprV expr)
    : ASTExprUnary(ast_parenthesized_expression, loc, expr) {}
};

template<>
// ast_tensor is a set of expressions embraced by (parenthesis)
// in most languages, it's called "tuple", but in TVM, "tuple" is a TVM primitive, that's why "tensor"
// example: `(1, 2)`, `(1, (2, 3))` (nested), `()` (empty tensor)
// note, that `(1)` is not a tensor, it's a parenthesized expression
// a tensor of N elements occupies N slots on a stack (opposed to TVM tuple primitive, 1 slot)
struct Vertex<ast_tensor> final : ASTExprVararg {
  const std::vector<AnyExprV>& get_items() const { return children; }
  AnyExprV get_item(int i) const { return child(i); }

  Vertex(SrcLocation loc, std::vector<AnyExprV> items)
    : ASTExprVararg(ast_tensor, loc, std::move(items)) {}
};

template<>
// ast_typed_tuple is a set of expressions in [square brackets]
// in TVM, it's a TVM tuple, that occupies 1 slot, but the compiler knows its "typed structure"
// example: `[1, x]`, `[[0]]` (nested)
// typed tuples can be assigned to N variables, like `[one, _, three] = [1,2,3]`
struct Vertex<ast_typed_tuple> final : ASTExprVararg {
  const std::vector<AnyExprV>& get_items() const { return children; }
  AnyExprV get_item(int i) const { return child(i); }

  Vertex(SrcLocation loc, std::vector<AnyExprV> items)
    : ASTExprVararg(ast_typed_tuple, loc, std::move(items)) {}
};

template<>
// ast_reference is "something that references a symbol"
// examples: `x` / `someF` / `someF<int>`
// it's a leaf expression from traversing point of view, but actually, has children (not expressions)
// note, that both `someF()` and `someF<int>()` are function calls, where a callee is just a reference
struct Vertex<ast_reference> final : ASTExprLeaf {
private:
  V<ast_identifier> identifier;                // its name, `x` / `someF`
  V<ast_instantiationT_list> instantiationTs;  // not null if `<int>`, otherwise nullptr

public:
  const Symbol* sym = nullptr;  // filled on resolve or type inferring; points to local / global / function / constant

  auto get_identifier() const { return identifier; }
  bool has_instantiationTs() const { return instantiationTs != nullptr; }
  auto get_instantiationTs() const { return instantiationTs; }
  std::string_view get_name() const { return identifier->name; }

  Vertex* mutate() const { return const_cast<Vertex*>(this); }
  void assign_sym(const Symbol* sym);

  Vertex(SrcLocation loc, V<ast_identifier> name_identifier, V<ast_instantiationT_list> instantiationTs)
    : ASTExprLeaf(ast_reference, loc)
    , identifier(name_identifier), instantiationTs(instantiationTs) {}
};

template<>
// ast_local_var_lhs is one variable inside `var` declaration
// example: `var x = 0;` then "x" is local var lhs
// example: `val (x: int, [y redef], _) = rhs` then "x" and "y" and "_" are
// it's a leaf from expression's point of view, though technically has an "identifier" child
struct Vertex<ast_local_var_lhs> final : ASTExprLeaf {
private:
  V<ast_identifier> identifier;

public:
  const LocalVarData* var_ref = nullptr;  // filled on resolve identifiers; for `redef` points to declared above; for underscore, name is empty
  TypePtr declared_type;            // not null for `var x: int = rhs`, otherwise nullptr
  bool is_immutable;                // declared via 'val', not 'var'
  bool marked_as_redef;             // var (existing_var redef, new_var: int) = ...

  V<ast_identifier> get_identifier() const { return identifier; }
  std::string_view get_name() const { return identifier->name; }     // empty for underscore

  Vertex* mutate() const { return const_cast<Vertex*>(this); }
  void assign_var_ref(const LocalVarData* var_ref);
  void assign_resolved_type(TypePtr declared_type);

  Vertex(SrcLocation loc, V<ast_identifier> identifier, TypePtr declared_type, bool is_immutable, bool marked_as_redef)
    : ASTExprLeaf(ast_local_var_lhs, loc)
    , identifier(identifier), declared_type(declared_type), is_immutable(is_immutable), marked_as_redef(marked_as_redef) {}
};

template<>
// ast_local_vars_declaration is an expression declaring local variables on the left side of assignment
// examples: see above
// for `var (x, [y])` its expr is "tensor (local var, typed tuple (local var))"
// for assignment `var x = 5`, this node is `var x`, lhs of assignment
struct Vertex<ast_local_vars_declaration> final : ASTExprUnary {
  AnyExprV get_expr() const { return child; } // ast_local_var_lhs / ast_tensor / ast_typed_tuple

  Vertex(SrcLocation loc, AnyExprV expr)
    : ASTExprUnary(ast_local_vars_declaration, loc, expr) {}
};

template<>
// ast_int_const is an integer literal
// examples: `0` / `0xFF`
// note, that `-1` is unary minus of `1` int const
struct Vertex<ast_int_const> final : ASTExprLeaf {
  td::RefInt256 intval;         // parsed value, 255 for "0xFF"
  std::string_view orig_str;    // original "0xFF"; empty for nodes generated by compiler (e.g. in constant folding)

  Vertex(SrcLocation loc, td::RefInt256 intval, std::string_view orig_str)
    : ASTExprLeaf(ast_int_const, loc)
    , intval(std::move(intval))
    , orig_str(orig_str) {}
};

template<>
// ast_string_const is a string literal in double quotes or """ when multiline
// examples: "asdf" / "Ef8zMz..."a / "to_calc_crc32_from"c
// an optional modifier specifies how a string is parsed (probably, like an integer)
// note, that TVM doesn't have strings, it has only slices, so "hello" has type slice
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
// ast_bool_const is either `true` or `false`
struct Vertex<ast_bool_const> final : ASTExprLeaf {
  bool bool_val;

  Vertex(SrcLocation loc, bool bool_val)
    : ASTExprLeaf(ast_bool_const, loc)
    , bool_val(bool_val) {}
};

template<>
// ast_null_keyword is the `null` literal
// it should be handled with care; for instance, `null` takes special place in the type system
struct Vertex<ast_null_keyword> final : ASTExprLeaf {
  explicit Vertex(SrcLocation loc)
    : ASTExprLeaf(ast_null_keyword, loc) {}
};

template<>
// ast_argument is an element of an argument list of a function/method call
// example: `f(1, x)` has 2 arguments, `t.tupleFirst()` has no arguments (though `t` is passed as `self`)
// example: `f(mutate arg)` has 1 argument with `passed_as_mutate` flag
// (without `mutate` keyword, the entity "argument" could be replaced just by "any expression")
struct Vertex<ast_argument> final : ASTExprUnary {
  bool passed_as_mutate;

  AnyExprV get_expr() const { return child; }

  Vertex(SrcLocation loc, AnyExprV expr, bool passed_as_mutate)
    : ASTExprUnary(ast_argument, loc, expr)
    , passed_as_mutate(passed_as_mutate) {}
};

template<>
// ast_argument_list contains N arguments of a function/method call
struct Vertex<ast_argument_list> final : ASTExprVararg {
  const std::vector<AnyExprV>& get_arguments() const { return children; }
  auto get_arg(int i) const { return child(i)->as<ast_argument>(); }

  Vertex(SrcLocation loc, std::vector<AnyExprV> arguments)
    : ASTExprVararg(ast_argument_list, loc, std::move(arguments)) {}
};

template<>
// ast_dot_access is "object before dot, identifier + optional <T> after dot"
// examples: `tensorVar.0` / `obj.field` / `getObj().method` / `t.tupleFirst<int>`
// from traversing point of view, it's an unary expression: only obj is expression, field name is not
// note, that `obj.method()` is a function call with "dot access `obj.method`" callee
struct Vertex<ast_dot_access> final : ASTExprUnary {
private:
  V<ast_identifier> identifier;                // `0` / `field` / `method`
  V<ast_instantiationT_list> instantiationTs;  // not null if `<int>`, otherwise nullptr

public:

  typedef const FunctionData* DotTarget;      // for `t.tupleAt` target is `tupleAt` global function
  DotTarget target = nullptr;                 // filled at type inferring

  AnyExprV get_obj() const { return child; }
  auto get_identifier() const { return identifier; }
  bool has_instantiationTs() const { return instantiationTs != nullptr; }
  auto get_instantiationTs() const { return instantiationTs; }
  std::string_view get_field_name() const { return identifier->name; }

  Vertex* mutate() const { return const_cast<Vertex*>(this); }
  void assign_target(const DotTarget& target);

  Vertex(SrcLocation loc, AnyExprV obj, V<ast_identifier> identifier, V<ast_instantiationT_list> instantiationTs)
    : ASTExprUnary(ast_dot_access, loc, obj)
    , identifier(identifier), instantiationTs(instantiationTs) {}
};

template<>
// ast_function_call is "calling some lhs with parenthesis", lhs is arbitrary expression (callee)
// example: `globalF()` then callee is reference
// example: `globalF<int>()` then callee is reference (with instantiation Ts filled)
// example: `local_var()` then callee is reference (points to local var, filled at resolve identifiers)
// example: `getF()()` then callee is another func call (which type is TypeDataFunCallable)
// example: `obj.method()` then callee is dot access (resolved while type inferring)
struct Vertex<ast_function_call> final : ASTExprBinary {
  const FunctionData* fun_maybe = nullptr;  // filled while type inferring for `globalF()` / `obj.f()`; remains nullptr for `local_var()` / `getF()()`

  AnyExprV get_callee() const { return lhs; }
  bool is_dot_call() const { return lhs->type == ast_dot_access; }
  AnyExprV get_dot_obj() const { return lhs->as<ast_dot_access>()->get_obj(); }
  auto get_arg_list() const { return rhs->as<ast_argument_list>(); }
  int get_num_args() const { return rhs->as<ast_argument_list>()->size(); }
  auto get_arg(int i) const { return rhs->as<ast_argument_list>()->get_arg(i); }

  Vertex* mutate() const { return const_cast<Vertex*>(this); }
  void assign_fun_ref(const FunctionData* fun_ref);

  Vertex(SrcLocation loc, AnyExprV lhs_f, V<ast_argument_list> arguments)
    : ASTExprBinary(ast_function_call, loc, lhs_f, arguments) {}
};

template<>
// ast_underscore represents `_` symbol used for left side of assignment
// example: `(cs, _) = cs.loadAndReturn()`
// though it's the only correct usage, using _ as rvalue like `var x = _;` is correct from AST point of view
// note, that for declaration `var _ = 1` underscore is a regular local var declared (with empty name)
// but for `_ = 1` (not declaration) it's underscore; it's because `var _:int` is also correct
struct Vertex<ast_underscore> final : ASTExprLeaf {
  explicit Vertex(SrcLocation loc)
    : ASTExprLeaf(ast_underscore, loc) {}
};

template<>
// ast_assign represents assignment "lhs = rhs"
// examples: `a = 4` / `var a = 4` / `(cs, b, mode) = rhs` / `f() = g()`
// note, that `a = 4` lhs is ast_reference, `var a = 4` lhs is ast_local_vars_declaration
struct Vertex<ast_assign> final : ASTExprBinary {
  AnyExprV get_lhs() const { return lhs; }
  AnyExprV get_rhs() const { return rhs; }

  explicit Vertex(SrcLocation loc, AnyExprV lhs, AnyExprV rhs)
    : ASTExprBinary(ast_assign, loc, lhs, rhs) {}
};

template<>
// ast_set_assign represents assignment-and-set operation "lhs <op>= rhs"
// examples: `a += 4` / `b <<= c`
struct Vertex<ast_set_assign> final : ASTExprBinary {
  const FunctionData* fun_ref = nullptr;      // filled at type inferring, points to `_+_` built-in for +=
  std::string_view operator_name;             // without equal sign, "+" for operator +=
  TokenType tok;                              // tok_set_*

  AnyExprV get_lhs() const { return lhs; }
  AnyExprV get_rhs() const { return rhs; }

  Vertex* mutate() const { return const_cast<Vertex*>(this); }
  void assign_fun_ref(const FunctionData* fun_ref);

  Vertex(SrcLocation loc, std::string_view operator_name, TokenType tok, AnyExprV lhs, AnyExprV rhs)
    : ASTExprBinary(ast_set_assign, loc, lhs, rhs)
    , operator_name(operator_name), tok(tok) {}
};

template<>
// ast_unary_operator is "some operator over one expression"
// examples: `-1` / `~found`
struct Vertex<ast_unary_operator> final : ASTExprUnary {
  const FunctionData* fun_ref = nullptr;      // filled at type inferring, points to some built-in function
  std::string_view operator_name;
  TokenType tok;

  AnyExprV get_rhs() const { return child; }

  Vertex* mutate() const { return const_cast<Vertex*>(this); }
  void assign_fun_ref(const FunctionData* fun_ref);

  Vertex(SrcLocation loc, std::string_view operator_name, TokenType tok, AnyExprV rhs)
    : ASTExprUnary(ast_unary_operator, loc, rhs)
    , operator_name(operator_name), tok(tok) {}
};

template<>
// ast_binary_operator is "some operator over two expressions"
// examples: `a + b` / `x & true` / `(a, b) << g()`
// note, that `a = b` is NOT a binary operator, it's ast_assign, also `a += b`, it's ast_set_assign
struct Vertex<ast_binary_operator> final : ASTExprBinary {
  const FunctionData* fun_ref = nullptr;      // filled at type inferring, points to some built-in function
  std::string_view operator_name;
  TokenType tok;

  AnyExprV get_lhs() const { return lhs; }
  AnyExprV get_rhs() const { return rhs; }

  Vertex* mutate() const { return const_cast<Vertex*>(this); }
  void assign_fun_ref(const FunctionData* fun_ref);

  Vertex(SrcLocation loc, std::string_view operator_name, TokenType tok, AnyExprV lhs, AnyExprV rhs)
    : ASTExprBinary(ast_binary_operator, loc, lhs, rhs)
    , operator_name(operator_name), tok(tok) {}
};

template<>
// ast_ternary_operator is a traditional ternary construction
// example: `cond ? a : b`
struct Vertex<ast_ternary_operator> final : ASTExprVararg {
  AnyExprV get_cond() const { return child(0); }
  AnyExprV get_when_true() const { return child(1); }
  AnyExprV get_when_false() const { return child(2); }

  Vertex(SrcLocation loc, AnyExprV cond, AnyExprV when_true, AnyExprV when_false)
    : ASTExprVararg(ast_ternary_operator, loc, {cond, when_true, when_false}) {}
};

template<>
// ast_cast_as_operator is explicit casting with "as" keyword
// examples: `arg as int` / `null as cell` / `t.tupleAt(2) as slice`
struct Vertex<ast_cast_as_operator> final : ASTExprUnary {
  AnyExprV get_expr() const { return child; }

  TypePtr cast_to_type;

  Vertex* mutate() const { return const_cast<Vertex*>(this); }
  void assign_resolved_type(TypePtr cast_to_type);

  Vertex(SrcLocation loc, AnyExprV expr, TypePtr cast_to_type)
    : ASTExprUnary(ast_cast_as_operator, loc, expr)
    , cast_to_type(cast_to_type) {}
};


//
// ---------------------------------------------------------
//     statements
//


template<>
// ast_empty_statement is very similar to "empty sequence" but has a special treatment
// example: `;` (just semicolon)
// example: body of `builtin` function is empty statement (not a zero sequence)
struct Vertex<ast_empty_statement> final : ASTStatementVararg {
  explicit Vertex(SrcLocation loc)
    : ASTStatementVararg(ast_empty_statement, loc, {}) {}
};

template<>
// ast_sequence is "some sequence of statements"
// example: function body is a sequence
// example: do while body is a sequence
struct Vertex<ast_sequence> final : ASTStatementVararg {
  SrcLocation loc_end;

  const std::vector<AnyV>& get_items() const { return children; }
  AnyV get_item(int i) const { return children.at(i); }

  Vertex(SrcLocation loc, SrcLocation loc_end, std::vector<AnyV> items)
    : ASTStatementVararg(ast_sequence, loc, std::move(items))
    , loc_end(loc_end) {}
};

template<>
// ast_return_statement is "return something from a function"
// examples: `return a` / `return any_expr()()` / `return;`
// note, that for `return;` (without a value, meaning "void"), in AST, it's stored as empty expression
struct Vertex<ast_return_statement> : ASTStatementUnary {
  AnyExprV get_return_value() const { return child_as_expr(); }
  bool has_return_value() const { return child->type != ast_empty_expression; }

  Vertex(SrcLocation loc, AnyExprV child)
    : ASTStatementUnary(ast_return_statement, loc, child) {}
};

template<>
// ast_if_statement is a traditional if statement, probably followed by an else branch
// examples: `if (cond) { ... } else { ... }` / `if (cond) { ... }`
// when else branch is missing, it's stored as empty statement
// for "else if", it's just "if statement" inside a sequence of else branch
struct Vertex<ast_if_statement> final : ASTStatementVararg {
  bool is_ifnot;  // if(!cond), to generate more optimal fift code

  AnyExprV get_cond() const { return child_as_expr(0); }
  auto get_if_body() const { return children.at(1)->as<ast_sequence>(); }
  auto get_else_body() const { return children.at(2)->as<ast_sequence>(); }    // always exists (when else omitted, it's empty)

  Vertex(SrcLocation loc, bool is_ifnot, AnyExprV cond, V<ast_sequence> if_body, V<ast_sequence> else_body)
    : ASTStatementVararg(ast_if_statement, loc, {cond, if_body, else_body})
    , is_ifnot(is_ifnot) {}
};

template<>
// ast_repeat_statement is "repeat something N times"
// example: `repeat (10) { ... }`
struct Vertex<ast_repeat_statement> final : ASTStatementVararg {
  AnyExprV get_cond() const { return child_as_expr(0); }
  auto get_body() const { return children.at(1)->as<ast_sequence>(); }

  Vertex(SrcLocation loc, AnyExprV cond, V<ast_sequence> body)
    : ASTStatementVararg(ast_repeat_statement, loc, {cond, body}) {}
};

template<>
// ast_while_statement is a standard "while" loop
// example: `while (x > 0) { ... }`
struct Vertex<ast_while_statement> final : ASTStatementVararg {
  AnyExprV get_cond() const { return child_as_expr(0); }
  auto get_body() const { return children.at(1)->as<ast_sequence>(); }

  Vertex(SrcLocation loc, AnyExprV cond, V<ast_sequence> body)
    : ASTStatementVararg(ast_while_statement, loc, {cond, body}) {}
};

template<>
// ast_do_while_statement is a standard "do while" loop
// example: `do { ... } while (x > 0);`
struct Vertex<ast_do_while_statement> final : ASTStatementVararg {
  auto get_body() const { return children.at(0)->as<ast_sequence>(); }
  AnyExprV get_cond() const { return child_as_expr(1); }

  Vertex(SrcLocation loc, V<ast_sequence> body, AnyExprV cond)
    : ASTStatementVararg(ast_do_while_statement, loc, {body, cond}) {}
};

template<>
// ast_throw_statement is throwing an exception, it accepts excNo and optional arg
// examples: `throw 10` / `throw (ERR_LOW_BALANCE)` / `throw (1001, incomingAddr)`
// when thrown arg is missing, it's stored as empty expression
struct Vertex<ast_throw_statement> final : ASTStatementVararg {
  AnyExprV get_thrown_code() const { return child_as_expr(0); }
  bool has_thrown_arg() const { return child_as_expr(1)->type != ast_empty_expression; }
  AnyExprV get_thrown_arg() const { return child_as_expr(1); }

  Vertex(SrcLocation loc, AnyExprV thrown_code, AnyExprV thrown_arg)
    : ASTStatementVararg(ast_throw_statement, loc, {thrown_code, thrown_arg}) {}
};

template<>
// ast_assert_statement is "assert that cond is true, otherwise throw an exception"
// examples: `assert (balance > 0, ERR_ZERO_BALANCE)` / `assert (balance > 0) throw (ERR_ZERO_BALANCE)`
struct Vertex<ast_assert_statement> final : ASTStatementVararg {
  AnyExprV get_cond() const { return child_as_expr(0); }
  AnyExprV get_thrown_code() const { return child_as_expr(1); }

  Vertex(SrcLocation loc, AnyExprV cond, AnyExprV thrown_code)
    : ASTStatementVararg(ast_assert_statement, loc, {cond, thrown_code}) {}
};

template<>
// ast_try_catch_statement is a standard try catch (finally block doesn't exist)
// example: `try { ... } catch (excNo) { ... }`
// there are two formal "arguments" of catch: excNo and arg, but both can be omitted
// when omitted, they are stored as underscores, so len of a catch tensor is always 2
struct Vertex<ast_try_catch_statement> final : ASTStatementVararg {
  auto get_try_body() const { return children.at(0)->as<ast_sequence>(); }
  auto get_catch_expr() const { return children.at(1)->as<ast_tensor>(); }    // (excNo, arg), always len 2
  auto get_catch_body() const { return children.at(2)->as<ast_sequence>(); }

  Vertex(SrcLocation loc, V<ast_sequence> try_body, V<ast_tensor> catch_expr, V<ast_sequence> catch_body)
    : ASTStatementVararg(ast_try_catch_statement, loc, {try_body, catch_expr, catch_body}) {}
};

template<>
// ast_asm_body is a body of `asm` function — a set of strings, and optionally stack order manipulations
// example: `fun skipMessageOp... asm "32 PUSHINT" "SDSKIPFIRST";`
// user can specify "arg order"; example: `fun store(self: builder, op: int) asm (op self)` then [1, 0]
// user can specify "ret order"; example: `fun modDiv... asm(-> 1 0) "DIVMOD";` then [1, 0]
struct Vertex<ast_asm_body> final : ASTStatementVararg {
  std::vector<int> arg_order;
  std::vector<int> ret_order;

  const std::vector<AnyV>& get_asm_commands() const { return children; }    // ast_string_const[]

  Vertex(SrcLocation loc, std::vector<int> arg_order, std::vector<int> ret_order, std::vector<AnyV> asm_commands)
    : ASTStatementVararg(ast_asm_body, loc, std::move(asm_commands))
    , arg_order(std::move(arg_order)), ret_order(std::move(ret_order)) {}
};


//
// ---------------------------------------------------------
//     other
//


template<>
// ast_genericsT_item is generics T at declaration
// example: `fun f<T1, T2>` has a list of 2 generic Ts
struct Vertex<ast_genericsT_item> final : ASTOtherLeaf {
  std::string_view nameT;

  Vertex(SrcLocation loc, std::string_view nameT)
    : ASTOtherLeaf(ast_genericsT_item, loc)
    , nameT(nameT) {}
};

template<>
// ast_genericsT_list is a container for generics T at declaration
// example: see above
struct Vertex<ast_genericsT_list> final : ASTOtherVararg {
  std::vector<AnyV> get_items() const { return children; }
  auto get_item(int i) const { return children.at(i)->as<ast_genericsT_item>(); }

  Vertex(SrcLocation loc, std::vector<AnyV> genericsT_items)
    : ASTOtherVararg(ast_genericsT_list, loc, std::move(genericsT_items)) {}

  int lookup_idx(std::string_view nameT) const;
};


template<>
// ast_instantiationT_item is manual substitution of generic T used in code, mostly for func calls
// examples: `g<int>()` / `t.tupleFirst<slice>()` / `f<(int, slice), builder>()`
struct Vertex<ast_instantiationT_item> final : ASTOtherLeaf {
  TypePtr substituted_type;

  Vertex* mutate() const { return const_cast<Vertex*>(this); }
  void assign_resolved_type(TypePtr substituted_type);

  Vertex(SrcLocation loc, TypePtr substituted_type)
    : ASTOtherLeaf(ast_instantiationT_item, loc)
    , substituted_type(substituted_type) {}
};

template<>
// ast_instantiationT_list is a container for generic T substitutions used in code
// examples: see above
struct Vertex<ast_instantiationT_list> final : ASTOtherVararg {
  std::vector<AnyV> get_items() const { return children; }
  auto get_item(int i) const { return children.at(i)->as<ast_instantiationT_item>(); }

  Vertex(SrcLocation loc, std::vector<AnyV> instantiationTs)
    : ASTOtherVararg(ast_instantiationT_list, loc, std::move(instantiationTs)) {}
};

template<>
// ast_parameter is a parameter of a function in its declaration
// example: `fun f(a: int, mutate b: slice)` has 2 parameters
struct Vertex<ast_parameter> final : ASTOtherLeaf {
  const LocalVarData* param_ref = nullptr;    // filled on resolve identifiers
  std::string_view param_name;
  TypePtr declared_type;
  bool declared_as_mutate;                    // declared as `mutate param_name`

  bool is_underscore() const { return param_name.empty(); }

  Vertex* mutate() const { return const_cast<Vertex*>(this); }
  void assign_param_ref(const LocalVarData* param_ref);
  void assign_resolved_type(TypePtr declared_type);

  Vertex(SrcLocation loc, std::string_view param_name, TypePtr declared_type, bool declared_as_mutate)
    : ASTOtherLeaf(ast_parameter, loc)
    , param_name(param_name), declared_type(declared_type), declared_as_mutate(declared_as_mutate) {}
};

template<>
// ast_parameter_list is a container of parameters
// example: see above
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
// ast_annotation is @annotation above a declaration
// example: `@pure fun ...`
struct Vertex<ast_annotation> final : ASTOtherVararg {
  AnnotationKind kind;

  auto get_arg() const { return children.at(0)->as<ast_tensor>(); }

  static AnnotationKind parse_kind(std::string_view name);

  Vertex(SrcLocation loc, AnnotationKind kind, V<ast_tensor> arg_probably_empty)
    : ASTOtherVararg(ast_annotation, loc, {arg_probably_empty})
    , kind(kind) {}
};

template<>
// ast_function_declaration is declaring a function/method
// methods are still global functions, just accepting "self" first parameter
// example: `fun f() { ... }`
// functions can be generic, `fun f<T>(params) { ... }`
// their body is either sequence (regular code function), or `asm`, or `builtin`
struct Vertex<ast_function_declaration> final : ASTOtherVararg {
  auto get_identifier() const { return children.at(0)->as<ast_identifier>(); }
  int get_num_params() const { return children.at(1)->as<ast_parameter_list>()->size(); }
  auto get_param_list() const { return children.at(1)->as<ast_parameter_list>(); }
  auto get_param(int i) const { return children.at(1)->as<ast_parameter_list>()->get_param(i); }
  AnyV get_body() const { return children.at(2); }   // ast_sequence / ast_asm_body

  const FunctionData* fun_ref = nullptr;  // filled after register
  TypePtr declared_return_type;           // filled at ast parsing; if unspecified (nullptr), means "auto infer"
  V<ast_genericsT_list> genericsT_list;   // for non-generics it's nullptr
  td::RefInt256 method_id;                // specified via @method_id annotation
  int flags;                              // from enum in FunctionData

  bool is_asm_function() const { return children.at(2)->type == ast_asm_body; }
  bool is_code_function() const { return children.at(2)->type == ast_sequence; }
  bool is_builtin_function() const { return children.at(2)->type == ast_empty_statement; }

  Vertex* mutate() const { return const_cast<Vertex*>(this); }
  void assign_fun_ref(const FunctionData* fun_ref);
  void assign_resolved_type(TypePtr declared_return_type);

  Vertex(SrcLocation loc, V<ast_identifier> name_identifier, V<ast_parameter_list> parameters, AnyV body, TypePtr declared_return_type, V<ast_genericsT_list> genericsT_list, td::RefInt256 method_id, int flags)
    : ASTOtherVararg(ast_function_declaration, loc, {name_identifier, parameters, body})
    , declared_return_type(declared_return_type), genericsT_list(genericsT_list), method_id(std::move(method_id)), flags(flags) {}
};

template<>
// ast_global_var_declaration is declaring a global var, outside a function
// example: `global g: int;`
// note, that globals don't have default values, since there is no single "entrypoint" for a contract
struct Vertex<ast_global_var_declaration> final : ASTOtherVararg {
  const GlobalVarData* var_ref = nullptr;  // filled after register
  TypePtr declared_type;                   // filled always, typing globals is mandatory

  auto get_identifier() const { return children.at(0)->as<ast_identifier>(); }

  Vertex* mutate() const { return const_cast<Vertex*>(this); }
  void assign_var_ref(const GlobalVarData* var_ref);
  void assign_resolved_type(TypePtr declared_type);

  Vertex(SrcLocation loc, V<ast_identifier> name_identifier, TypePtr declared_type)
    : ASTOtherVararg(ast_global_var_declaration, loc, {name_identifier})
    , declared_type(declared_type) {}
};

template<>
// ast_constant_declaration is declaring a global constant, outside a function
// example: `const op = 0x123;`
struct Vertex<ast_constant_declaration> final : ASTOtherVararg {
  const GlobalConstData* const_ref = nullptr;  // filled after register
  TypePtr declared_type;                       // not null for `const op: int = ...`

  auto get_identifier() const { return children.at(0)->as<ast_identifier>(); }
  AnyExprV get_init_value() const { return child_as_expr(1); }

  Vertex* mutate() const { return const_cast<Vertex*>(this); }
  void assign_const_ref(const GlobalConstData* const_ref);
  void assign_resolved_type(TypePtr declared_type);

  Vertex(SrcLocation loc, V<ast_identifier> name_identifier, TypePtr declared_type, AnyExprV init_value)
    : ASTOtherVararg(ast_constant_declaration, loc, {name_identifier, init_value})
    , declared_type(declared_type) {}
};

template<>
// ast_tolk_required_version is a preamble fixating compiler's version at the top of the file
// example: `tolk 0.6`
// when compiler version mismatches, it means, that another compiler was earlier for that sources, a warning is emitted
struct Vertex<ast_tolk_required_version> final : ASTOtherLeaf {
  std::string_view semver;

  Vertex(SrcLocation loc, std::string_view semver)
    : ASTOtherLeaf(ast_tolk_required_version, loc)
    , semver(semver) {}
};

template<>
// ast_import_directive is an import at the top of the file
// examples: `import "another.tolk"` / `import "@stdlib/tvm-dicts"`
struct Vertex<ast_import_directive> final : ASTOtherVararg {
  const SrcFile* file = nullptr;    // assigned after imports have been resolved, just after parsing a file to ast

  auto get_file_leaf() const { return children.at(0)->as<ast_string_const>(); }

  std::string get_file_name() const { return static_cast<std::string>(children.at(0)->as<ast_string_const>()->str_val); }

  Vertex* mutate() const { return const_cast<Vertex*>(this); }
  void assign_src_file(const SrcFile* file);

  Vertex(SrcLocation loc, V<ast_string_const> file_name)
    : ASTOtherVararg(ast_import_directive, loc, {file_name}) {}
};

template<>
// ast_tolk_file represents a whole parsed input .tolk file
// with functions, constants, etc.
// particularly, it contains imports that lead to loading other files
// a whole program consists of multiple parsed files, each of them has a parsed ast tree (stdlib is also parsed)
struct Vertex<ast_tolk_file> final : ASTOtherVararg {
  const SrcFile* const file;

  const std::vector<AnyV>& get_toplevel_declarations() const { return children; }

  Vertex(const SrcFile* file, std::vector<AnyV> toplevel_declarations)
    : ASTOtherVararg(ast_tolk_file, SrcLocation(file), std::move(toplevel_declarations))
    , file(file) {}
};

} // namespace tolk
