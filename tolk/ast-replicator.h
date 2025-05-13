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

class ASTReplicator final {
  static std::vector<AnyV> clone(const std::vector<AnyV>& items) {
    std::vector<AnyV> result;
    result.reserve(items.size());
    for (AnyV item : items) {
      result.push_back(clone(item));
    }
    return result;
  }

  static std::vector<AnyExprV> clone(const std::vector<AnyExprV>& items) {
    std::vector<AnyExprV> result;
    result.reserve(items.size());
    for (AnyExprV item : items) {
      result.push_back(clone(item));
    }
    return result;
  }

  static std::vector<AnyTypeV> clone(const std::vector<AnyTypeV>& items) {
    std::vector<AnyTypeV> result;
    result.reserve(items.size());
    for (AnyTypeV item : items) {
      result.push_back(clone(item));
    }
    return result;
  }

  // types

  static V<ast_type_leaf_text> clone(V<ast_type_leaf_text> v) {
    return createV<ast_type_leaf_text>(v->loc, v->text);
  }
  static V<ast_type_question_nullable> clone(V<ast_type_question_nullable> v) {
    return createV<ast_type_question_nullable>(v->loc, clone(v->get_inner()));
  }
  static V<ast_type_parenthesis_tensor> clone(V<ast_type_parenthesis_tensor> v) {
    return createV<ast_type_parenthesis_tensor>(v->loc, clone(v->get_items()));
  }
  static V<ast_type_bracket_tuple> clone(V<ast_type_bracket_tuple> v) {
    return createV<ast_type_bracket_tuple>(v->loc, clone(v->get_items()));
  }
  static V<ast_type_arrow_callable> clone(V<ast_type_arrow_callable> v) {
    return createV<ast_type_arrow_callable>(v->loc, clone(v->get_params_and_return()));
  }
  static V<ast_type_vertical_bar_union> clone(V<ast_type_vertical_bar_union> v) {
    return createV<ast_type_vertical_bar_union>(v->loc, clone(v->get_variants()));
  }
  static V<ast_type_triangle_args> clone(V<ast_type_triangle_args> v) {
    return createV<ast_type_triangle_args>(v->loc, clone(v->get_inner_and_args()));
  }

  // expressions

  static V<ast_empty_expression> clone(V<ast_empty_expression> v) {
    return createV<ast_empty_expression>(v->loc);
  }
  static V<ast_parenthesized_expression> clone(V<ast_parenthesized_expression> v) {
    return createV<ast_parenthesized_expression>(v->loc, clone(v->get_expr()));
  }
  static V<ast_braced_expression> clone(V<ast_braced_expression> v) {
    return createV<ast_braced_expression>(v->loc, clone(v->get_block_statement()));
  }
  static V<ast_artificial_aux_vertex> clone(V<ast_artificial_aux_vertex> v) {
    return createV<ast_artificial_aux_vertex>(v->loc, clone(v->get_wrapped_expr()), v->aux_data, v->inferred_type);
  }
  static V<ast_tensor> clone(V<ast_tensor> v) {
    return createV<ast_tensor>(v->loc, clone(v->get_items()));
  }
  static V<ast_bracket_tuple> clone(V<ast_bracket_tuple> v) {
    return createV<ast_bracket_tuple>(v->loc, clone(v->get_items()));
  }
  static V<ast_reference> clone(V<ast_reference> v) {
    return createV<ast_reference>(v->loc, clone(v->get_identifier()), v->has_instantiationTs() ? clone(v->get_instantiationTs()) : nullptr);
  }
  static V<ast_local_var_lhs> clone(V<ast_local_var_lhs> v) {
    return createV<ast_local_var_lhs>(v->loc, clone(v->get_identifier()), clone(v->type_node), v->is_immutable, v->marked_as_redef);
  }
  static V<ast_local_vars_declaration> clone(V<ast_local_vars_declaration> v) {
    return createV<ast_local_vars_declaration>(v->loc, clone(v->get_expr()));
  }
  static V<ast_int_const> clone(V<ast_int_const> v) {
    return createV<ast_int_const>(v->loc, v->intval, v->orig_str);
  }
  static V<ast_string_const> clone(V<ast_string_const> v) {
    return createV<ast_string_const>(v->loc, v->str_val);
  }
  static V<ast_bool_const> clone(V<ast_bool_const> v) {
    return createV<ast_bool_const>(v->loc, v->bool_val);
  }
  static V<ast_null_keyword> clone(V<ast_null_keyword> v) {
    return createV<ast_null_keyword>(v->loc);
  }
  static V<ast_argument> clone(V<ast_argument> v) {
    return createV<ast_argument>(v->loc, clone(v->get_expr()), v->passed_as_mutate);
  }
  static V<ast_argument_list> clone(V<ast_argument_list> v) {
    return createV<ast_argument_list>(v->loc, clone(v->get_arguments()));
  }
  static V<ast_dot_access> clone(V<ast_dot_access> v) {
    return createV<ast_dot_access>(v->loc, clone(v->get_obj()), clone(v->get_identifier()), v->has_instantiationTs() ? clone(v->get_instantiationTs()) : nullptr);
  }
  static V<ast_function_call> clone(V<ast_function_call> v) {
    return createV<ast_function_call>(v->loc, clone(v->get_callee()), clone(v->get_arg_list()));
  }
  static V<ast_underscore> clone(V<ast_underscore> v) {
    return createV<ast_underscore>(v->loc);
  }
  static V<ast_assign> clone(V<ast_assign> v) {
    return createV<ast_assign>(v->loc, clone(v->get_lhs()), clone(v->get_rhs()));
  }
  static V<ast_set_assign> clone(V<ast_set_assign> v) {
    return createV<ast_set_assign>(v->loc, v->operator_name, v->tok, clone(v->get_lhs()), clone(v->get_rhs()));
  }
  static V<ast_unary_operator> clone(V<ast_unary_operator> v) {
    return createV<ast_unary_operator>(v->loc, v->operator_name, v->tok, clone(v->get_rhs()));
  }
  static V<ast_binary_operator> clone(V<ast_binary_operator> v) {
    return createV<ast_binary_operator>(v->loc, v->operator_name, v->tok, clone(v->get_lhs()), clone(v->get_rhs()));
  }
  static V<ast_ternary_operator> clone(V<ast_ternary_operator> v) {
    return createV<ast_ternary_operator>(v->loc, clone(v->get_cond()), clone(v->get_when_true()), clone(v->get_when_false()));
  }
  static V<ast_cast_as_operator> clone(V<ast_cast_as_operator> v) {
    return createV<ast_cast_as_operator>(v->loc, clone(v->get_expr()), clone(v->type_node));
  }
  static V<ast_is_type_operator> clone(V<ast_is_type_operator> v) {
    return createV<ast_is_type_operator>(v->loc, clone(v->get_expr()), clone(v->type_node), v->is_negated);
  }
  static V<ast_not_null_operator> clone(V<ast_not_null_operator> v) {
    return createV<ast_not_null_operator>(v->loc, clone(v->get_expr()));
  }
  static V<ast_match_expression> clone(V<ast_match_expression> v) {
    return createV<ast_match_expression>(v->loc, clone(v->get_all_children()));
  }
  static V<ast_match_arm> clone(V<ast_match_arm> v) {
    return createV<ast_match_arm>(v->loc, v->pattern_kind, clone(v->pattern_type_node), clone(v->get_pattern_expr()), clone(v->get_body()));
  }
  static V<ast_object_field> clone(V<ast_object_field> v) {
    return createV<ast_object_field>(v->loc, clone(v->get_field_identifier()), clone(v->get_init_val()));
  }
  static V<ast_object_body> clone(V<ast_object_body> v) {
    return createV<ast_object_body>(v->loc, clone(v->get_all_fields()));
  }
  static V<ast_object_literal> clone(V<ast_object_literal> v) {
    return createV<ast_object_literal>(v->loc, clone(v->type_node), clone(v->get_body()));
  }

  // statements

  static V<ast_empty_statement> clone(V<ast_empty_statement> v) {
    return createV<ast_empty_statement>(v->loc);
  }
  static V<ast_block_statement> clone(V<ast_block_statement> v) {
    return createV<ast_block_statement>(v->loc, v->loc_end, clone(v->get_items()));
  }
  static V<ast_return_statement> clone(V<ast_return_statement> v) {
    return createV<ast_return_statement>(v->loc, clone(v->get_return_value()));
  }
  static V<ast_if_statement> clone(V<ast_if_statement> v) {
    return createV<ast_if_statement>(v->loc, v->is_ifnot, clone(v->get_cond()), clone(v->get_if_body()), clone(v->get_else_body()));
  }
  static V<ast_repeat_statement> clone(V<ast_repeat_statement> v) {
    return createV<ast_repeat_statement>(v->loc, clone(v->get_cond()), clone(v->get_body()));
  }
  static V<ast_while_statement> clone(V<ast_while_statement> v) {
    return createV<ast_while_statement>(v->loc, clone(v->get_cond()), clone(v->get_body()));
  }
  static V<ast_do_while_statement> clone(V<ast_do_while_statement> v) {
    return createV<ast_do_while_statement>(v->loc, clone(v->get_body()), clone(v->get_cond()));
  }
  static V<ast_throw_statement> clone(V<ast_throw_statement> v) {
    return createV<ast_throw_statement>(v->loc, clone(v->get_thrown_code()), clone(v->get_thrown_arg()));
  }
  static V<ast_assert_statement> clone(V<ast_assert_statement> v) {
    return createV<ast_assert_statement>(v->loc, clone(v->get_cond()), clone(v->get_thrown_code()));
  }
  static V<ast_try_catch_statement> clone(V<ast_try_catch_statement> v) {
    return createV<ast_try_catch_statement>(v->loc, clone(v->get_try_body()), clone(v->get_catch_expr()), clone(v->get_catch_body()));
  }
  static V<ast_asm_body> clone(V<ast_asm_body> v) {
    return createV<ast_asm_body>(v->loc, v->arg_order, v->ret_order, clone(v->get_asm_commands()));
  }

  // other (common)

  static V<ast_identifier> clone(V<ast_identifier> v) {
    return createV<ast_identifier>(v->loc, v->name);
  }
  static V<ast_genericsT_item> clone(V<ast_genericsT_item> v) {
    return createV<ast_genericsT_item>(v->loc, v->nameT, clone(v->default_type_node));
  }
  static V<ast_genericsT_list> clone(V<ast_genericsT_list> v) {
    return createV<ast_genericsT_list>(v->loc, clone(v->get_items()));
  }
  static V<ast_instantiationT_item> clone(V<ast_instantiationT_item> v) {
    return createV<ast_instantiationT_item>(v->loc, clone(v->type_node));
  }
  static V<ast_instantiationT_list> clone(V<ast_instantiationT_list> v) {
    return createV<ast_instantiationT_list>(v->loc, clone(v->get_items()));
  }
  static V<ast_parameter> clone(V<ast_parameter> v) {
    return createV<ast_parameter>(v->loc, v->param_name, clone(v->type_node), v->declared_as_mutate);
  }
  static V<ast_parameter_list> clone(V<ast_parameter_list> v) {
    return createV<ast_parameter_list>(v->loc, clone(v->get_params()));
  }
  static V<ast_struct_field> clone(V<ast_struct_field> v) {
    return createV<ast_struct_field>(v->loc, clone(v->get_identifier()), clone(v->get_default_value()), clone(v->type_node));
  }
  static V<ast_struct_body> clone(V<ast_struct_body> v) {
    return createV<ast_struct_body>(v->loc, clone(v->get_all_fields()));
  }


  static AnyV clone(AnyV v) {
    switch (v->kind) {
      case ast_empty_statement:                 return clone(v->as<ast_empty_statement>());
      case ast_block_statement:                 return clone(v->as<ast_block_statement>());
      case ast_return_statement:                return clone(v->as<ast_return_statement>());
      case ast_if_statement:                    return clone(v->as<ast_if_statement>());
      case ast_repeat_statement:                return clone(v->as<ast_repeat_statement>());
      case ast_while_statement:                 return clone(v->as<ast_while_statement>());
      case ast_do_while_statement:              return clone(v->as<ast_do_while_statement>());
      case ast_throw_statement:                 return clone(v->as<ast_throw_statement>());
      case ast_assert_statement:                return clone(v->as<ast_assert_statement>());
      case ast_try_catch_statement:             return clone(v->as<ast_try_catch_statement>());
      case ast_asm_body:                        return clone(v->as<ast_asm_body>());
      // other AST nodes that can be children of ast nodes of function/struct body
      case ast_identifier:                      return clone(v->as<ast_identifier>());
      case ast_genericsT_item:                  return clone(v->as<ast_genericsT_item>());
      case ast_genericsT_list:                  return clone(v->as<ast_genericsT_list>());
      case ast_instantiationT_item:             return clone(v->as<ast_instantiationT_item>());
      case ast_instantiationT_list:             return clone(v->as<ast_instantiationT_list>());
      case ast_parameter:                       return clone(v->as<ast_parameter>());
      case ast_parameter_list:                  return clone(v->as<ast_parameter_list>());
      case ast_struct_field:                    return clone(v->as<ast_struct_field>());
      case ast_struct_body:                     return clone(v->as<ast_struct_body>());

      default: {
        // be very careful, don't forget to handle all statements/other (not expressions) above!
        AnyExprV as_expr = reinterpret_cast<const ASTNodeExpressionBase*>(v);
        return clone(as_expr);
      }
    }
  }

  static AnyExprV clone(AnyExprV v) {
    switch (v->kind) {
      case ast_empty_expression:                return clone(v->as<ast_empty_expression>());
      case ast_parenthesized_expression:        return clone(v->as<ast_parenthesized_expression>());
      case ast_braced_expression:               return clone(v->as<ast_braced_expression>());
      case ast_artificial_aux_vertex:           return clone(v->as<ast_artificial_aux_vertex>());
      case ast_tensor:                          return clone(v->as<ast_tensor>());
      case ast_bracket_tuple:                   return clone(v->as<ast_bracket_tuple>());
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
      case ast_is_type_operator:                return clone(v->as<ast_is_type_operator>());
      case ast_not_null_operator:               return clone(v->as<ast_not_null_operator>());
      case ast_match_expression:                return clone(v->as<ast_match_expression>());
      case ast_match_arm:                       return clone(v->as<ast_match_arm>());
      case ast_object_field:                    return clone(v->as<ast_object_field>());
      case ast_object_body:                     return clone(v->as<ast_object_body>());
      case ast_object_literal:                  return clone(v->as<ast_object_literal>());
      default:
        throw UnexpectedASTNodeKind(v, "ASTReplicatorFunction::clone(AnyExprV)");
    }
  }

  static AnyTypeV clone(AnyTypeV v) {
    if (v == nullptr) {
      return nullptr;
    }
    switch (v->kind) {
      case ast_type_leaf_text:                  return clone(v->as<ast_type_leaf_text>());
      case ast_type_question_nullable:          return clone(v->as<ast_type_question_nullable>());
      case ast_type_parenthesis_tensor:         return clone(v->as<ast_type_parenthesis_tensor>());
      case ast_type_bracket_tuple:              return clone(v->as<ast_type_bracket_tuple>());
      case ast_type_arrow_callable:             return clone(v->as<ast_type_arrow_callable>());
      case ast_type_vertical_bar_union:         return clone(v->as<ast_type_vertical_bar_union>());
      case ast_type_triangle_args:              return clone(v->as<ast_type_triangle_args>());
      default:
        throw UnexpectedASTNodeKind(v, "ASTReplicator::clone(AnyTypeV)");
    }
  }

public:
  // the cloned function becomes a deep copy, all AST nodes are copied, no previous pointers left
  static V<ast_function_declaration> clone_function_ast(V<ast_function_declaration> v_orig) {
    return createV<ast_function_declaration>(
      v_orig->loc,
      clone(v_orig->get_identifier()),
      clone(v_orig->get_param_list()),
      clone(v_orig->get_body()),
      clone(v_orig->receiver_type_node),
      clone(v_orig->return_type_node),
      v_orig->genericsT_list ? clone(v_orig->genericsT_list) : nullptr,
      v_orig->tvm_method_id,
      v_orig->flags
    );
  }

  // the cloned struct becomes a deep copy, all AST nodes are copied, no previous pointers left
  static V<ast_struct_declaration> clone_struct_ast(V<ast_struct_declaration> v_orig, V<ast_identifier> new_name_ident) {
    return createV<ast_struct_declaration>(
      v_orig->loc,
      new_name_ident,
      clone(v_orig->genericsT_list),
      clone(v_orig->get_struct_body())
    );
  }

  // the cloned type alias becomes a deep copy, all AST nodes are copied, no previous pointers left
  static V<ast_type_alias_declaration> clone_type_alias_ast(V<ast_type_alias_declaration> v_orig, V<ast_identifier> new_name_ident) {
    return createV<ast_type_alias_declaration>(
      v_orig->loc,
      new_name_ident,
      clone(v_orig->genericsT_list),
      clone(v_orig->underlying_type_node)
    );
  }
};

} // namespace tolk
