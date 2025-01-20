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

UnexpectedASTNodeType::UnexpectedASTNodeType(AnyV v_unexpected, const char* place_where): v_unexpected(v_unexpected) {
  message = "Unexpected ASTNodeType ";
#ifdef TOLK_DEBUG
  message += ASTStringifier::ast_node_type_to_string(v_unexpected->type);
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

void ASTNodeExpressionBase::assign_inferred_type(TypePtr type) {
  this->inferred_type = type;
}

void ASTNodeExpressionBase::assign_rvalue_true() {
  this->is_rvalue = true;
}

void ASTNodeExpressionBase::assign_lvalue_true() {
  this->is_lvalue = true;
}

void Vertex<ast_reference>::assign_sym(const Symbol* sym) {
  this->sym = sym;
}

void Vertex<ast_function_call>::assign_fun_ref(const FunctionData* fun_ref) {
  this->fun_maybe = fun_ref;
}

void Vertex<ast_cast_as_operator>::assign_resolved_type(TypePtr cast_to_type) {
  this->cast_to_type = cast_to_type;
}

void Vertex<ast_global_var_declaration>::assign_var_ref(const GlobalVarData* var_ref) {
  this->var_ref = var_ref;
}

void Vertex<ast_global_var_declaration>::assign_resolved_type(TypePtr declared_type) {
  this->declared_type = declared_type;
}

void Vertex<ast_constant_declaration>::assign_const_ref(const GlobalConstData* const_ref) {
  this->const_ref = const_ref;
}

void Vertex<ast_constant_declaration>::assign_resolved_type(TypePtr declared_type) {
  this->declared_type = declared_type;
}

void Vertex<ast_instantiationT_item>::assign_resolved_type(TypePtr substituted_type) {
  this->substituted_type = substituted_type;
}

void Vertex<ast_parameter>::assign_param_ref(const LocalVarData* param_ref) {
  this->param_ref = param_ref;
}

void Vertex<ast_parameter>::assign_resolved_type(TypePtr declared_type) {
  this->declared_type = declared_type;
}

void Vertex<ast_set_assign>::assign_fun_ref(const FunctionData* fun_ref) {
  this->fun_ref = fun_ref;
}

void Vertex<ast_unary_operator>::assign_fun_ref(const FunctionData* fun_ref) {
  this->fun_ref = fun_ref;
}

void Vertex<ast_binary_operator>::assign_fun_ref(const FunctionData* fun_ref) {
  this->fun_ref = fun_ref;
}

void Vertex<ast_dot_access>::assign_target(const DotTarget& target) {
  this->target = target;
}

void Vertex<ast_function_declaration>::assign_fun_ref(const FunctionData* fun_ref) {
  this->fun_ref = fun_ref;
}

void Vertex<ast_function_declaration>::assign_resolved_type(TypePtr declared_return_type) {
  this->declared_return_type = declared_return_type;
}

void Vertex<ast_local_var_lhs>::assign_var_ref(const LocalVarData* var_ref) {
  this->var_ref = var_ref;
}

void Vertex<ast_local_var_lhs>::assign_resolved_type(TypePtr declared_type) {
  this->declared_type = declared_type;
}

void Vertex<ast_import_directive>::assign_src_file(const SrcFile* file) {
  this->file = file;
}

} // namespace tolk
