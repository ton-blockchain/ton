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
    {ast_identifier, "ast_identifier"},
    {ast_int_const, "ast_int_const"},
    {ast_string_const, "ast_string_const"},
    {ast_bool_const, "ast_bool_const"},
    {ast_nil_tuple, "ast_nil_tuple"},
    {ast_function_call, "ast_function_call"},
    {ast_parenthesized_expr, "ast_parenthesized_expr"},
    {ast_global_var_declaration, "ast_global_var_declaration"},
    {ast_global_var_declaration_list, "ast_global_var_declaration_list"},
    {ast_constant_declaration, "ast_constant_declaration"},
    {ast_constant_declaration_list, "ast_constant_declaration_list"},
    {ast_underscore, "ast_underscore"},
    {ast_type_expression, "ast_type_expression"},
    {ast_variable_declaration, "ast_variable_declaration"},
    {ast_tensor, "ast_tensor"},
    {ast_tensor_square, "ast_tensor_square"},
    {ast_dot_tilde_call, "ast_dot_tilde_call"},
    {ast_unary_operator, "ast_unary_operator"},
    {ast_binary_operator, "ast_binary_operator"},
    {ast_ternary_operator, "ast_ternary_operator"},
    {ast_return_statement, "ast_return_statement"},
    {ast_sequence, "ast_sequence"},
    {ast_repeat_statement, "ast_repeat_statement"},
    {ast_while_statement, "ast_while_statement"},
    {ast_do_until_statement, "ast_do_until_statement"},
    {ast_try_catch_statement, "ast_try_catch_statement"},
    {ast_if_statement, "ast_if_statement"},
    {ast_forall_item, "ast_forall_item"},
    {ast_forall_list, "ast_forall_list"},
    {ast_argument, "ast_argument"},
    {ast_argument_list, "ast_argument_list"},
    {ast_asm_body, "ast_asm_body"},
    {ast_function_declaration, "ast_function_declaration"},
    {ast_pragma_no_arg, "ast_pragma_no_arg"},
    {ast_pragma_version, "ast_pragma_version"},
    {ast_include_statement, "ast_include_statement"},
    {ast_tolk_file, "ast_tolk_file"},
  };

  template<ASTNodeType node_type>
  constexpr static const char* ast_node_type_to_string() {
    static_assert(std::size(name_pairs) == ast_tolk_file + 1, "name_pairs needs to be updated");
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

  static std::string specific_str(AnyV node) {
    switch (node->type) {
      case ast_identifier:
        return static_cast<std::string>(node->as<ast_identifier>()->name);
      case ast_int_const:
        return static_cast<std::string>(node->as<ast_int_const>()->int_val);
      case ast_string_const:
        if (char modifier = node->as<ast_string_const>()->modifier) {
          return "\"" + static_cast<std::string>(node->as<ast_string_const>()->str_val) + "\"" + std::string(1, modifier);
        } else {
          return "\"" + static_cast<std::string>(node->as<ast_string_const>()->str_val) + "\"";
        }
      case ast_global_var_declaration:
        return static_cast<std::string>(node->as<ast_global_var_declaration>()->var_name);
      case ast_constant_declaration:
        return static_cast<std::string>(node->as<ast_constant_declaration>()->const_name);
      case ast_type_expression: {
        std::ostringstream os;
        os << node->as<ast_type_expression>()->declared_type;
        return os.str();
      }
      case ast_variable_declaration: {
        std::ostringstream os;
        os << node->as<ast_variable_declaration>()->declared_type;
        return os.str();
      }
      case ast_dot_tilde_call:
        return static_cast<std::string>(node->as<ast_dot_tilde_call>()->method_name);
      case ast_unary_operator:
        return static_cast<std::string>(node->as<ast_unary_operator>()->operator_name);
      case ast_binary_operator:
        return static_cast<std::string>(node->as<ast_binary_operator>()->operator_name);
      case ast_sequence:
        return "↓" + std::to_string(node->as<ast_sequence>()->get_items().size());
      case ast_if_statement:
        return node->as<ast_if_statement>()->is_ifnot ? "ifnot" : "";
      case ast_argument: {
        std::ostringstream os;
        os << node->as<ast_argument>()->arg_type;
        return static_cast<std::string>(node->as<ast_argument>()->arg_name) + ": " + os.str();
      }
      case ast_function_declaration: {
        std::string arg_names;
        for (int i = 0; i < node->as<ast_function_declaration>()->get_num_args(); i++) {
          if (!arg_names.empty())
            arg_names += ",";
          arg_names += node->as<ast_function_declaration>()->get_arg(i)->arg_name;
        }
        return "fun " + node->as<ast_function_declaration>()->name + "(" + arg_names + ")";
      }
      case ast_pragma_no_arg:
        return static_cast<std::string>(node->as<ast_pragma_no_arg>()->pragma_name);
      case ast_pragma_version:
        return static_cast<std::string>(node->as<ast_pragma_version>()->semver);
      case ast_include_statement:
        return static_cast<std::string>(node->as<ast_include_statement>()->file_name);
      case ast_tolk_file:
        return node->as<ast_tolk_file>()->file->rel_filename;
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
      case ast_identifier:                    return handle_vertex(v->as<ast_identifier>());
      case ast_int_const:                     return handle_vertex(v->as<ast_int_const>());
      case ast_string_const:                  return handle_vertex(v->as<ast_string_const>());
      case ast_bool_const:                    return handle_vertex(v->as<ast_bool_const>());
      case ast_nil_tuple:                     return handle_vertex(v->as<ast_nil_tuple>());
      case ast_function_call:                 return handle_vertex(v->as<ast_function_call>());
      case ast_parenthesized_expr:            return handle_vertex(v->as<ast_parenthesized_expr>());
      case ast_global_var_declaration:        return handle_vertex(v->as<ast_global_var_declaration>());
      case ast_global_var_declaration_list:   return handle_vertex(v->as<ast_global_var_declaration_list>());
      case ast_constant_declaration:          return handle_vertex(v->as<ast_constant_declaration>());
      case ast_constant_declaration_list:     return handle_vertex(v->as<ast_constant_declaration_list>());
      case ast_underscore:                    return handle_vertex(v->as<ast_underscore>());
      case ast_type_expression:               return handle_vertex(v->as<ast_type_expression>());
      case ast_variable_declaration:          return handle_vertex(v->as<ast_variable_declaration>());
      case ast_tensor:                        return handle_vertex(v->as<ast_tensor>());
      case ast_tensor_square:                 return handle_vertex(v->as<ast_tensor_square>());
      case ast_dot_tilde_call:                return handle_vertex(v->as<ast_dot_tilde_call>());
      case ast_unary_operator:                return handle_vertex(v->as<ast_unary_operator>());
      case ast_binary_operator:               return handle_vertex(v->as<ast_binary_operator>());
      case ast_ternary_operator:              return handle_vertex(v->as<ast_ternary_operator>());
      case ast_return_statement:              return handle_vertex(v->as<ast_return_statement>());
      case ast_sequence:                      return handle_vertex(v->as<ast_sequence>());
      case ast_repeat_statement:              return handle_vertex(v->as<ast_repeat_statement>());
      case ast_while_statement:               return handle_vertex(v->as<ast_while_statement>());
      case ast_do_until_statement:            return handle_vertex(v->as<ast_do_until_statement>());
      case ast_try_catch_statement:           return handle_vertex(v->as<ast_try_catch_statement>());
      case ast_if_statement:                  return handle_vertex(v->as<ast_if_statement>());
      case ast_forall_item:                   return handle_vertex(v->as<ast_forall_item>());
      case ast_forall_list:                   return handle_vertex(v->as<ast_forall_list>());
      case ast_argument:                      return handle_vertex(v->as<ast_argument>());
      case ast_argument_list:                 return handle_vertex(v->as<ast_argument_list>());
      case ast_asm_body:                      return handle_vertex(v->as<ast_asm_body>());
      case ast_function_declaration:          return handle_vertex(v->as<ast_function_declaration>());
      case ast_pragma_no_arg:                 return handle_vertex(v->as<ast_pragma_no_arg>());
      case ast_pragma_version:                return handle_vertex(v->as<ast_pragma_version>());
      case ast_include_statement:             return handle_vertex(v->as<ast_include_statement>());
      case ast_tolk_file:                     return handle_vertex(v->as<ast_tolk_file>());
      default:
        throw UnexpectedASTNodeType(v, "ASTStringifier::visit");
    }
  }
};

} // namespace tolk

#endif // TOLK_DEBUG
