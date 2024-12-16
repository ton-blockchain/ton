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
#include "symtable.h"
#include "compiler-state.h"
#include "platform-utils.h"
#include <sstream>
#include <cassert>

namespace tolk {

bool FunctionData::does_need_codegen() const {
  // when a function is declared, but not referenced from code in any way, don't generate its body
  if (!is_really_used() && G.settings.remove_unused_functions) {
    return false;
  }
  // when a function is referenced like `var a = some_fn;` (or in some other non-call way), its continuation should exist
  if (is_used_as_noncall()) {
    return true;
  }
  // currently, there is no inlining, all functions are codegenerated
  // (but actually, unused ones are later removed by Fift)
  // in the future, we may want to implement a true AST inlining for "simple" functions
  return true;
}

void FunctionData::assign_is_really_used() {
  this->flags |= flagReallyUsed;
}

void FunctionData::assign_is_used_as_noncall() {
  this->flags |= flagUsedAsNonCall;
}

void FunctionData::assign_is_implicit_return() {
  this->flags |= flagImplicitReturn;
}

void GlobalVarData::assign_is_really_used() {
  this->flags |= flagReallyUsed;
}

void LocalVarData::assign_idx(int idx) {
  this->idx = idx;
}

GNU_ATTRIBUTE_NORETURN GNU_ATTRIBUTE_COLD
static void fire_error_redefinition_of_symbol(SrcLocation loc, const Symbol* previous) {
  SrcLocation prev_loc = previous->loc;
  if (prev_loc.is_stdlib()) {
    throw ParseError(loc, "redefinition of a symbol from stdlib");
  }
  if (prev_loc.is_defined()) {
    throw ParseError(loc, "redefinition of symbol, previous was at: " + prev_loc.to_string());
  }
  throw ParseError(loc, "redefinition of built-in symbol");
}

void GlobalSymbolTable::add_function(const FunctionData* f_sym) {
  auto key = key_hash(f_sym->name);
  auto [it, inserted] = entries.emplace(key, f_sym);
  if (!inserted) {
    fire_error_redefinition_of_symbol(f_sym->loc, it->second);
  }
}

void GlobalSymbolTable::add_global_var(const GlobalVarData* g_sym) {
  auto key = key_hash(g_sym->name);
  auto [it, inserted] = entries.emplace(key, g_sym);
  if (!inserted) {
    fire_error_redefinition_of_symbol(g_sym->loc, it->second);
  }
}

void GlobalSymbolTable::add_global_const(const GlobalConstData* c_sym) {
  auto key = key_hash(c_sym->name);
  auto [it, inserted] = entries.emplace(key, c_sym);
  if (!inserted) {
    fire_error_redefinition_of_symbol(c_sym->loc, it->second);
  }
}

const Symbol* lookup_global_symbol(std::string_view name) {
  return G.symtable.lookup(name);
}

}  // namespace tolk
