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
 *   A module of implementing traversing a vertex tree and replacing any vertex to another.
 *   For example, to replace "beginCell()" call to "begin_cell()" in a function body (in V<ast_function>)
 * regardless of the place this call is performed, you need to iterate over all the function AST,
 * to find ast_function_call(beginCell), create ast_function_call(begin_cell) instead and to replace
 * a pointer inside its parent.
 *   Inheriting from ASTVisitor makes this task quite simple, without any boilerplate.
 *
 *   If you need just to traverse a vertex tree without replacing vertices,
 * consider another api: ast-visitor.h.
 */

namespace tolk {

class ASTReplacer {
protected:
  GNU_ATTRIBUTE_ALWAYS_INLINE static AnyExprV replace_children(const ASTExprLeaf* v) {
    return v;
  }

  GNU_ATTRIBUTE_ALWAYS_INLINE AnyExprV replace_children(const ASTExprUnary* v) {
    auto* v_mutable = const_cast<ASTExprUnary*>(v);
    v_mutable->child = replace(v_mutable->child);
    return v_mutable;
  }

  GNU_ATTRIBUTE_ALWAYS_INLINE AnyExprV replace_children(const ASTExprBinary* v) {
    auto* v_mutable = const_cast<ASTExprBinary*>(v);
    v_mutable->lhs = replace(v->lhs);
    v_mutable->rhs = replace(v->rhs);
    return v_mutable;
  }

  GNU_ATTRIBUTE_ALWAYS_INLINE AnyExprV replace_children(const ASTExprVararg* v) {
    auto* v_mutable = const_cast<ASTExprVararg*>(v);
    for (AnyExprV& child : v_mutable->children) {
      child = replace(child);
    }
    return v_mutable;
  }

  GNU_ATTRIBUTE_ALWAYS_INLINE AnyExprV replace_children(const ASTExprBlockOfStatements* v) {
    auto* v_mutable = const_cast<ASTExprBlockOfStatements*>(v);
    v_mutable->child_block_statement = replace(v_mutable->child_block_statement->as<ast_block_statement>());
    return v_mutable;
  }

  GNU_ATTRIBUTE_ALWAYS_INLINE AnyV replace_children(const ASTStatementUnary* v) {
    auto* v_mutable = const_cast<ASTStatementUnary*>(v);
    v_mutable->child = replace(v_mutable->child);
    return v_mutable;
  }

  GNU_ATTRIBUTE_ALWAYS_INLINE AnyV replace_children(const ASTStatementVararg* v) {
    auto* v_mutable = const_cast<ASTStatementVararg*>(v);
    for (AnyV& child : v_mutable->children) {
      child = replace(child);
    }
    return v_mutable;
  }

public:
  virtual ~ASTReplacer() = default;

  virtual AnyV replace(AnyV v) = 0;
  virtual AnyExprV replace(AnyExprV v) = 0;
};

class ASTReplacerInFunctionBody : public ASTReplacer {
protected:
  using parent = ASTReplacerInFunctionBody;

  // expressions
  virtual AnyExprV replace(V<ast_empty_expression> v)          { return replace_children(v); }
  virtual AnyExprV replace(V<ast_parenthesized_expression> v)  { return replace_children(v); }
  virtual AnyExprV replace(V<ast_braced_expression> v)         { return replace_children(v); }
  virtual AnyExprV replace(V<ast_artificial_aux_vertex> v)     { return replace_children(v); }
  virtual AnyExprV replace(V<ast_tensor> v)                    { return replace_children(v); }
  virtual AnyExprV replace(V<ast_bracket_tuple> v)             { return replace_children(v); }
  virtual AnyExprV replace(V<ast_reference> v)                 { return replace_children(v); }
  virtual AnyExprV replace(V<ast_local_var_lhs> v)             { return replace_children(v); }
  virtual AnyExprV replace(V<ast_local_vars_declaration> v)    { return replace_children(v); }
  virtual AnyExprV replace(V<ast_int_const> v)                 { return replace_children(v); }
  virtual AnyExprV replace(V<ast_string_const> v)              { return replace_children(v); }
  virtual AnyExprV replace(V<ast_bool_const> v)                { return replace_children(v); }
  virtual AnyExprV replace(V<ast_null_keyword> v)              { return replace_children(v); }
  virtual AnyExprV replace(V<ast_argument> v)                  { return replace_children(v); }
  virtual AnyExprV replace(V<ast_argument_list> v)             { return replace_children(v); }
  virtual AnyExprV replace(V<ast_dot_access> v)                { return replace_children(v); }
  virtual AnyExprV replace(V<ast_function_call> v)             { return replace_children(v); }
  virtual AnyExprV replace(V<ast_underscore> v)                { return replace_children(v); }
  virtual AnyExprV replace(V<ast_assign> v)                    { return replace_children(v); }
  virtual AnyExprV replace(V<ast_set_assign> v)                { return replace_children(v); }
  virtual AnyExprV replace(V<ast_unary_operator> v)            { return replace_children(v); }
  virtual AnyExprV replace(V<ast_binary_operator> v)           { return replace_children(v); }
  virtual AnyExprV replace(V<ast_ternary_operator> v)          { return replace_children(v); }
  virtual AnyExprV replace(V<ast_cast_as_operator> v)          { return replace_children(v); }
  virtual AnyExprV replace(V<ast_is_type_operator> v)          { return replace_children(v); }
  virtual AnyExprV replace(V<ast_not_null_operator> v)         { return replace_children(v); }
  virtual AnyExprV replace(V<ast_match_expression> v)          { return replace_children(v); }
  virtual AnyExprV replace(V<ast_match_arm> v)                 { return replace_children(v); }
  virtual AnyExprV replace(V<ast_object_field> v)              { return replace_children(v); }
  virtual AnyExprV replace(V<ast_object_body> v)               { return replace_children(v); }
  virtual AnyExprV replace(V<ast_object_literal> v)            { return replace_children(v); }
  // statements
  virtual AnyV replace(V<ast_empty_statement> v)               { return replace_children(v); }
  virtual AnyV replace(V<ast_block_statement> v)               { return replace_children(v); }
  virtual AnyV replace(V<ast_return_statement> v)              { return replace_children(v); }
  virtual AnyV replace(V<ast_if_statement> v)                  { return replace_children(v); }
  virtual AnyV replace(V<ast_repeat_statement> v)              { return replace_children(v); }
  virtual AnyV replace(V<ast_while_statement> v)               { return replace_children(v); }
  virtual AnyV replace(V<ast_do_while_statement> v)            { return replace_children(v); }
  virtual AnyV replace(V<ast_throw_statement> v)               { return replace_children(v); }
  virtual AnyV replace(V<ast_assert_statement> v)              { return replace_children(v); }
  virtual AnyV replace(V<ast_try_catch_statement> v)           { return replace_children(v); }

  AnyExprV replace(AnyExprV v) final {
    switch (v->kind) {
      case ast_empty_expression:                return replace(v->as<ast_empty_expression>());
      case ast_parenthesized_expression:        return replace(v->as<ast_parenthesized_expression>());
      case ast_braced_expression:               return replace(v->as<ast_braced_expression>());
      case ast_artificial_aux_vertex:           return replace(v->as<ast_artificial_aux_vertex>());
      case ast_tensor:                          return replace(v->as<ast_tensor>());
      case ast_bracket_tuple:                   return replace(v->as<ast_bracket_tuple>());
      case ast_reference:                       return replace(v->as<ast_reference>());
      case ast_local_var_lhs:                   return replace(v->as<ast_local_var_lhs>());
      case ast_local_vars_declaration:          return replace(v->as<ast_local_vars_declaration>());
      case ast_int_const:                       return replace(v->as<ast_int_const>());
      case ast_string_const:                    return replace(v->as<ast_string_const>());
      case ast_bool_const:                      return replace(v->as<ast_bool_const>());
      case ast_null_keyword:                    return replace(v->as<ast_null_keyword>());
      case ast_argument:                        return replace(v->as<ast_argument>());
      case ast_argument_list:                   return replace(v->as<ast_argument_list>());
      case ast_dot_access:                      return replace(v->as<ast_dot_access>());
      case ast_function_call:                   return replace(v->as<ast_function_call>());
      case ast_underscore:                      return replace(v->as<ast_underscore>());
      case ast_assign:                          return replace(v->as<ast_assign>());
      case ast_set_assign:                      return replace(v->as<ast_set_assign>());
      case ast_unary_operator:                  return replace(v->as<ast_unary_operator>());
      case ast_binary_operator:                 return replace(v->as<ast_binary_operator>());
      case ast_ternary_operator:                return replace(v->as<ast_ternary_operator>());
      case ast_cast_as_operator:                return replace(v->as<ast_cast_as_operator>());
      case ast_is_type_operator:                return replace(v->as<ast_is_type_operator>());
      case ast_not_null_operator:               return replace(v->as<ast_not_null_operator>());
      case ast_match_expression:                return replace(v->as<ast_match_expression>());
      case ast_match_arm:                       return replace(v->as<ast_match_arm>());
      case ast_object_field:                    return replace(v->as<ast_object_field>());
      case ast_object_body:                     return replace(v->as<ast_object_body>());
      case ast_object_literal:                  return replace(v->as<ast_object_literal>());
      default:
        throw UnexpectedASTNodeKind(v, "ASTReplacerInFunctionBody::replace");
    }
  }

  AnyV replace(AnyV v) final {
    switch (v->kind) {
      case ast_empty_statement:                 return replace(v->as<ast_empty_statement>());
      case ast_block_statement:                 return replace(v->as<ast_block_statement>());
      case ast_return_statement:                return replace(v->as<ast_return_statement>());
      case ast_if_statement:                    return replace(v->as<ast_if_statement>());
      case ast_repeat_statement:                return replace(v->as<ast_repeat_statement>());
      case ast_while_statement:                 return replace(v->as<ast_while_statement>());
      case ast_do_while_statement:              return replace(v->as<ast_do_while_statement>());
      case ast_throw_statement:                 return replace(v->as<ast_throw_statement>());
      case ast_assert_statement:                return replace(v->as<ast_assert_statement>());
      case ast_try_catch_statement:             return replace(v->as<ast_try_catch_statement>());
#ifdef TOLK_DEBUG
      case ast_asm_body:
        throw UnexpectedASTNodeKind(v, "ASTReplacer::replace");
#endif
      default: {
        // be very careful, don't forget to handle all statements (not expressions) above!
        AnyExprV as_expr = reinterpret_cast<const ASTNodeExpressionBase*>(v);
        return replace(as_expr);
      }
    }
  }

public:
  virtual bool should_visit_function(FunctionPtr fun_ref) = 0;

  void start_replacing_in_function(FunctionPtr fun_ref, V<ast_function_declaration> v_function) {
    replace(v_function->get_body());
  }
};


const std::vector<FunctionPtr>& get_all_not_builtin_functions();
const std::vector<GlobalConstPtr>& get_all_declared_constants();
const std::vector<StructPtr>& get_all_declared_structs();

template<class BodyReplacerT>
void replace_ast_of_all_functions() {
  BodyReplacerT visitor;
  for (FunctionPtr fun_ref : get_all_not_builtin_functions()) {
    if (visitor.should_visit_function(fun_ref)) {
      visitor.start_replacing_in_function(fun_ref, fun_ref->ast_root->as<ast_function_declaration>());
    }
  }
}

} // namespace tolk
