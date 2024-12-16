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

#include "src-file.h"
#include "type-expr.h"
#include "constant-evaluator.h"
#include "crypto/common/refint.h"
#include <unordered_map>
#include <variant>
#include <vector>

namespace tolk {

struct Symbol {
  std::string name;
  SrcLocation loc;

  Symbol(std::string name, SrcLocation loc)
    : name(std::move(name))
    , loc(loc) {
  }

  virtual ~Symbol() = default;

  template<class T>
  const T* as() const {
#ifdef TOLK_DEBUG
    assert(dynamic_cast<const T*>(this) != nullptr);
#endif
    return dynamic_cast<const T*>(this);
  }

  template<class T>
  const T* try_as() const {
    return dynamic_cast<const T*>(this);
  }
};

struct LocalVarData final : Symbol {
  enum {
    flagMutateParameter = 1,    // parameter was declared with `mutate` keyword
    flagImmutable = 2,          // variable was declared via `val` (not `var`)
  };

  TypeExpr* declared_type;
  int flags = 0;
  int idx;

  LocalVarData(std::string name, SrcLocation loc, int idx, TypeExpr* declared_type)
    : Symbol(std::move(name), loc)
    , declared_type(declared_type)
    , idx(idx) {
  }

  bool is_underscore() const { return name.empty(); }
  bool is_immutable() const { return flags & flagImmutable; }
  bool is_mutate_parameter() const { return flags & flagMutateParameter; }

  LocalVarData* mutate() const { return const_cast<LocalVarData*>(this); }
  void assign_idx(int idx);
};

struct FunctionBodyCode;
struct FunctionBodyAsm;
struct FunctionBodyBuiltin;

typedef std::variant<
  FunctionBodyCode*,
  FunctionBodyAsm*,
  FunctionBodyBuiltin*
> FunctionBody;

struct FunctionData final : Symbol {
  static constexpr int EMPTY_METHOD_ID = -10;

  enum {
    flagInline = 1,             // marked `@inline`
    flagInlineRef = 2,          // marked `@inline_ref`
    flagReallyUsed = 4,         // calculated via dfs from used functions; declared but unused functions are not codegenerated
    flagUsedAsNonCall = 8,      // used not only as `f()`, but as a 1-st class function (assigned to var, pushed to tuple, etc.)
    flagMarkedAsPure = 16,      // declared as `pure`, can't call impure and access globals, unused invocations are optimized out
    flagImplicitReturn = 32,    // control flow reaches end of function, so it needs implicit return at the end
    flagGetMethod = 64,         // was declared via `get func(): T`, method_id is auto-assigned
    flagIsEntrypoint = 128,     // it's `main` / `onExternalMessage` / etc.
    flagHasMutateParams = 256,  // has parameters declared as `mutate`
    flagAcceptsSelf = 512,      // is a member function (has `self` first parameter)
    flagReturnsSelf = 1024,     // return type is `self` (returns the mutated 1st argument), calls can be chainable
  };

  int method_id = EMPTY_METHOD_ID;
  int flags;
  TypeExpr* full_type;    // currently, TypeExpr::_Map, probably wrapped with forall

  std::vector<LocalVarData> parameters;
  std::vector<int> arg_order, ret_order;

  FunctionBody body;

  FunctionData(std::string name, SrcLocation loc, TypeExpr* full_type, std::vector<LocalVarData> parameters, int initial_flags, FunctionBody body)
    : Symbol(std::move(name), loc)
    , flags(initial_flags)
    , full_type(full_type)
    , parameters(std::move(parameters))
    , body(body) {
  }

  const std::vector<int>* get_arg_order() const {
    return arg_order.empty() ? nullptr : &arg_order;
  }
  const std::vector<int>* get_ret_order() const {
    return ret_order.empty() ? nullptr : &ret_order;
  }

  bool is_regular_function() const { return std::holds_alternative<FunctionBodyCode*>(body); }
  bool is_asm_function() const { return std::holds_alternative<FunctionBodyAsm*>(body); }
  bool is_builtin_function() const { return std::holds_alternative<FunctionBodyBuiltin*>(body); }

  bool is_inline() const { return flags & flagInline; }
  bool is_inline_ref() const { return flags & flagInlineRef; }
  bool is_really_used() const { return flags & flagReallyUsed; }
  bool is_used_as_noncall() const { return flags & flagUsedAsNonCall; }
  bool is_marked_as_pure() const { return flags & flagMarkedAsPure; }
  bool is_implicit_return() const { return flags & flagImplicitReturn; }
  bool is_get_method() const { return flags & flagGetMethod; }
  bool is_method_id_not_empty() const { return method_id != EMPTY_METHOD_ID; }
  bool is_entrypoint() const { return flags & flagIsEntrypoint; }
  bool has_mutate_params() const { return flags & flagHasMutateParams; }
  bool does_accept_self() const { return flags & flagAcceptsSelf; }
  bool does_return_self() const { return flags & flagReturnsSelf; }
  bool does_mutate_self() const { return (flags & flagAcceptsSelf) && parameters[0].is_mutate_parameter(); }

  bool does_need_codegen() const;

  FunctionData* mutate() const { return const_cast<FunctionData*>(this); }
  void assign_is_really_used();
  void assign_is_used_as_noncall();
  void assign_is_implicit_return();
};

struct GlobalVarData final : Symbol {
  enum {
    flagReallyUsed = 1,          // calculated via dfs from used functions; unused globals are not codegenerated
  };

  TypeExpr* declared_type;
  int flags = 0;

  GlobalVarData(std::string name, SrcLocation loc, TypeExpr* declared_type)
    : Symbol(std::move(name), loc)
    , declared_type(declared_type) {
  }

  bool is_really_used() const { return flags & flagReallyUsed; }

  GlobalVarData* mutate() const { return const_cast<GlobalVarData*>(this); }
  void assign_is_really_used();
};

struct GlobalConstData final : Symbol {
  ConstantValue value;
  TypeExpr* inferred_type;

  GlobalConstData(std::string name, SrcLocation loc, ConstantValue&& value)
    : Symbol(std::move(name), loc)
    , value(std::move(value))
    , inferred_type(TypeExpr::new_atomic(this->value.is_int() ? TypeExpr::_Int : TypeExpr::_Slice)) {
  }

  bool is_int_const() const { return value.is_int(); }
  bool is_slice_const() const { return value.is_slice(); }

  td::RefInt256 as_int_const() const { return value.as_int(); }
  const std::string& as_slice_const() const { return value.as_slice(); }
};

class GlobalSymbolTable {
  std::unordered_map<uint64_t, const Symbol*> entries;

  static uint64_t key_hash(std::string_view name_key) {
    return std::hash<std::string_view>{}(name_key);
  }

public:
  void add_function(const FunctionData* f_sym);
  void add_global_var(const GlobalVarData* g_sym);
  void add_global_const(const GlobalConstData* c_sym);

  const Symbol* lookup(std::string_view name) const {
    const auto it = entries.find(key_hash(name));
    return it == entries.end() ? nullptr : it->second;
  }
};

const Symbol* lookup_global_symbol(std::string_view name);

}  // namespace tolk
