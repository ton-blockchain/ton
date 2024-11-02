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
#include "ast-stringifier.h"
#include <iostream>

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
    if (children[idx] && children[idx]->as<ast_parameter>()->get_identifier()->name == param_name) {
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

void Vertex<ast_import_statement>::mutate_set_src_file(const SrcFile* file) const {
  const_cast<Vertex*>(this)->file = file;
}

} // namespace tolk
