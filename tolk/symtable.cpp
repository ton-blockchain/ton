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
#include "generics-helpers.h"

namespace tolk {

std::string FunctionData::as_human_readable() const {
  if (!genericTs) {
    return name;  // if it's generic instantiation like `f<int>`, its name is "f<int>", not "f"
  }
  return name + genericTs->as_human_readable();
}

bool FunctionData::does_need_codegen() const {
  // when a function is declared, but not referenced from code in any way, don't generate its body
  if (!is_really_used() && G.settings.remove_unused_functions) {
    return false;
  }
  // functions with asm body don't need code generation
  // (even if used as non-call: `var a = beginCell;` inserts TVM continuation inline)
  if (is_asm_function() || is_builtin_function()) {
    return false;
  }
  // when a function is referenced like `var a = some_fn;` (or in some other non-call way), its continuation should exist
  if (is_used_as_noncall()) {
    return true;
  }
  // generic functions also don't need code generation, only generic instantiations do
  if (is_generic_function()) {
    return false;
  }
  // currently, there is no inlining, all functions are codegenerated
  // (but actually, unused ones are later removed by Fift)
  // in the future, we may want to implement a true AST inlining for "simple" functions
  return true;
}

void FunctionData::assign_resolved_type(TypePtr declared_return_type) {
  this->declared_return_type = declared_return_type;
}

void FunctionData::assign_inferred_type(TypePtr inferred_return_type, TypePtr inferred_full_type) {
  this->inferred_return_type = inferred_return_type;
  this->inferred_full_type = inferred_full_type;
}

void FunctionData::assign_is_used_as_noncall() {
  this->flags |= flagUsedAsNonCall;
}

void FunctionData::assign_is_implicit_return() {
  this->flags |= flagImplicitReturn;
}

void FunctionData::assign_is_type_inferring_done() {
  this->flags |= flagTypeInferringDone;
}

void FunctionData::assign_is_really_used() {
  this->flags |= flagReallyUsed;
}

void FunctionData::assign_arg_order(std::vector<int>&& arg_order) {
  this->arg_order = std::move(arg_order);
}

void GlobalVarData::assign_resolved_type(TypePtr declared_type) {
  this->declared_type = declared_type;
}

void GlobalVarData::assign_is_really_used() {
  this->flags |= flagReallyUsed;
}

void GlobalConstData::assign_resolved_type(TypePtr declared_type) {
  this->declared_type = declared_type;
}

void LocalVarData::assign_idx(int idx) {
  this->idx = idx;
}

void LocalVarData::assign_resolved_type(TypePtr declared_type) {
  this->declared_type = declared_type;
}

void LocalVarData::assign_inferred_type(TypePtr inferred_type) {
#ifdef TOLK_DEBUG
  assert(this->declared_type == nullptr);  // called when type declaration omitted, inferred from assigned value
#endif
  this->declared_type = inferred_type;
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
