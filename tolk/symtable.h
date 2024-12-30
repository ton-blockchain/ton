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
#include "fwd-declarations.h"
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

  TypePtr declared_type;            // either at declaration `var x:int`, or if omitted, from assigned value `var x=2`
  int flags;
  int idx;

  LocalVarData(std::string name, SrcLocation loc, TypePtr declared_type, int flags, int idx)
    : Symbol(std::move(name), loc)
    , declared_type(declared_type)
    , flags(flags)
    , idx(idx) {
  }

  bool is_immutable() const { return flags & flagImmutable; }
  bool is_mutate_parameter() const { return flags & flagMutateParameter; }

  LocalVarData* mutate() const { return const_cast<LocalVarData*>(this); }
  void assign_idx(int idx);
  void assign_resolved_type(TypePtr declared_type);
  void assign_inferred_type(TypePtr inferred_type);
};

struct FunctionBodyCode;
struct FunctionBodyAsm;
struct FunctionBodyBuiltin;
struct GenericsDeclaration;
struct GenericsInstantiation;

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
    flagTypeInferringDone = 4,  // type inferring step of function's body (all AST nodes assigning v->inferred_type) is done
    flagUsedAsNonCall = 8,      // used not only as `f()`, but as a 1-st class function (assigned to var, pushed to tuple, etc.)
    flagMarkedAsPure = 16,      // declared as `pure`, can't call impure and access globals, unused invocations are optimized out
    flagImplicitReturn = 32,    // control flow reaches end of function, so it needs implicit return at the end
    flagGetMethod = 64,         // was declared via `get func(): T`, method_id is auto-assigned
    flagIsEntrypoint = 128,     // it's `main` / `onExternalMessage` / etc.
    flagHasMutateParams = 256,  // has parameters declared as `mutate`
    flagAcceptsSelf = 512,      // is a member function (has `self` first parameter)
    flagReturnsSelf = 1024,     // return type is `self` (returns the mutated 1st argument), calls can be chainable
    flagReallyUsed = 2048,      // calculated via dfs from used functions; declared but unused functions are not codegenerated
  };

  int method_id = EMPTY_METHOD_ID;
  int flags;

  std::vector<LocalVarData> parameters;
  std::vector<int> arg_order, ret_order;
  TypePtr declared_return_type;               // may be nullptr, meaning "auto infer"
  TypePtr inferred_return_type = nullptr;     // assigned on type inferring
  TypePtr inferred_full_type = nullptr;       // assigned on type inferring, it's TypeDataFunCallable(params -> return)

  const GenericsDeclaration* genericTs;
  const GenericsInstantiation* instantiationTs;
  FunctionBody body;
  AnyV ast_root;                                            // V<ast_function_declaration> for user-defined (not builtin)

  FunctionData(std::string name, SrcLocation loc, TypePtr declared_return_type, std::vector<LocalVarData> parameters, int initial_flags, const GenericsDeclaration* genericTs, const GenericsInstantiation* instantiationTs, FunctionBody body, AnyV ast_root)
    : Symbol(std::move(name), loc)
    , flags(initial_flags)
    , parameters(std::move(parameters))
    , declared_return_type(declared_return_type)
    , genericTs(genericTs)
    , instantiationTs(instantiationTs)
    , body(body)
    , ast_root(ast_root) {
  }

  std::string as_human_readable() const;

  const std::vector<int>* get_arg_order() const {
    return arg_order.empty() ? nullptr : &arg_order;
  }
  const std::vector<int>* get_ret_order() const {
    return ret_order.empty() ? nullptr : &ret_order;
  }

  int get_num_params() const { return static_cast<int>(parameters.size()); }
  const LocalVarData& get_param(int idx) const { return parameters[idx]; }

  bool is_code_function() const { return std::holds_alternative<FunctionBodyCode*>(body); }
  bool is_asm_function() const { return std::holds_alternative<FunctionBodyAsm*>(body); }
  bool is_builtin_function() const { return ast_root == nullptr; }

  bool is_generic_function() const { return genericTs != nullptr; }
  bool is_instantiation_of_generic_function() const { return instantiationTs != nullptr; }

  bool is_inline() const { return flags & flagInline; }
  bool is_inline_ref() const { return flags & flagInlineRef; }
  bool is_type_inferring_done() const { return flags & flagTypeInferringDone; }
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
  bool is_really_used() const { return flags & flagReallyUsed; }

  bool does_need_codegen() const;

  FunctionData* mutate() const { return const_cast<FunctionData*>(this); }
  void assign_resolved_type(TypePtr declared_return_type);
  void assign_inferred_type(TypePtr inferred_return_type, TypePtr inferred_full_type);
  void assign_is_used_as_noncall();
  void assign_is_implicit_return();
  void assign_is_type_inferring_done();
  void assign_is_really_used();
  void assign_arg_order(std::vector<int>&& arg_order);
};

struct GlobalVarData final : Symbol {
  enum {
    flagReallyUsed = 1,          // calculated via dfs from used functions; unused globals are not codegenerated
  };

  TypePtr declared_type; // always exists, declaring globals without type is prohibited
  int flags = 0;

  GlobalVarData(std::string name, SrcLocation loc, TypePtr declared_type)
    : Symbol(std::move(name), loc)
    , declared_type(declared_type) {
  }

  bool is_really_used() const { return flags & flagReallyUsed; }

  GlobalVarData* mutate() const { return const_cast<GlobalVarData*>(this); }
  void assign_resolved_type(TypePtr declared_type);
  void assign_is_really_used();
};

struct GlobalConstData final : Symbol {
  ConstantValue value;
  TypePtr declared_type; // may be nullptr

  GlobalConstData(std::string name, SrcLocation loc, TypePtr declared_type, ConstantValue&& value)
    : Symbol(std::move(name), loc)
    , value(std::move(value))
    , declared_type(declared_type) {
  }

  bool is_int_const() const { return value.is_int(); }
  bool is_slice_const() const { return value.is_slice(); }

  td::RefInt256 as_int_const() const { return value.as_int(); }
  const std::string& as_slice_const() const { return value.as_slice(); }

  GlobalConstData* mutate() const { return const_cast<GlobalConstData*>(this); }
  void assign_resolved_type(TypePtr declared_type);
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
