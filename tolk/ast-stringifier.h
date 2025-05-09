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

#ifdef TOLK_DEBUG

#include "ast.h"
#include "ast-visitor.h"
#include "type-system.h"
#include <sstream>

/*
 *   ASTStringifier is used to print out the whole vertex tree in a human-readable format.
 *   To stringify any vertex, call v->debug_print(), which uses this class.
 */

namespace tolk {

class ASTStringifier final : public ASTVisitor {
  constexpr static std::pair<ASTNodeKind, const char*> name_pairs[] = {
    {ast_identifier, "ast_identifier"},
    // types
    {ast_type_leaf_text, "ast_type_leaf_text"},
    {ast_type_question_nullable, "ast_type_question_nullable"},
    {ast_type_parenthesis_tensor, "ast_type_parenthesis_tensor"},
    {ast_type_bracket_tuple, "ast_type_bracket_tuple"},
    {ast_type_arrow_callable, "ast_type_arrow_callable"},
    {ast_type_vertical_bar_union, "ast_type_vertical_bar_union"},
    {ast_type_triangle_args, "ast_type_triangle_args"},
    // expressions
    {ast_empty_expression, "ast_empty_expression"},
    {ast_parenthesized_expression, "ast_parenthesized_expression"},
    {ast_braced_expression, "ast_braced_expression"},
    {ast_artificial_aux_vertex, "ast_artificial_aux_vertex"},
    {ast_tensor, "ast_tensor"},
    {ast_bracket_tuple, "ast_bracket_tuple"},
    {ast_reference, "ast_reference"},
    {ast_local_var_lhs, "ast_local_var_lhs"},
    {ast_local_vars_declaration, "ast_local_vars_declaration"},
    {ast_int_const, "ast_int_const"},
    {ast_string_const, "ast_string_const"},
    {ast_bool_const, "ast_bool_const"},
    {ast_null_keyword, "ast_null_keyword"},
    {ast_argument, "ast_argument"},
    {ast_argument_list, "ast_argument_list"},
    {ast_dot_access, "ast_dot_access"},
    {ast_function_call, "ast_function_call"},
    {ast_underscore, "ast_underscore"},
    {ast_assign, "ast_assign"},
    {ast_set_assign, "ast_set_assign"},
    {ast_unary_operator, "ast_unary_operator"},
    {ast_binary_operator, "ast_binary_operator"},
    {ast_ternary_operator, "ast_ternary_operator"},
    {ast_cast_as_operator, "ast_cast_as_operator"},
    {ast_is_type_operator, "ast_is_type_operator"},
    {ast_not_null_operator, "ast_not_null_operator"},
    {ast_match_expression, "ast_match_expression"},
    {ast_match_arm, "ast_match_arm"},
    {ast_object_field, "ast_object_field"},
    {ast_object_body, "ast_object_body"},
    {ast_object_literal, "ast_object_literal"},
    // statements
    {ast_empty_statement, "ast_empty_statement"},
    {ast_block_statement, "ast_block_statement"},
    {ast_return_statement, "ast_return_statement"},
    {ast_if_statement, "ast_if_statement"},
    {ast_repeat_statement, "ast_repeat_statement"},
    {ast_while_statement, "ast_while_statement"},
    {ast_do_while_statement, "ast_do_while_statement"},
    {ast_throw_statement, "ast_throw_statement"},
    {ast_assert_statement, "ast_assert_statement"},
    {ast_try_catch_statement, "ast_try_catch_statement"},
    {ast_asm_body, "ast_asm_body"},
    // other
    {ast_genericsT_item, "ast_genericsT_item"},
    {ast_genericsT_list, "ast_genericsT_list"},
    {ast_instantiationT_item, "ast_instantiationT_item"},
    {ast_instantiationT_list, "ast_instantiationT_list"},
    {ast_parameter, "ast_parameter"},
    {ast_parameter_list, "ast_parameter_list"},
    {ast_annotation, "ast_annotation"},
    {ast_function_declaration, "ast_function_declaration"},
    {ast_global_var_declaration, "ast_global_var_declaration"},
    {ast_constant_declaration, "ast_constant_declaration"},
    {ast_type_alias_declaration, "ast_type_alias_declaration"},
    {ast_struct_field, "ast_struct_field"},
    {ast_struct_body, "ast_struct_body"},
    {ast_struct_declaration, "ast_struct_declaration"},
    {ast_tolk_required_version, "ast_tolk_required_version"},
    {ast_import_directive, "ast_import_directive"},
    {ast_tolk_file, "ast_tolk_file"},
  };

  static_assert(std::size(name_pairs) == ast_tolk_file + 1, "name_pairs needs to be updated");

  constexpr static std::pair<AnnotationKind, const char*> annotation_kinds[] = {
    {AnnotationKind::inline_simple, "@inline"},
    {AnnotationKind::inline_ref, "@inline_ref"},
    {AnnotationKind::method_id, "@method_id"},
    {AnnotationKind::pure, "@pure"},
    {AnnotationKind::deprecated, "@deprecated"},
  };

  static_assert(std::size(annotation_kinds) == static_cast<size_t>(AnnotationKind::unknown), "annotation_kinds needs to be updated");

  template<ASTNodeKind node_kind>
  constexpr static const char* ast_node_kind_to_string() {
    return name_pairs[node_kind].second;
  }

  int depth = 0;
  std::string out;
  bool colored = false;

  template<ASTNodeKind node_kind>
  void handle_vertex(V<node_kind> v) {
    out += std::string(depth * 2, ' ');
    out += ast_node_kind_to_string<node_kind>();
    if (std::string postfix = specific_str(v); !postfix.empty()) {
      out += colored ? "  \x1b[34m" : " // ";
      out += postfix;
      out += colored ? "\x1b[0m" : "";
    }
    out += '\n';
    depth++;
    visit_children(v);
    depth--;
  }

  static std::string specific_str(AnyV v) {
    switch (v->kind) {
      case ast_type_leaf_text:
        return static_cast<std::string>(v->as<ast_type_leaf_text>()->text);
      case ast_identifier:
        return static_cast<std::string>(v->as<ast_identifier>()->name);
      case ast_reference: {
        std::string result(v->as<ast_reference>()->get_name());
        if (v->as<ast_reference>()->has_instantiationTs()) {
          result += specific_str(v->as<ast_reference>()->get_instantiationTs());
        }
        return result;
      }
      case ast_int_const:
        return static_cast<std::string>(v->as<ast_int_const>()->orig_str);
      case ast_string_const:
        return "\"" + static_cast<std::string>(v->as<ast_string_const>()->str_val) + "\"";
      case ast_bool_const:
        return v->as<ast_bool_const>()->bool_val ? "true" : "false";
      case ast_dot_access: {
        std::string result = "." + static_cast<std::string>(v->as<ast_dot_access>()->get_field_name());
        if (v->as<ast_dot_access>()->has_instantiationTs()) {
          result += specific_str(v->as<ast_dot_access>()->get_instantiationTs());
        }
        return result;
      }
      case ast_function_call: {
        std::string inner = specific_str(v->as<ast_function_call>()->get_callee());
        if (int n_args = v->as<ast_function_call>()->get_num_args()) {
          return inner + "(..."  + std::to_string(n_args) + ")";
        }
        return inner + "()";
      }
      case ast_global_var_declaration:
        return static_cast<std::string>(v->as<ast_global_var_declaration>()->get_identifier()->name);
      case ast_constant_declaration:
        return static_cast<std::string>(v->as<ast_constant_declaration>()->get_identifier()->name);
      case ast_type_alias_declaration:
        return "type " + static_cast<std::string>(v->as<ast_type_alias_declaration>()->get_identifier()->name);
      case ast_struct_field:
        return static_cast<std::string>(v->as<ast_struct_field>()->get_identifier()->name) + ": " + ast_type_node_to_string(v->as<ast_struct_field>()->type_node);
      case ast_struct_declaration:
        return "struct " + static_cast<std::string>(v->as<ast_struct_declaration>()->get_identifier()->name);
      case ast_assign:
        return "=";
      case ast_set_assign:
        return static_cast<std::string>(v->as<ast_set_assign>()->operator_name) + "=";
      case ast_unary_operator:
        return static_cast<std::string>(v->as<ast_unary_operator>()->operator_name);
      case ast_binary_operator:
        return static_cast<std::string>(v->as<ast_binary_operator>()->operator_name);
      case ast_cast_as_operator:
        return ast_type_node_to_string(v->as<ast_cast_as_operator>()->type_node);
      case ast_is_type_operator: {
        std::string prefix = v->as<ast_is_type_operator>()->is_negated ? "!is " : "is ";
        return prefix + ast_type_node_to_string(v->as<ast_is_type_operator>()->type_node);
      }
      case ast_block_statement:
        return "↓" + std::to_string(v->as<ast_block_statement>()->get_items().size());
      case ast_instantiationT_item:
        return ast_type_node_to_string(v->as<ast_instantiationT_item>()->type_node);
      case ast_if_statement:
        return v->as<ast_if_statement>()->is_ifnot ? "ifnot" : "";
      case ast_annotation:
        return annotation_kinds[static_cast<int>(v->as<ast_annotation>()->kind)].second;
      case ast_parameter:
        return static_cast<std::string>(v->as<ast_parameter>()->param_name) + ": " + ast_type_node_to_string(v->as<ast_parameter>()->type_node);
      case ast_function_declaration: {
        std::string param_names;
        for (int i = 0; i < v->as<ast_function_declaration>()->get_num_params(); i++) {
          if (!param_names.empty())
            param_names += ",";
          param_names += v->as<ast_function_declaration>()->get_param(i)->param_name;
        }
        std::string decl = "fun ";
        if (auto receiver_node = v->as<ast_function_declaration>()->receiver_type_node) {
          decl += specific_str(receiver_node);
          decl += ".";
        }
        return decl + static_cast<std::string>(v->as<ast_function_declaration>()->get_identifier()->name) + "(" + param_names + ")";
      }
      case ast_local_var_lhs: {
        std::string str_type = v->as<ast_local_var_lhs>()->inferred_type ? v->as<ast_local_var_lhs>()->inferred_type->as_human_readable() : ast_type_node_to_string(v->as<ast_local_var_lhs>()->type_node);
        if (v->as<ast_local_var_lhs>()->get_name().empty()) {
          return "_: " + str_type;
        }
        return static_cast<std::string>(v->as<ast_local_var_lhs>()->get_name()) + ": " + str_type;
      }
      case ast_instantiationT_list: {
        std::string result = "<";
        for (AnyV item : v->as<ast_instantiationT_list>()->get_items()) {
          if (result.size() > 1)
            result += ",";
          result += ast_type_node_to_string(item->as<ast_instantiationT_item>()->type_node);
        }
        return result + ">";
      }
      case ast_match_arm:
        if (v->as<ast_match_arm>()->pattern_kind == MatchArmKind::exact_type) {
          return ast_type_node_to_string(v->as<ast_match_arm>()->pattern_type_node);
        }
        if (v->as<ast_match_arm>()->pattern_kind == MatchArmKind::const_expression) {
          return "(expression)";
        }
        return "(else)";
      case ast_object_field:
        return static_cast<std::string>(v->as<ast_object_field>()->get_field_name());
      case ast_object_literal:
        return "↓" + std::to_string(v->as<ast_object_literal>()->get_body()->get_num_fields());
      case ast_tolk_required_version:
        return static_cast<std::string>(v->as<ast_tolk_required_version>()->semver);
      case ast_import_directive:
        return static_cast<std::string>(v->as<ast_import_directive>()->get_file_leaf()->str_val);
      case ast_tolk_file:
        return v->as<ast_tolk_file>()->file->rel_filename;
      default:
        return {};
    }
  }

public:
  explicit ASTStringifier(bool colored) : colored(colored) {
  }

  std::string to_string_with_children(AnyV v) {
    out.clear();
    visit(v);
    return std::move(out);
  }

  static std::string to_string_without_children(AnyV v) {
    std::string result = ast_node_kind_to_string(v->kind);
    if (std::string postfix = specific_str(v); !postfix.empty()) {
      result += ' ';
      result += specific_str(v);
    }
    return result;
  }

  static std::string ast_type_node_to_string(AnyTypeV type_node) {
    if (type_node == nullptr) {
      return "";
    }
    if (auto v_leaf = type_node->try_as<ast_type_leaf_text>()) {
      return static_cast<std::string>(v_leaf->text);
    }
    if (auto v_nullable = type_node->try_as<ast_type_question_nullable>()) {
      return ast_type_node_to_string(v_nullable->get_inner()) + "?";
    }
    return ast_node_kind_to_string(type_node->kind);
  }

  static const char* ast_node_kind_to_string(ASTNodeKind node_kind) {
    return name_pairs[node_kind].second;
  }

  void visit(AnyV v) override {
    switch (v->kind) {
      case ast_identifier:                    return handle_vertex(v->as<ast_identifier>());
      // types
      case ast_type_leaf_text:                return handle_vertex(v->as<ast_type_leaf_text>());
      case ast_type_question_nullable:        return handle_vertex(v->as<ast_type_question_nullable>());
      case ast_type_parenthesis_tensor:       return handle_vertex(v->as<ast_type_parenthesis_tensor>());
      case ast_type_bracket_tuple:            return handle_vertex(v->as<ast_type_bracket_tuple>());
      case ast_type_arrow_callable:           return handle_vertex(v->as<ast_type_arrow_callable>());
      case ast_type_vertical_bar_union:       return handle_vertex(v->as<ast_type_vertical_bar_union>());
      case ast_type_triangle_args:            return handle_vertex(v->as<ast_type_triangle_args>());
      // expressions
      case ast_empty_expression:              return handle_vertex(v->as<ast_empty_expression>());
      case ast_parenthesized_expression:      return handle_vertex(v->as<ast_parenthesized_expression>());
      case ast_braced_expression:             return handle_vertex(v->as<ast_braced_expression>());
      case ast_artificial_aux_vertex:         return handle_vertex(v->as<ast_artificial_aux_vertex>());
      case ast_tensor:                        return handle_vertex(v->as<ast_tensor>());
      case ast_bracket_tuple:                 return handle_vertex(v->as<ast_bracket_tuple>());
      case ast_reference:                     return handle_vertex(v->as<ast_reference>());
      case ast_local_var_lhs:                 return handle_vertex(v->as<ast_local_var_lhs>());
      case ast_local_vars_declaration:        return handle_vertex(v->as<ast_local_vars_declaration>());
      case ast_int_const:                     return handle_vertex(v->as<ast_int_const>());
      case ast_string_const:                  return handle_vertex(v->as<ast_string_const>());
      case ast_bool_const:                    return handle_vertex(v->as<ast_bool_const>());
      case ast_null_keyword:                  return handle_vertex(v->as<ast_null_keyword>());
      case ast_argument:                      return handle_vertex(v->as<ast_argument>());
      case ast_argument_list:                 return handle_vertex(v->as<ast_argument_list>());
      case ast_dot_access:                    return handle_vertex(v->as<ast_dot_access>());
      case ast_function_call:                 return handle_vertex(v->as<ast_function_call>());
      case ast_underscore:                    return handle_vertex(v->as<ast_underscore>());
      case ast_assign:                        return handle_vertex(v->as<ast_assign>());
      case ast_set_assign:                    return handle_vertex(v->as<ast_set_assign>());
      case ast_unary_operator:                return handle_vertex(v->as<ast_unary_operator>());
      case ast_binary_operator:               return handle_vertex(v->as<ast_binary_operator>());
      case ast_ternary_operator:              return handle_vertex(v->as<ast_ternary_operator>());
      case ast_cast_as_operator:              return handle_vertex(v->as<ast_cast_as_operator>());
      case ast_is_type_operator:              return handle_vertex(v->as<ast_is_type_operator>());
      case ast_not_null_operator:             return handle_vertex(v->as<ast_not_null_operator>());
      case ast_match_expression:              return handle_vertex(v->as<ast_match_expression>());
      case ast_match_arm:                     return handle_vertex(v->as<ast_match_arm>());
      case ast_object_field:                  return handle_vertex(v->as<ast_object_field>());
      case ast_object_body:                   return handle_vertex(v->as<ast_object_body>());
      case ast_object_literal:                return handle_vertex(v->as<ast_object_literal>());
      // statements
      case ast_empty_statement:               return handle_vertex(v->as<ast_empty_statement>());
      case ast_block_statement:               return handle_vertex(v->as<ast_block_statement>());
      case ast_return_statement:              return handle_vertex(v->as<ast_return_statement>());
      case ast_if_statement:                  return handle_vertex(v->as<ast_if_statement>());
      case ast_repeat_statement:              return handle_vertex(v->as<ast_repeat_statement>());
      case ast_while_statement:               return handle_vertex(v->as<ast_while_statement>());
      case ast_do_while_statement:            return handle_vertex(v->as<ast_do_while_statement>());
      case ast_throw_statement:               return handle_vertex(v->as<ast_throw_statement>());
      case ast_assert_statement:              return handle_vertex(v->as<ast_assert_statement>());
      case ast_try_catch_statement:           return handle_vertex(v->as<ast_try_catch_statement>());
      case ast_asm_body:                      return handle_vertex(v->as<ast_asm_body>());
      // other
      case ast_genericsT_item:                return handle_vertex(v->as<ast_genericsT_item>());
      case ast_genericsT_list:                return handle_vertex(v->as<ast_genericsT_list>());
      case ast_instantiationT_item:           return handle_vertex(v->as<ast_instantiationT_item>());
      case ast_instantiationT_list:           return handle_vertex(v->as<ast_instantiationT_list>());
      case ast_parameter:                     return handle_vertex(v->as<ast_parameter>());
      case ast_parameter_list:                return handle_vertex(v->as<ast_parameter_list>());
      case ast_annotation:                    return handle_vertex(v->as<ast_annotation>());
      case ast_function_declaration:          return handle_vertex(v->as<ast_function_declaration>());
      case ast_global_var_declaration:        return handle_vertex(v->as<ast_global_var_declaration>());
      case ast_constant_declaration:          return handle_vertex(v->as<ast_constant_declaration>());
      case ast_type_alias_declaration:        return handle_vertex(v->as<ast_type_alias_declaration>());
      case ast_struct_field:                  return handle_vertex(v->as<ast_struct_field>());
      case ast_struct_body:                   return handle_vertex(v->as<ast_struct_body>());
      case ast_struct_declaration:            return handle_vertex(v->as<ast_struct_declaration>());
      case ast_tolk_required_version:         return handle_vertex(v->as<ast_tolk_required_version>());
      case ast_import_directive:              return handle_vertex(v->as<ast_import_directive>());
      case ast_tolk_file:                     return handle_vertex(v->as<ast_tolk_file>());
      default:
        throw UnexpectedASTNodeKind(v, "ASTStringifier::visit");
    }
  }
};

} // namespace tolk

#endif // TOLK_DEBUG
