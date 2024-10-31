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

int Vertex<ast_forall_list>::lookup_idx(std::string_view nameT) const {
  for (size_t idx = 0; idx < children.size(); ++idx) {
    if (children[idx] && children[idx]->as<ast_forall_item>()->nameT == nameT) {
      return static_cast<int>(idx);
    }
  }
  return -1;
}

int Vertex<ast_argument_list>::lookup_idx(std::string_view arg_name) const {
  for (size_t idx = 0; idx < children.size(); ++idx) {
    if (children[idx] && children[idx]->as<ast_argument>()->get_identifier()->name == arg_name) {
      return static_cast<int>(idx);
    }
  }
  return -1;
}

void Vertex<ast_include_statement>::mutate_set_src_file(const SrcFile* file) const {
  const_cast<Vertex*>(this)->file = file;
}

} // namespace tolk
