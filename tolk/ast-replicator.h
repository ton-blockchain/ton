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

namespace tolk {

class ASTReplicator {
protected:
  virtual AnyV clone(AnyV v) = 0;
  virtual AnyExprV clone(AnyExprV v) = 0;
  virtual TypePtr clone(TypePtr) = 0;

public:
  virtual ~ASTReplicator() = default;
};

class ASTReplicatorFunction : public ASTReplicator {
protected:
  using parent = ASTReplicatorFunction;

  std::vector<AnyV> clone(const std::vector<AnyV>& items) {
    std::vector<AnyV> result;
    result.reserve(items.size());
    for (AnyV item : items) {
      result.push_back(clone(item));
    }
    return result;
  }

  std::vector<AnyExprV> clone(const std::vector<AnyExprV>& items) {
    std::vector<AnyExprV> result;
    result.reserve(items.size());
    for (AnyExprV item : items) {
      result.push_back(clone(item));
    }
    return result;
  }

  // expressions

  virtual V<ast_empty_expression> clone(V<ast_empty_expression> v) {
    return createV<ast_empty_expression>(v->loc);
  }
  virtual V<ast_parenthesized_expression> clone(V<ast_parenthesized_expression> v) {
    return createV<ast_parenthesized_expression>(v->loc, clone(v->get_expr()));
  }
  virtual V<ast_tensor> clone(V<ast_tensor> v) {
    return createV<ast_tensor>(v->loc, clone(v->get_items()));
  }
  virtual V<ast_typed_tuple> clone(V<ast_typed_tuple> v) {
    return createV<ast_typed_tuple>(v->loc, clone(v->get_items()));
  }
  virtual V<ast_reference> clone(V<ast_reference> v) {
    return createV<ast_reference>(v->loc, clone(v->get_identifier()), v->has_instantiationTs() ? clone(v->get_instantiationTs()) : nullptr);
  }
  virtual V<ast_local_var_lhs> clone(V<ast_local_var_lhs> v) {
    return createV<ast_local_var_lhs>(v->loc, clone(v->get_identifier()), clone(v->declared_type), v->is_immutable, v->marked_as_redef);
  }
  virtual V<ast_local_vars_declaration> clone(V<ast_local_vars_declaration> v) {
    return createV<ast_local_vars_declaration>(v->loc, clone(v->get_expr()));
  }
  virtual V<ast_int_const> clone(V<ast_int_const> v) {
    return createV<ast_int_const>(v->loc, v->intval, v->orig_str);
  }
  virtual V<ast_string_const> clone(V<ast_string_const> v) {
    return createV<ast_string_const>(v->loc, v->str_val, v->modifier);
  }
  virtual V<ast_bool_const> clone(V<ast_bool_const> v) {
    return createV<ast_bool_const>(v->loc, v->bool_val);
  }
  virtual V<ast_null_keyword> clone(V<ast_null_keyword> v) {
    return createV<ast_null_keyword>(v->loc);
  }
  virtual V<ast_argument> clone(V<ast_argument> v) {
    return createV<ast_argument>(v->loc, clone(v->get_expr()), v->passed_as_mutate);
  }
  virtual V<ast_argument_list> clone(V<ast_argument_list> v) {
    return createV<ast_argument_list>(v->loc, clone(v->get_arguments()));
  }
  virtual V<ast_dot_access> clone(V<ast_dot_access> v) {
    return createV<ast_dot_access>(v->loc, clone(v->get_obj()), clone(v->get_identifier()), v->has_instantiationTs() ? clone(v->get_instantiationTs()) : nullptr);
  }
  virtual V<ast_function_call> clone(V<ast_function_call> v) {
    return createV<ast_function_call>(v->loc, clone(v->get_callee()), clone(v->get_arg_list()));
  }
  virtual V<ast_underscore> clone(V<ast_underscore> v) {
    return createV<ast_underscore>(v->loc);
  }
  virtual V<ast_assign> clone(V<ast_assign> v) {
    return createV<ast_assign>(v->loc, clone(v->get_lhs()), clone(v->get_rhs()));
  }
  virtual V<ast_set_assign> clone(V<ast_set_assign> v) {
    return createV<ast_set_assign>(v->loc, v->operator_name, v->tok, clone(v->get_lhs()), clone(v->get_rhs()));
  }
  virtual V<ast_unary_operator> clone(V<ast_unary_operator> v) {
    return createV<ast_unary_operator>(v->loc, v->operator_name, v->tok, clone(v->get_rhs()));
  }
  virtual V<ast_binary_operator> clone(V<ast_binary_operator> v) {
    return createV<ast_binary_operator>(v->loc, v->operator_name, v->tok, clone(v->get_lhs()), clone(v->get_rhs()));
  }
  virtual V<ast_ternary_operator> clone(V<ast_ternary_operator> v) {
    return createV<ast_ternary_operator>(v->loc, clone(v->get_cond()), clone(v->get_when_true()), clone(v->get_when_false()));
  }
  virtual V<ast_cast_as_operator> clone(V<ast_cast_as_operator> v) {
    return createV<ast_cast_as_operator>(v->loc, clone(v->get_expr()), clone(v->cast_to_type));
  }

  // statements

  virtual V<ast_empty_statement> clone(V<ast_empty_statement> v) {
    return createV<ast_empty_statement>(v->loc);
  }
  virtual V<ast_sequence> clone(V<ast_sequence> v) {
    return createV<ast_sequence>(v->loc, v->loc_end, clone(v->get_items()));
  }
  virtual V<ast_return_statement> clone(V<ast_return_statement> v) {
    return createV<ast_return_statement>(v->loc, clone(v->get_return_value()));
  }
  virtual V<ast_if_statement> clone(V<ast_if_statement> v) {
    return createV<ast_if_statement>(v->loc, v->is_ifnot, clone(v->get_cond()), clone(v->get_if_body()), clone(v->get_else_body()));
  }
  virtual V<ast_repeat_statement> clone(V<ast_repeat_statement> v) {
    return createV<ast_repeat_statement>(v->loc, clone(v->get_cond()), clone(v->get_body()));
  }
  virtual V<ast_while_statement> clone(V<ast_while_statement> v) {
    return createV<ast_while_statement>(v->loc, clone(v->get_cond()), clone(v->get_body()));
  }
  virtual V<ast_do_while_statement> clone(V<ast_do_while_statement> v) {
    return createV<ast_do_while_statement>(v->loc, clone(v->get_body()), clone(v->get_cond()));
  }
  virtual V<ast_throw_statement> clone(V<ast_throw_statement> v) {
    return createV<ast_throw_statement>(v->loc, clone(v->get_thrown_code()), clone(v->get_thrown_arg()));
  }
  virtual V<ast_assert_statement> clone(V<ast_assert_statement> v) {
    return createV<ast_assert_statement>(v->loc, clone(v->get_cond()), clone(v->get_thrown_code()));
  }
  virtual V<ast_try_catch_statement> clone(V<ast_try_catch_statement> v) {
    return createV<ast_try_catch_statement>(v->loc, clone(v->get_try_body()), clone(v->get_catch_expr()), clone(v->get_catch_body()));
  }
  virtual V<ast_asm_body> clone(V<ast_asm_body> v) {
    return createV<ast_asm_body>(v->loc, v->arg_order, v->ret_order, clone(v->get_asm_commands()));
  }

  // other

  virtual V<ast_identifier> clone(V<ast_identifier> v) {
    return createV<ast_identifier>(v->loc, v->name);
  }
  virtual V<ast_instantiationT_item> clone(V<ast_instantiationT_item> v) {
    return createV<ast_instantiationT_item>(v->loc, clone(v->substituted_type));
  }
  virtual V<ast_instantiationT_list> clone(V<ast_instantiationT_list> v) {
    return createV<ast_instantiationT_list>(v->loc, clone(v->get_items()));
  }
  virtual V<ast_parameter> clone(V<ast_parameter> v) {
    return createV<ast_parameter>(v->loc, v->param_name, clone(v->declared_type), v->declared_as_mutate);
  }
  virtual V<ast_parameter_list> clone(V<ast_parameter_list> v) {
    return createV<ast_parameter_list>(v->loc, clone(v->get_params()));
  }

  AnyExprV clone(AnyExprV v) final {
    switch (v->type) {
      case ast_empty_expression:                return clone(v->as<ast_empty_expression>());
      case ast_parenthesized_expression:        return clone(v->as<ast_parenthesized_expression>());
      case ast_tensor:                          return clone(v->as<ast_tensor>());
      case ast_typed_tuple:                     return clone(v->as<ast_typed_tuple>());
      case ast_reference:                       return clone(v->as<ast_reference>());
      case ast_local_var_lhs:                   return clone(v->as<ast_local_var_lhs>());
      case ast_local_vars_declaration:          return clone(v->as<ast_local_vars_declaration>());
      case ast_int_const:                       return clone(v->as<ast_int_const>());
      case ast_string_const:                    return clone(v->as<ast_string_const>());
      case ast_bool_const:                      return clone(v->as<ast_bool_const>());
      case ast_null_keyword:                    return clone(v->as<ast_null_keyword>());
      case ast_argument:                        return clone(v->as<ast_argument>());
      case ast_argument_list:                   return clone(v->as<ast_argument_list>());
      case ast_dot_access:                      return clone(v->as<ast_dot_access>());
      case ast_function_call:                   return clone(v->as<ast_function_call>());
      case ast_underscore:                      return clone(v->as<ast_underscore>());
      case ast_assign:                          return clone(v->as<ast_assign>());
      case ast_set_assign:                      return clone(v->as<ast_set_assign>());
      case ast_unary_operator:                  return clone(v->as<ast_unary_operator>());
      case ast_binary_operator:                 return clone(v->as<ast_binary_operator>());
      case ast_ternary_operator:                return clone(v->as<ast_ternary_operator>());
      case ast_cast_as_operator:                return clone(v->as<ast_cast_as_operator>());
      default:
        throw UnexpectedASTNodeType(v, "ASTReplicatorFunction::clone");
    }
  }

  AnyV clone(AnyV v) final {
    switch (v->type) {
      case ast_empty_statement:                 return clone(v->as<ast_empty_statement>());
      case ast_sequence:                        return clone(v->as<ast_sequence>());
      case ast_return_statement:                return clone(v->as<ast_return_statement>());
      case ast_if_statement:                    return clone(v->as<ast_if_statement>());
      case ast_repeat_statement:                return clone(v->as<ast_repeat_statement>());
      case ast_while_statement:                 return clone(v->as<ast_while_statement>());
      case ast_do_while_statement:              return clone(v->as<ast_do_while_statement>());
      case ast_throw_statement:                 return clone(v->as<ast_throw_statement>());
      case ast_assert_statement:                return clone(v->as<ast_assert_statement>());
      case ast_try_catch_statement:             return clone(v->as<ast_try_catch_statement>());
      case ast_asm_body:                        return clone(v->as<ast_asm_body>());
      // other AST nodes that can be children of ast nodes of function body
      case ast_identifier:                      return clone(v->as<ast_identifier>());
      case ast_instantiationT_item:             return clone(v->as<ast_instantiationT_item>());
      case ast_instantiationT_list:             return clone(v->as<ast_instantiationT_list>());
      case ast_parameter:                       return clone(v->as<ast_parameter>());
      case ast_parameter_list:                  return clone(v->as<ast_parameter_list>());

      default: {
        // be very careful, don't forget to handle all statements/other (not expressions) above!
        AnyExprV as_expr = reinterpret_cast<const ASTNodeExpressionBase*>(v);
        return clone(as_expr);
      }
    }
  }

  TypePtr clone(TypePtr t) override {
    return t;
  }

 public:
  virtual V<ast_function_declaration> clone_function_body(V<ast_function_declaration> v_function) {
    return createV<ast_function_declaration>(
      v_function->loc,
      clone(v_function->get_identifier()),
      clone(v_function->get_param_list()),
      clone(v_function->get_body()->as<ast_sequence>()),
      clone(v_function->declared_return_type),
      v_function->genericsT_list,
      v_function->method_id,
      v_function->flags
    );
  }
};

} // namespace tolk
