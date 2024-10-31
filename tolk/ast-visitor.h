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
 *   Since a vertex in general doesn't store a vector of children, iterating is possible only for concrete node_type.
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
  GNU_ATTRIBUTE_ALWAYS_INLINE static void visit_children(const ASTNodeLeaf* v) {
    static_cast<void>(v);
  }

  GNU_ATTRIBUTE_ALWAYS_INLINE void visit_children(const ASTNodeUnary* v) {
    visit(v->child);
  }

  GNU_ATTRIBUTE_ALWAYS_INLINE void visit_children(const ASTNodeBinary* v) {
    visit(v->lhs);
    visit(v->rhs);
  }

  GNU_ATTRIBUTE_ALWAYS_INLINE void visit_children(const ASTNodeVararg* v) {
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

  virtual void visit(V<ast_empty> v)                     { return visit_children(v); }
  virtual void visit(V<ast_identifier> v)                { return visit_children(v); }
  virtual void visit(V<ast_int_const> v)                 { return visit_children(v); }
  virtual void visit(V<ast_string_const> v)              { return visit_children(v); }
  virtual void visit(V<ast_bool_const> v)                { return visit_children(v); }
  virtual void visit(V<ast_nil_tuple> v)                 { return visit_children(v); }
  virtual void visit(V<ast_function_call> v)             { return visit_children(v); }
  virtual void visit(V<ast_parenthesized_expr> v)        { return visit_children(v); }
  virtual void visit(V<ast_underscore> v)                { return visit_children(v); }
  virtual void visit(V<ast_type_expression> v)           { return visit_children(v); }
  virtual void visit(V<ast_variable_declaration> v)      { return visit_children(v); }
  virtual void visit(V<ast_tensor> v)                    { return visit_children(v); }
  virtual void visit(V<ast_tensor_square> v)             { return visit_children(v); }
  virtual void visit(V<ast_dot_tilde_call> v)            { return visit_children(v); }
  virtual void visit(V<ast_unary_operator> v)            { return visit_children(v); }
  virtual void visit(V<ast_binary_operator> v)           { return visit_children(v); }
  virtual void visit(V<ast_ternary_operator> v)          { return visit_children(v); }
  virtual void visit(V<ast_return_statement> v)          { return visit_children(v); }
  virtual void visit(V<ast_sequence> v)                  { return visit_children(v); }
  virtual void visit(V<ast_repeat_statement> v)          { return visit_children(v); }
  virtual void visit(V<ast_while_statement> v)           { return visit_children(v); }
  virtual void visit(V<ast_do_until_statement> v)        { return visit_children(v); }
  virtual void visit(V<ast_try_catch_statement> v)       { return visit_children(v); }
  virtual void visit(V<ast_if_statement> v)              { return visit_children(v); }
  virtual void visit(V<ast_asm_body> v)                  { return visit_children(v); }

  void visit(AnyV v) final {
    switch (v->type) {
      case ast_empty:                           return visit(v->as<ast_empty>());
      case ast_identifier:                      return visit(v->as<ast_identifier>());
      case ast_int_const:                       return visit(v->as<ast_int_const>());
      case ast_string_const:                    return visit(v->as<ast_string_const>());
      case ast_bool_const:                      return visit(v->as<ast_bool_const>());
      case ast_nil_tuple:                       return visit(v->as<ast_nil_tuple>());
      case ast_function_call:                   return visit(v->as<ast_function_call>());
      case ast_parenthesized_expr:              return visit(v->as<ast_parenthesized_expr>());
      case ast_underscore:                      return visit(v->as<ast_underscore>());
      case ast_type_expression:                 return visit(v->as<ast_type_expression>());
      case ast_variable_declaration:            return visit(v->as<ast_variable_declaration>());
      case ast_tensor:                          return visit(v->as<ast_tensor>());
      case ast_tensor_square:                   return visit(v->as<ast_tensor_square>());
      case ast_dot_tilde_call:                  return visit(v->as<ast_dot_tilde_call>());
      case ast_unary_operator:                  return visit(v->as<ast_unary_operator>());
      case ast_binary_operator:                 return visit(v->as<ast_binary_operator>());
      case ast_ternary_operator:                return visit(v->as<ast_ternary_operator>());
      case ast_return_statement:                return visit(v->as<ast_return_statement>());
      case ast_sequence:                        return visit(v->as<ast_sequence>());
      case ast_repeat_statement:                return visit(v->as<ast_repeat_statement>());
      case ast_while_statement:                 return visit(v->as<ast_while_statement>());
      case ast_do_until_statement:              return visit(v->as<ast_do_until_statement>());
      case ast_try_catch_statement:             return visit(v->as<ast_try_catch_statement>());
      case ast_if_statement:                    return visit(v->as<ast_if_statement>());
      case ast_asm_body:                        return visit(v->as<ast_asm_body>());
      default:
        throw UnexpectedASTNodeType(v, "ASTVisitorFunctionBody::visit");
    }
  }

public:
  void start_visiting_function(V<ast_function_declaration> v_function) {
    visit(v_function->get_body());
  }
};

class ASTVisitorAllFunctionsInFile : public ASTVisitorFunctionBody {
protected:
  using parent = ASTVisitorAllFunctionsInFile;

  virtual bool should_enter_function(V<ast_function_declaration> v) = 0;

public:
  void start_visiting_file(V<ast_tolk_file> v_file) {
    for (AnyV v : v_file->get_toplevel_declarations()) {
      if (auto v_func = v->try_as<ast_function_declaration>()) {
        if (should_enter_function(v_func)) {
          visit(v_func->get_body());
        }
      }
    }
  }
};

class ASTVisitorToplevelDeclarations : public ASTVisitor {
protected:
  using parent = ASTVisitorToplevelDeclarations;

  virtual void on_pragma_no_arg(V<ast_pragma_no_arg> v) = 0;
  virtual void on_pragma_version(V<ast_pragma_version> v) = 0;
  virtual void on_include_statement(V<ast_include_statement> v) = 0;
  virtual void on_constant_declaration(V<ast_constant_declaration> v) = 0;
  virtual void on_global_var_declaration(V<ast_global_var_declaration> v) = 0;
  virtual void on_function_declaration(V<ast_function_declaration> v) = 0;

  void visit(AnyV v) final {
   switch (v->type) {
     case ast_pragma_no_arg:
       on_pragma_no_arg(v->as<ast_pragma_no_arg>());
       break;
     case ast_pragma_version:
       on_pragma_version(v->as<ast_pragma_version>());
       break;
     case ast_include_statement:
       on_include_statement(v->as<ast_include_statement>());
       break;
     case ast_constant_declaration_list:
       for (const auto& v_decl : v->as<ast_constant_declaration_list>()->get_declarations()) {
         on_constant_declaration(v_decl->as<ast_constant_declaration>());
       }
       break;
     case ast_global_var_declaration_list:
       for (const auto& v_decl : v->as<ast_global_var_declaration_list>()->get_declarations()) {
         on_global_var_declaration(v_decl->as<ast_global_var_declaration>());
       }
       break;
     case ast_function_declaration:
       on_function_declaration(v->as<ast_function_declaration>());
       break;
     default:
       throw UnexpectedASTNodeType(v, "ASTVisitorToplevelDeclarations::visit");
    }
  }

public:
  void start_visiting_file(V<ast_tolk_file> v_file) {
    for (AnyV v : v_file->get_toplevel_declarations()) {
      visit(v);
    }
  }
};

} // namespace tolk
