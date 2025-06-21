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
#include "ast.h"
#ifdef TOLK_DEBUG
#include "ast-stringifier.h"
#endif

namespace tolk {

static_assert(sizeof(ASTNodeBase) == 12);

#ifdef TOLK_DEBUG

std::string ASTNodeBase::to_debug_string(bool colored) const {
  ASTStringifier s(colored);
  return s.to_string_with_children(this);
}

void ASTNodeBase::debug_print() const {
  std::cerr << to_debug_string(true) << std::endl;
}

#endif  // TOLK_DEBUG

UnexpectedASTNodeKind::UnexpectedASTNodeKind(AnyV v_unexpected, const char* place_where): v_unexpected(v_unexpected) {
  message = "Unexpected ASTNodeKind ";
#ifdef TOLK_DEBUG
  message += ASTStringifier::ast_node_kind_to_string(v_unexpected->kind);
  message += " ";
#endif
  message += "in ";
  message += place_where;
}

void ASTNodeBase::error(const std::string& err_msg) const {
  throw ParseError(loc, err_msg);
}

AnnotationKind Vertex<ast_annotation>::parse_kind(std::string_view name) {
  if (name == "@pure") {
    return AnnotationKind::pure;
  }
  if (name == "@inline") {
    return AnnotationKind::inline_simple;
  }
  if (name == "@inline_ref") {
    return AnnotationKind::inline_ref;
  }
  if (name == "@method_id") {
    return AnnotationKind::method_id;
  }
  if (name == "@deprecated") {
    return AnnotationKind::deprecated;
  }
  if (name == "@custom") {
    return AnnotationKind::custom;
  }
  if (name == "@overflow1023_policy") {
    return AnnotationKind::overflow1023_policy;
  }
  return AnnotationKind::unknown;
}

int Vertex<ast_genericsT_list>::lookup_idx(std::string_view nameT) const {
  for (size_t idx = 0; idx < children.size(); ++idx) {
    if (children[idx] && children[idx]->as<ast_genericsT_item>()->nameT == nameT) {
      return static_cast<int>(idx);
    }
  }
  return -1;
}

int Vertex<ast_parameter_list>::lookup_idx(std::string_view param_name) const {
  for (size_t idx = 0; idx < children.size(); ++idx) {
    if (children[idx] && children[idx]->as<ast_parameter>()->param_name == param_name) {
      return static_cast<int>(idx);
    }
  }
  return -1;
}

int Vertex<ast_parameter_list>::get_mutate_params_count() const {
  int n = 0;
  for (AnyV param : children) {
    if (param->as<ast_parameter>()->declared_as_mutate) {
      n++;
    }
  }
  return n;
}

// ---------------------------------------------------------
// "assign" methods
//
// From the user's point of view, all AST vertices are constant, fields are public, but can't be modified.
// The only way to modify a field is to call "mutate()" and then use these "assign_*" methods.
// Therefore, there is a guarantee, that all AST mutations are done via these methods,
// easily searched by usages, and there is no another way to modify any other field.

void ASTNodeDeclaredTypeBase::assign_resolved_type(TypePtr resolved_type) {
  this->resolved_type = resolved_type;
}

void ASTNodeExpressionBase::assign_inferred_type(TypePtr type) {
  this->inferred_type = type;
}

void ASTNodeExpressionBase::assign_rvalue_true() {
  this->is_rvalue = true;
}

void ASTNodeExpressionBase::assign_lvalue_true() {
  this->is_lvalue = true;
}

void ASTNodeExpressionBase::assign_always_true_or_false(int flow_true_false_state) {
  this->is_always_true = flow_true_false_state == 1;      // see smart-casts-cfg.h
  this->is_always_false = flow_true_false_state == 2;
}

void Vertex<ast_reference>::assign_sym(const Symbol* sym) {
  this->sym = sym;
}

void Vertex<ast_string_const>::assign_literal_value(std::string&& literal_value) {
  this->literal_value = std::move(literal_value);
}

void Vertex<ast_function_call>::assign_fun_ref(FunctionPtr fun_ref, bool dot_obj_is_self) {
  this->fun_maybe = fun_ref;
  this->dot_obj_is_self = dot_obj_is_self;
}

void Vertex<ast_is_type_operator>::assign_is_negated(bool is_negated) {
  this->is_negated = is_negated;
}

void Vertex<ast_match_arm>::assign_resolved_pattern(MatchArmKind pattern_kind, AnyExprV pattern_expr) {
  this->pattern_type_node = nullptr;
  this->pattern_kind = pattern_kind;
  this->lhs = pattern_expr;
}

void Vertex<ast_global_var_declaration>::assign_glob_ref(GlobalVarPtr glob_ref) {
  this->glob_ref = glob_ref;
}

void Vertex<ast_constant_declaration>::assign_const_ref(GlobalConstPtr const_ref) {
  this->const_ref = const_ref;
}

void Vertex<ast_type_alias_declaration>::assign_alias_ref(AliasDefPtr alias_ref) {
  this->alias_ref = alias_ref;
}

void Vertex<ast_struct_declaration>::assign_struct_ref(StructPtr struct_ref) {
  this->struct_ref = struct_ref;
}

void Vertex<ast_set_assign>::assign_fun_ref(FunctionPtr fun_ref) {
  this->fun_ref = fun_ref;
}

void Vertex<ast_unary_operator>::assign_fun_ref(FunctionPtr fun_ref) {
  this->fun_ref = fun_ref;
}

void Vertex<ast_binary_operator>::assign_fun_ref(FunctionPtr fun_ref) {
  this->fun_ref = fun_ref;
}

void Vertex<ast_block_statement>::assign_first_unreachable(AnyV first_unreachable) {
  this->first_unreachable = first_unreachable;
}

void Vertex<ast_dot_access>::assign_target(const DotTarget& target) {
  this->target = target;
}

void Vertex<ast_object_field>::assign_field_ref(StructFieldPtr field_ref) {
  this->field_ref = field_ref;
}

void Vertex<ast_object_literal>::assign_struct_ref(StructPtr struct_ref) {
  this->struct_ref = struct_ref;
}

void Vertex<ast_function_declaration>::assign_fun_ref(FunctionPtr fun_ref) {
  this->fun_ref = fun_ref;
}

void Vertex<ast_local_var_lhs>::assign_var_ref(LocalVarPtr var_ref) {
  this->var_ref = var_ref;
}

void Vertex<ast_import_directive>::assign_src_file(const SrcFile* file) {
  this->file = file;
}

} // namespace tolk
