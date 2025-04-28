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

#include "ast.h"
#include "platform-utils.h"

/*
 *   A module implementing base functionality of read-only traversing a vertex tree.
 *   Since a vertex in general doesn't store a vector of children, iterating is possible only for concrete node_kind.
 * E.g., for ast_if_statement, visit nodes cond, if-body and else-body. For ast_string_const, nothing. And so on.
 *   Visitors below are helpers to inherit from and handle specific vertex types.
 *
 *   Note, that absence of "children" in ASTNodeBase is not a drawback. Instead, it encourages you to think
 * about types and match the type system.
 *
 *   The visitor is read-only, it does not modify visited nodes (except if you purposely call mutating methods).
 *   For example, if you want to replace "beginCell()" call with "begin_cell", a visitor isn't enough for you.
 *   To replace vertices, consider another API: ast-replacer.h.
 */

namespace tolk {

class ASTVisitor {
protected:
  GNU_ATTRIBUTE_ALWAYS_INLINE static void visit_children(const ASTTypeLeaf* v) {
    static_cast<void>(v);
  }

  GNU_ATTRIBUTE_ALWAYS_INLINE void visit_children(const ASTTypeVararg* v) {
    for (AnyTypeV child : v->children) {
      visit(child);
    }
  }

  GNU_ATTRIBUTE_ALWAYS_INLINE static void visit_children(const ASTExprLeaf* v) {
    static_cast<void>(v);
  }

  GNU_ATTRIBUTE_ALWAYS_INLINE void visit_children(const ASTExprUnary* v) {
    visit(v->child);
  }

  GNU_ATTRIBUTE_ALWAYS_INLINE void visit_children(const ASTExprBinary* v) {
    visit(v->lhs);
    visit(v->rhs);
  }

  GNU_ATTRIBUTE_ALWAYS_INLINE void visit_children(const ASTExprVararg* v) {
    for (AnyExprV child : v->children) {
      visit(child);
    }
  }

  GNU_ATTRIBUTE_ALWAYS_INLINE void visit_children(const ASTExprBlockOfStatements* v) {
    visit_children(v->child_block_statement->as<ast_block_statement>());
  }

  GNU_ATTRIBUTE_ALWAYS_INLINE void visit_children(const ASTStatementUnary* v) {
    visit(v->child);
  }

  GNU_ATTRIBUTE_ALWAYS_INLINE void visit_children(const ASTStatementVararg* v) {
    for (AnyV child : v->children) {
      visit(child);
    }
  }

  GNU_ATTRIBUTE_ALWAYS_INLINE static void visit_children(const ASTOtherLeaf* v) {
    static_cast<void>(v);
  }

  GNU_ATTRIBUTE_ALWAYS_INLINE void visit_children(const ASTOtherVararg* v) {
    for (AnyV child : v->children) {
      visit(child);
    }
  }

  virtual void visit(AnyV v) = 0;

public:
  virtual ~ASTVisitor() = default;
};

class ASTVisitorFunctionBody : public ASTVisitor {
protected:
  using parent = ASTVisitorFunctionBody;

  // expressions
  virtual void visit(V<ast_empty_expression> v)          { return visit_children(v); }
  virtual void visit(V<ast_parenthesized_expression> v)  { return visit_children(v); }
  virtual void visit(V<ast_braced_expression> v)         { return visit_children(v); }
  virtual void visit(V<ast_artificial_aux_vertex> v)     { return visit_children(v); }
  virtual void visit(V<ast_tensor> v)                    { return visit_children(v); }
  virtual void visit(V<ast_bracket_tuple> v)             { return visit_children(v); }
  virtual void visit(V<ast_reference> v)                 { return visit_children(v); }
  virtual void visit(V<ast_local_var_lhs> v)             { return visit_children(v); }
  virtual void visit(V<ast_local_vars_declaration> v)    { return visit_children(v); }
  virtual void visit(V<ast_int_const> v)                 { return visit_children(v); }
  virtual void visit(V<ast_string_const> v)              { return visit_children(v); }
  virtual void visit(V<ast_bool_const> v)                { return visit_children(v); }
  virtual void visit(V<ast_null_keyword> v)              { return visit_children(v); }
  virtual void visit(V<ast_argument> v)                  { return visit_children(v); }
  virtual void visit(V<ast_argument_list> v)             { return visit_children(v); }
  virtual void visit(V<ast_dot_access> v)                { return visit_children(v); }
  virtual void visit(V<ast_function_call> v)             { return visit_children(v); }
  virtual void visit(V<ast_underscore> v)                { return visit_children(v); }
  virtual void visit(V<ast_assign> v)                    { return visit_children(v); }
  virtual void visit(V<ast_set_assign> v)                { return visit_children(v); }
  virtual void visit(V<ast_unary_operator> v)            { return visit_children(v); }
  virtual void visit(V<ast_binary_operator> v)           { return visit_children(v); }
  virtual void visit(V<ast_ternary_operator> v)          { return visit_children(v); }
  virtual void visit(V<ast_cast_as_operator> v)          { return visit_children(v); }
  virtual void visit(V<ast_is_type_operator> v)          { return visit_children(v); }
  virtual void visit(V<ast_not_null_operator> v)         { return visit_children(v); }
  virtual void visit(V<ast_match_expression> v)          { return visit_children(v); }
  virtual void visit(V<ast_match_arm> v)                 { return visit_children(v); }
  virtual void visit(V<ast_object_field> v)              { return visit_children(v); }
  virtual void visit(V<ast_object_body> v)               { return visit_children(v); }
  virtual void visit(V<ast_object_literal> v)            { return visit_children(v); }
  // statements
  virtual void visit(V<ast_empty_statement> v)           { return visit_children(v); }
  virtual void visit(V<ast_block_statement> v)           { return visit_children(v); }
  virtual void visit(V<ast_return_statement> v)          { return visit_children(v); }
  virtual void visit(V<ast_if_statement> v)              { return visit_children(v); }
  virtual void visit(V<ast_repeat_statement> v)          { return visit_children(v); }
  virtual void visit(V<ast_while_statement> v)           { return visit_children(v); }
  virtual void visit(V<ast_do_while_statement> v)        { return visit_children(v); }
  virtual void visit(V<ast_throw_statement> v)           { return visit_children(v); }
  virtual void visit(V<ast_assert_statement> v)          { return visit_children(v); }
  virtual void visit(V<ast_try_catch_statement> v)       { return visit_children(v); }

  void visit(AnyV v) final {
    switch (v->kind) {
      // expressions
      case ast_empty_expression:                return visit(v->as<ast_empty_expression>());
      case ast_parenthesized_expression:        return visit(v->as<ast_parenthesized_expression>());
      case ast_braced_expression:               return visit(v->as<ast_braced_expression>());
      case ast_artificial_aux_vertex:           return visit(v->as<ast_artificial_aux_vertex>());
      case ast_tensor:                          return visit(v->as<ast_tensor>());
      case ast_bracket_tuple:                   return visit(v->as<ast_bracket_tuple>());
      case ast_reference:                       return visit(v->as<ast_reference>());
      case ast_local_var_lhs:                   return visit(v->as<ast_local_var_lhs>());
      case ast_local_vars_declaration:          return visit(v->as<ast_local_vars_declaration>());
      case ast_int_const:                       return visit(v->as<ast_int_const>());
      case ast_string_const:                    return visit(v->as<ast_string_const>());
      case ast_bool_const:                      return visit(v->as<ast_bool_const>());
      case ast_null_keyword:                    return visit(v->as<ast_null_keyword>());
      case ast_argument:                        return visit(v->as<ast_argument>());
      case ast_argument_list:                   return visit(v->as<ast_argument_list>());
      case ast_dot_access:                      return visit(v->as<ast_dot_access>());
      case ast_function_call:                   return visit(v->as<ast_function_call>());
      case ast_underscore:                      return visit(v->as<ast_underscore>());
      case ast_assign:                          return visit(v->as<ast_assign>());
      case ast_set_assign:                      return visit(v->as<ast_set_assign>());
      case ast_unary_operator:                  return visit(v->as<ast_unary_operator>());
      case ast_binary_operator:                 return visit(v->as<ast_binary_operator>());
      case ast_ternary_operator:                return visit(v->as<ast_ternary_operator>());
      case ast_cast_as_operator:                return visit(v->as<ast_cast_as_operator>());
      case ast_is_type_operator:                return visit(v->as<ast_is_type_operator>());
      case ast_not_null_operator:               return visit(v->as<ast_not_null_operator>());
      case ast_match_expression:                return visit(v->as<ast_match_expression>());
      case ast_match_arm:                       return visit(v->as<ast_match_arm>());
      case ast_object_field:                    return visit(v->as<ast_object_field>());
      case ast_object_body:                     return visit(v->as<ast_object_body>());
      case ast_object_literal:                  return visit(v->as<ast_object_literal>());
      // statements
      case ast_empty_statement:                 return visit(v->as<ast_empty_statement>());
      case ast_block_statement:                 return visit(v->as<ast_block_statement>());
      case ast_return_statement:                return visit(v->as<ast_return_statement>());
      case ast_if_statement:                    return visit(v->as<ast_if_statement>());
      case ast_repeat_statement:                return visit(v->as<ast_repeat_statement>());
      case ast_while_statement:                 return visit(v->as<ast_while_statement>());
      case ast_do_while_statement:              return visit(v->as<ast_do_while_statement>());
      case ast_throw_statement:                 return visit(v->as<ast_throw_statement>());
      case ast_assert_statement:                return visit(v->as<ast_assert_statement>());
      case ast_try_catch_statement:             return visit(v->as<ast_try_catch_statement>());
#ifdef TOLK_DEBUG
      case ast_asm_body:
        throw UnexpectedASTNodeKind(v, "ASTVisitor; forgot to filter out asm functions in should_visit_function()?");
#endif
      default:
        throw UnexpectedASTNodeKind(v, "ASTVisitorFunctionBody::visit");
    }
  }

public:
  virtual bool should_visit_function(FunctionPtr fun_ref) = 0;

  virtual void start_visiting_function(FunctionPtr fun_ref, V<ast_function_declaration> v_function) {
    visit(v_function->get_body());
  }
};


const std::vector<FunctionPtr>& get_all_not_builtin_functions();
const std::vector<GlobalConstPtr>& get_all_declared_constants();
const std::vector<StructPtr>& get_all_declared_structs();

template<class BodyVisitorT>
void visit_ast_of_all_functions() {
  BodyVisitorT visitor;
  const std::vector<FunctionPtr>& all = get_all_not_builtin_functions();
  for (size_t i = 0; i < all.size(); ++i) { // NOLINT(*-loop-convert)
    FunctionPtr fun_ref = all[i];   // not range-base loop to prevent iterator invalidation (push_back at generics)
    if (visitor.should_visit_function(fun_ref)) {
      visitor.start_visiting_function(fun_ref, fun_ref->ast_root->as<ast_function_declaration>());
    }
  }
}

} // namespace tolk
