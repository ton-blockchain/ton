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
  constexpr static std::pair<ASTNodeType, const char*> name_pairs[] = {
    {ast_identifier, "ast_identifier"},
    // expressions
    {ast_empty_expression, "ast_empty_expression"},
    {ast_parenthesized_expression, "ast_parenthesized_expression"},
    {ast_tensor, "ast_tensor"},
    {ast_typed_tuple, "ast_typed_tuple"},
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
    // statements
    {ast_empty_statement, "ast_empty_statement"},
    {ast_sequence, "ast_sequence"},
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

  template<ASTNodeType node_type>
  constexpr static const char* ast_node_type_to_string() {
    return name_pairs[node_type].second;
  }

  int depth = 0;
  std::string out;
  bool colored = false;

  template<ASTNodeType node_type>
  void handle_vertex(V<node_type> v) {
    out += std::string(depth * 2, ' ');
    out += ast_node_type_to_string<node_type>();
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
    switch (v->type) {
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
        if (char modifier = v->as<ast_string_const>()->modifier) {
          return "\"" + static_cast<std::string>(v->as<ast_string_const>()->str_val) + "\"" + std::string(1, modifier);
        } else {
          return "\"" + static_cast<std::string>(v->as<ast_string_const>()->str_val) + "\"";
        }
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
      case ast_assign:
        return "=";
      case ast_set_assign:
        return static_cast<std::string>(v->as<ast_set_assign>()->operator_name) + "=";
      case ast_unary_operator:
        return static_cast<std::string>(v->as<ast_unary_operator>()->operator_name);
      case ast_binary_operator:
        return static_cast<std::string>(v->as<ast_binary_operator>()->operator_name);
      case ast_cast_as_operator:
        return v->as<ast_cast_as_operator>()->cast_to_type->as_human_readable();
      case ast_sequence:
        return "↓" + std::to_string(v->as<ast_sequence>()->get_items().size());
      case ast_instantiationT_item:
        return v->as<ast_instantiationT_item>()->substituted_type->as_human_readable();
      case ast_if_statement:
        return v->as<ast_if_statement>()->is_ifnot ? "ifnot" : "";
      case ast_annotation:
        return annotation_kinds[static_cast<int>(v->as<ast_annotation>()->kind)].second;
      case ast_parameter: {
        std::ostringstream os;
        os << v->as<ast_parameter>()->declared_type;
        return static_cast<std::string>(v->as<ast_parameter>()->param_name) + ": " + os.str();
      }
      case ast_function_declaration: {
        std::string param_names;
        for (int i = 0; i < v->as<ast_function_declaration>()->get_num_params(); i++) {
          if (!param_names.empty())
            param_names += ",";
          param_names += v->as<ast_function_declaration>()->get_param(i)->param_name;
        }
        return "fun " + static_cast<std::string>(v->as<ast_function_declaration>()->get_identifier()->name) + "(" + param_names + ")";
      }
      case ast_local_var_lhs: {
        std::ostringstream os;
        os << (v->as<ast_local_var_lhs>()->inferred_type ? v->as<ast_local_var_lhs>()->inferred_type : v->as<ast_local_var_lhs>()->declared_type);
        if (v->as<ast_local_var_lhs>()->get_name().empty()) {
          return "_: " + os.str();
        }
        return static_cast<std::string>(v->as<ast_local_var_lhs>()->get_name()) + ":" + os.str();
      }
      case ast_instantiationT_list: {
        std::string result = "<";
        for (AnyV item : v->as<ast_instantiationT_list>()->get_items()) {
          if (result.size() > 1)
            result += ",";
          result += item->as<ast_instantiationT_item>()->substituted_type->as_human_readable();
        }
        return result + ">";
      }
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
    std::string result = ast_node_type_to_string(v->type);
    if (std::string postfix = specific_str(v); !postfix.empty()) {
      result += ' ';
      result += specific_str(v);
    }
    return result;
  }

  static const char* ast_node_type_to_string(ASTNodeType node_type) {
    return name_pairs[node_type].second;
  }

  void visit(AnyV v) override {
    switch (v->type) {
      case ast_identifier:                    return handle_vertex(v->as<ast_identifier>());
      // expressions
      case ast_empty_expression:              return handle_vertex(v->as<ast_empty_expression>());
      case ast_parenthesized_expression:      return handle_vertex(v->as<ast_parenthesized_expression>());
      case ast_tensor:                        return handle_vertex(v->as<ast_tensor>());
      case ast_typed_tuple:                   return handle_vertex(v->as<ast_typed_tuple>());
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
      // statements
      case ast_empty_statement:               return handle_vertex(v->as<ast_empty_statement>());
      case ast_sequence:                      return handle_vertex(v->as<ast_sequence>());
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
      case ast_tolk_required_version:         return handle_vertex(v->as<ast_tolk_required_version>());
      case ast_import_directive:              return handle_vertex(v->as<ast_import_directive>());
      case ast_tolk_file:                     return handle_vertex(v->as<ast_tolk_file>());
      default:
        throw UnexpectedASTNodeType(v, "ASTStringifier::visit");
    }
  }
};

} // namespace tolk

#endif // TOLK_DEBUG
