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
  if (!is_generic_function()) {
    return name;  // if it's generic instantiation like `f<int>`, its name is "f<int>", not "f"
  }
  return name + genericTs->as_human_readable();
}

std::string AliasDefData::as_human_readable() const {
  if (!is_generic_alias()) {
    return name;
  }
  return name + genericTs->as_human_readable();
}

std::string StructData::as_human_readable() const {
  if (!is_generic_struct()) {
    return name;
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

void FunctionData::assign_resolved_receiver_type(TypePtr receiver_type, std::string&& name_prefix) {
  this->receiver_type = receiver_type;
  if (!this->substitutedTs) {   // after receiver has been resolve, update name to "receiver.method"
    name_prefix.erase(std::remove(name_prefix.begin(), name_prefix.end(), ' '), name_prefix.end());
    this->name = name_prefix + "." + this->method_name;
  }
}

void FunctionData::assign_resolved_genericTs(const GenericsDeclaration* genericTs) {
  if (this->substitutedTs == nullptr) {
    this->genericTs = genericTs;
  }
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

void GlobalConstData::assign_inferred_type(TypePtr inferred_type) {
  this->inferred_type = inferred_type;
}

void GlobalConstData::assign_init_value(AnyExprV init_value) {
  this->init_value = init_value;
}

void LocalVarData::assign_ir_idx(std::vector<int>&& ir_idx) {
  this->ir_idx = std::move(ir_idx);
}

void LocalVarData::assign_resolved_type(TypePtr declared_type) {
  this->declared_type = declared_type;
}

void LocalVarData::assign_inferred_type(TypePtr inferred_type) {
  this->declared_type = inferred_type;
}

void LocalVarData::assign_default_value(AnyExprV default_value) {
  this->default_value = default_value;
}

void AliasDefData::assign_resolved_genericTs(const GenericsDeclaration* genericTs) {
  if (this->substitutedTs == nullptr) {
    this->genericTs = genericTs;
  }
}

void AliasDefData::assign_resolved_type(TypePtr underlying_type) {
  this->underlying_type = underlying_type;
}

void StructFieldData::assign_resolved_type(TypePtr declared_type) {
  this->declared_type = declared_type;
}

void StructFieldData::assign_default_value(AnyExprV default_value) {
  this->default_value = default_value;
}

void StructData::assign_resolved_genericTs(const GenericsDeclaration* genericTs) {
  if (this->substitutedTs == nullptr) {
    this->genericTs = genericTs;
  }
}

StructFieldPtr StructData::find_field(std::string_view field_name) const {
  for (StructFieldPtr field : fields) {
    if (field->name == field_name) {
      return field;
    }
  }
  return nullptr;
}

// formats opcode as "x{...}" or "b{...}"
std::string StructData::PackOpcode::format_as_slice() const {
  const int base = prefix_len % 4 == 0 ? 16 : 2;
  const int s_len = base == 16 ? prefix_len / 4 : prefix_len;
  const char* digits = "0123456789abcdef";

  std::string result(s_len + 3, '0');
  result[0] = base == 16 ? 'x' : 'b';
  result[1] = '{';
  result[s_len + 3 - 1] = '}';
  int64_t opcode = pack_prefix;
  for (int i = s_len - 1; i >= 0 && opcode != 0; --i) {
    result[2 + i] = digits[opcode % base];
    opcode /= base;
  }
  return result;
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

void GlobalSymbolTable::add_function(FunctionPtr f_sym) {
  auto key = key_hash(f_sym->name);
  auto [it, inserted] = entries.emplace(key, f_sym);
  if (!inserted) {
    fire_error_redefinition_of_symbol(f_sym->loc, it->second);
  }
}

void GlobalSymbolTable::add_global_var(GlobalVarPtr g_sym) {
  auto key = key_hash(g_sym->name);
  auto [it, inserted] = entries.emplace(key, g_sym);
  if (!inserted) {
    fire_error_redefinition_of_symbol(g_sym->loc, it->second);
  }
}

void GlobalSymbolTable::add_global_const(GlobalConstPtr c_sym) {
  auto key = key_hash(c_sym->name);
  auto [it, inserted] = entries.emplace(key, c_sym);
  if (!inserted) {
    fire_error_redefinition_of_symbol(c_sym->loc, it->second);
  }
}

void GlobalSymbolTable::add_type_alias(AliasDefPtr a_sym) {
  auto key = key_hash(a_sym->name);
  auto [it, inserted] = entries.emplace(key, a_sym);
  if (!inserted) {
    fire_error_redefinition_of_symbol(a_sym->loc, it->second);
  }
}

void GlobalSymbolTable::add_struct(StructPtr s_sym) {
  auto key = key_hash(s_sym->name);
  auto [it, inserted] = entries.emplace(key, s_sym);
  if (!inserted) {
    fire_error_redefinition_of_symbol(s_sym->loc, it->second);
  }
}

void GlobalSymbolTable::replace_function(FunctionPtr f_sym) {
  auto key = key_hash(f_sym->name);
  assert(entries.contains(key));
  entries[key] = f_sym;
}

const Symbol* lookup_global_symbol(std::string_view name) {
  return G.symtable.lookup(name);
}

FunctionPtr lookup_function(std::string_view name) {
  return G.symtable.lookup(name)->try_as<FunctionPtr>();
}

std::vector<FunctionPtr> lookup_methods_with_name(std::string_view name) {
  std::vector<FunctionPtr> result;
  for (FunctionPtr method_ref : G.all_methods) {
    if (method_ref->method_name == name) {
      result.push_back(method_ref);
    }
  }
  return result;
}

}  // namespace tolk
