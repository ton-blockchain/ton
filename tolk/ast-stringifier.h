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
#include <sstream>

/*
 *   ASTStringifier is used to print out the whole vertex tree in a human-readable format.
 *   To stringify any vertex, call v->debug_print(), which uses this class.
 */

namespace tolk {

class ASTStringifier final : public ASTVisitor {
  constexpr static std::pair<ASTNodeType, const char*> name_pairs[] = {
    {ast_empty, "ast_empty"},
    {ast_parenthesized_expr, "ast_parenthesized_expr"},
    {ast_tensor, "ast_tensor"},
    {ast_tensor_square, "ast_tensor_square"},
    {ast_identifier, "ast_identifier"},
    {ast_int_const, "ast_int_const"},
    {ast_string_const, "ast_string_const"},
    {ast_bool_const, "ast_bool_const"},
    {ast_null_keyword, "ast_null_keyword"},
    {ast_self_keyword, "ast_self_keyword"},
    {ast_argument, "ast_argument"},
    {ast_argument_list, "ast_argument_list"},
    {ast_function_call, "ast_function_call"},
    {ast_dot_method_call, "ast_dot_method_call"},
    {ast_global_var_declaration, "ast_global_var_declaration"},
    {ast_constant_declaration, "ast_constant_declaration"},
    {ast_underscore, "ast_underscore"},
    {ast_unary_operator, "ast_unary_operator"},
    {ast_binary_operator, "ast_binary_operator"},
    {ast_ternary_operator, "ast_ternary_operator"},
    {ast_return_statement, "ast_return_statement"},
    {ast_sequence, "ast_sequence"},
    {ast_repeat_statement, "ast_repeat_statement"},
    {ast_while_statement, "ast_while_statement"},
    {ast_do_while_statement, "ast_do_while_statement"},
    {ast_throw_statement, "ast_throw_statement"},
    {ast_assert_statement, "ast_assert_statement"},
    {ast_try_catch_statement, "ast_try_catch_statement"},
    {ast_if_statement, "ast_if_statement"},
    {ast_genericsT_item, "ast_genericsT_item"},
    {ast_genericsT_list, "ast_genericsT_list"},
    {ast_parameter, "ast_parameter"},
    {ast_parameter_list, "ast_parameter_list"},
    {ast_asm_body, "ast_asm_body"},
    {ast_annotation, "ast_annotation"},
    {ast_function_declaration, "ast_function_declaration"},
    {ast_local_var, "ast_local_var"},
    {ast_local_vars_declaration, "ast_local_vars_declaration"},
    {ast_tolk_required_version, "ast_tolk_required_version"},
    {ast_import_statement, "ast_import_statement"},
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
      case ast_int_const:
        return static_cast<std::string>(v->as<ast_int_const>()->int_val);
      case ast_string_const:
        if (char modifier = v->as<ast_string_const>()->modifier) {
          return "\"" + static_cast<std::string>(v->as<ast_string_const>()->str_val) + "\"" + std::string(1, modifier);
        } else {
          return "\"" + static_cast<std::string>(v->as<ast_string_const>()->str_val) + "\"";
        }
      case ast_function_call: {
        if (auto v_lhs = v->as<ast_function_call>()->get_called_f()->try_as<ast_identifier>()) {
          return static_cast<std::string>(v_lhs->name) + "()";
        }
        return {};
      }
      case ast_dot_method_call:
        return static_cast<std::string>(v->as<ast_dot_method_call>()->method_name);
      case ast_global_var_declaration:
        return static_cast<std::string>(v->as<ast_global_var_declaration>()->get_identifier()->name);
      case ast_constant_declaration:
        return static_cast<std::string>(v->as<ast_constant_declaration>()->get_identifier()->name);
      case ast_unary_operator:
        return static_cast<std::string>(v->as<ast_unary_operator>()->operator_name);
      case ast_binary_operator:
        return static_cast<std::string>(v->as<ast_binary_operator>()->operator_name);
      case ast_sequence:
        return "↓" + std::to_string(v->as<ast_sequence>()->get_items().size());
      case ast_if_statement:
        return v->as<ast_if_statement>()->is_ifnot ? "ifnot" : "";
      case ast_annotation:
        return annotation_kinds[static_cast<int>(v->as<ast_annotation>()->kind)].second;
      case ast_parameter: {
        std::ostringstream os;
        os << v->as<ast_parameter>()->param_type;
        return static_cast<std::string>(v->as<ast_parameter>()->get_identifier()->name) + ": " + os.str();
      }
      case ast_function_declaration: {
        std::string param_names;
        for (int i = 0; i < v->as<ast_function_declaration>()->get_num_params(); i++) {
          if (!param_names.empty())
            param_names += ",";
          param_names += v->as<ast_function_declaration>()->get_param(i)->get_identifier()->name;
        }
        return "fun " + static_cast<std::string>(v->as<ast_function_declaration>()->get_identifier()->name) + "(" + param_names + ")";
      }
      case ast_local_var: {
        std::ostringstream os;
        os << v->as<ast_local_var>()->declared_type;
        if (auto v_ident = v->as<ast_local_var>()->get_identifier()->try_as<ast_identifier>()) {
          return static_cast<std::string>(v_ident->name) + ":" + os.str();
        }
        return "_: " + os.str();
      }
      case ast_tolk_required_version:
        return static_cast<std::string>(v->as<ast_tolk_required_version>()->semver);
      case ast_import_statement:
        return static_cast<std::string>(v->as<ast_import_statement>()->get_file_leaf()->str_val);
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
      case ast_empty:                         return handle_vertex(v->as<ast_empty>());
      case ast_parenthesized_expr:            return handle_vertex(v->as<ast_parenthesized_expr>());
      case ast_tensor:                        return handle_vertex(v->as<ast_tensor>());
      case ast_tensor_square:                 return handle_vertex(v->as<ast_tensor_square>());
      case ast_identifier:                    return handle_vertex(v->as<ast_identifier>());
      case ast_int_const:                     return handle_vertex(v->as<ast_int_const>());
      case ast_string_const:                  return handle_vertex(v->as<ast_string_const>());
      case ast_bool_const:                    return handle_vertex(v->as<ast_bool_const>());
      case ast_null_keyword:                  return handle_vertex(v->as<ast_null_keyword>());
      case ast_self_keyword:                  return handle_vertex(v->as<ast_self_keyword>());
      case ast_argument:                      return handle_vertex(v->as<ast_argument>());
      case ast_argument_list:                 return handle_vertex(v->as<ast_argument_list>());
      case ast_function_call:                 return handle_vertex(v->as<ast_function_call>());
      case ast_dot_method_call:               return handle_vertex(v->as<ast_dot_method_call>());
      case ast_global_var_declaration:        return handle_vertex(v->as<ast_global_var_declaration>());
      case ast_constant_declaration:          return handle_vertex(v->as<ast_constant_declaration>());
      case ast_underscore:                    return handle_vertex(v->as<ast_underscore>());
      case ast_unary_operator:                return handle_vertex(v->as<ast_unary_operator>());
      case ast_binary_operator:               return handle_vertex(v->as<ast_binary_operator>());
      case ast_ternary_operator:              return handle_vertex(v->as<ast_ternary_operator>());
      case ast_return_statement:              return handle_vertex(v->as<ast_return_statement>());
      case ast_sequence:                      return handle_vertex(v->as<ast_sequence>());
      case ast_repeat_statement:              return handle_vertex(v->as<ast_repeat_statement>());
      case ast_while_statement:               return handle_vertex(v->as<ast_while_statement>());
      case ast_do_while_statement:            return handle_vertex(v->as<ast_do_while_statement>());
      case ast_throw_statement:               return handle_vertex(v->as<ast_throw_statement>());
      case ast_assert_statement:              return handle_vertex(v->as<ast_assert_statement>());
      case ast_try_catch_statement:           return handle_vertex(v->as<ast_try_catch_statement>());
      case ast_if_statement:                  return handle_vertex(v->as<ast_if_statement>());
      case ast_genericsT_item:                return handle_vertex(v->as<ast_genericsT_item>());
      case ast_genericsT_list:                return handle_vertex(v->as<ast_genericsT_list>());
      case ast_parameter:                     return handle_vertex(v->as<ast_parameter>());
      case ast_parameter_list:                return handle_vertex(v->as<ast_parameter_list>());
      case ast_asm_body:                      return handle_vertex(v->as<ast_asm_body>());
      case ast_annotation:                    return handle_vertex(v->as<ast_annotation>());
      case ast_function_declaration:          return handle_vertex(v->as<ast_function_declaration>());
      case ast_local_var:                     return handle_vertex(v->as<ast_local_var>());
      case ast_local_vars_declaration:        return handle_vertex(v->as<ast_local_vars_declaration>());
      case ast_tolk_required_version:         return handle_vertex(v->as<ast_tolk_required_version>());
      case ast_import_statement:              return handle_vertex(v->as<ast_import_statement>());
      case ast_tolk_file:                     return handle_vertex(v->as<ast_tolk_file>());
      default:
        throw UnexpectedASTNodeType(v, "ASTStringifier::visit");
    }
  }
};

} // namespace tolk

#endif // TOLK_DEBUG
