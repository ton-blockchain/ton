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
  GNU_ATTRIBUTE_ALWAYS_INLINE static AnyV replace_children(const ASTNodeLeaf* v) {
    return v;
  }

  GNU_ATTRIBUTE_ALWAYS_INLINE AnyV replace_children(const ASTNodeUnary* v) {
    auto* v_mutable = const_cast<ASTNodeUnary*>(v);
    v_mutable->child = replace(v_mutable->child);
    return v_mutable;
  }

  GNU_ATTRIBUTE_ALWAYS_INLINE AnyV replace_children(const ASTNodeBinary* v) {
    auto* v_mutable = const_cast<ASTNodeBinary*>(v);
    v_mutable->lhs = replace(v->lhs);
    v_mutable->rhs = replace(v->rhs);
    return v_mutable;
  }

  GNU_ATTRIBUTE_ALWAYS_INLINE AnyV replace_children(const ASTNodeVararg* v) {
    auto* v_mutable = const_cast<ASTNodeVararg*>(v);
    for (AnyV& child : v_mutable->children) {
      child = replace(child);
    }
    return v_mutable;
  }

public:
  virtual ~ASTReplacer() = default;

  virtual AnyV replace(AnyV v) = 0;
};

class ASTReplacerInFunctionBody : public ASTReplacer {
protected:
  using parent = ASTReplacerInFunctionBody;

  virtual AnyV replace(V<ast_empty> v)                   { return replace_children(v); }
  virtual AnyV replace(V<ast_parenthesized_expr> v)      { return replace_children(v); }
  virtual AnyV replace(V<ast_tensor> v)                  { return replace_children(v); }
  virtual AnyV replace(V<ast_tensor_square> v)           { return replace_children(v); }
  virtual AnyV replace(V<ast_identifier> v)              { return replace_children(v); }
  virtual AnyV replace(V<ast_int_const> v)               { return replace_children(v); }
  virtual AnyV replace(V<ast_string_const> v)            { return replace_children(v); }
  virtual AnyV replace(V<ast_bool_const> v)              { return replace_children(v); }
  virtual AnyV replace(V<ast_null_keyword> v)            { return replace_children(v); }
  virtual AnyV replace(V<ast_function_call> v)           { return replace_children(v); }
  virtual AnyV replace(V<ast_dot_method_call> v)         { return replace_children(v); }
  virtual AnyV replace(V<ast_underscore> v)              { return replace_children(v); }
  virtual AnyV replace(V<ast_unary_operator> v)          { return replace_children(v); }
  virtual AnyV replace(V<ast_binary_operator> v)         { return replace_children(v); }
  virtual AnyV replace(V<ast_ternary_operator> v)        { return replace_children(v); }
  virtual AnyV replace(V<ast_return_statement> v)        { return replace_children(v); }
  virtual AnyV replace(V<ast_sequence> v)                { return replace_children(v); }
  virtual AnyV replace(V<ast_repeat_statement> v)        { return replace_children(v); }
  virtual AnyV replace(V<ast_while_statement> v)         { return replace_children(v); }
  virtual AnyV replace(V<ast_do_while_statement> v)      { return replace_children(v); }
  virtual AnyV replace(V<ast_throw_statement> v)         { return replace_children(v); }
  virtual AnyV replace(V<ast_assert_statement> v)        { return replace_children(v); }
  virtual AnyV replace(V<ast_try_catch_statement> v)     { return replace_children(v); }
  virtual AnyV replace(V<ast_if_statement> v)            { return replace_children(v); }
  virtual AnyV replace(V<ast_local_var> v)               { return replace_children(v); }
  virtual AnyV replace(V<ast_local_vars_declaration> v)  { return replace_children(v); }
  virtual AnyV replace(V<ast_asm_body> v)                { return replace_children(v); }

  AnyV replace(AnyV v) final {
    switch (v->type) {
      case ast_empty:                           return replace(v->as<ast_empty>());
      case ast_parenthesized_expr:              return replace(v->as<ast_parenthesized_expr>());
      case ast_tensor:                          return replace(v->as<ast_tensor>());
      case ast_tensor_square:                   return replace(v->as<ast_tensor_square>());
      case ast_identifier:                      return replace(v->as<ast_identifier>());
      case ast_int_const:                       return replace(v->as<ast_int_const>());
      case ast_string_const:                    return replace(v->as<ast_string_const>());
      case ast_bool_const:                      return replace(v->as<ast_bool_const>());
      case ast_null_keyword:                    return replace(v->as<ast_null_keyword>());
      case ast_self_keyword:                    return replace(v->as<ast_self_keyword>());
      case ast_function_call:                   return replace(v->as<ast_function_call>());
      case ast_dot_method_call:                 return replace(v->as<ast_dot_method_call>());
      case ast_underscore:                      return replace(v->as<ast_underscore>());
      case ast_unary_operator:                  return replace(v->as<ast_unary_operator>());
      case ast_binary_operator:                 return replace(v->as<ast_binary_operator>());
      case ast_ternary_operator:                return replace(v->as<ast_ternary_operator>());
      case ast_return_statement:                return replace(v->as<ast_return_statement>());
      case ast_sequence:                        return replace(v->as<ast_sequence>());
      case ast_repeat_statement:                return replace(v->as<ast_repeat_statement>());
      case ast_while_statement:                 return replace(v->as<ast_while_statement>());
      case ast_do_while_statement:              return replace(v->as<ast_do_while_statement>());
      case ast_throw_statement:                 return replace(v->as<ast_throw_statement>());
      case ast_assert_statement:                return replace(v->as<ast_assert_statement>());
      case ast_try_catch_statement:             return replace(v->as<ast_try_catch_statement>());
      case ast_if_statement:                    return replace(v->as<ast_if_statement>());
      case ast_local_var:                       return replace(v->as<ast_local_var>());
      case ast_local_vars_declaration:          return replace(v->as<ast_local_vars_declaration>());
      case ast_asm_body:                        return replace(v->as<ast_asm_body>());
      default:
        throw UnexpectedASTNodeType(v, "ASTReplacerInFunctionBody::visit");
    }
  }

public:
  void start_replacing_in_function(V<ast_function_declaration> v) {
    replace(v->get_body());
  }
};

class ASTReplacerAllFunctionsInFile : public ASTReplacerInFunctionBody {
protected:
  using parent = ASTReplacerAllFunctionsInFile;

  virtual bool should_enter_function(V<ast_function_declaration> v) = 0;

public:
  void start_replacing_in_file(V<ast_tolk_file> v_file) {
    for (AnyV v : v_file->get_toplevel_declarations()) {
      if (auto v_function = v->try_as<ast_function_declaration>()) {
        if (should_enter_function(v_function)) {
          replace(v_function->get_body());
        }
      }
    }
  }
};

} // namespace tolk
