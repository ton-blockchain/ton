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

  template<class ConstTPtr>
  ConstTPtr try_as() const {
#ifdef TOLK_DEBUG
    assert(this != nullptr);
#endif
    return dynamic_cast<ConstTPtr>(this);
  }
};

struct LocalVarData final : Symbol {
  enum {
    flagMutateParameter = 1,    // parameter was declared with `mutate` keyword
    flagImmutable = 2,          // variable was declared via `val` (not `var`)
  };

  AnyTypeV type_node;               // either at declaration `var x:int`, or if omitted, from assigned value `var x=2`
  TypePtr declared_type = nullptr;  // = resolved type_node
  int flags;
  int param_idx;                    // 0...N for function parameters, -1 for local vars
  std::vector<int> ir_idx;

  LocalVarData(std::string name, SrcLocation loc, AnyTypeV type_node, int flags, int param_idx)
    : Symbol(std::move(name), loc)
    , type_node(type_node)
    , flags(flags)
    , param_idx(param_idx) {
  }
  LocalVarData(std::string name, SrcLocation loc, TypePtr declared_type, int flags, int param_idx)
    : Symbol(std::move(name), loc)
    , type_node(nullptr)         // for built-in functions (their parameters)
    , declared_type(declared_type)
    , flags(flags)
    , param_idx(param_idx) {
  }

  bool is_parameter() const { return param_idx >= 0; }

  bool is_immutable() const { return flags & flagImmutable; }
  bool is_mutate_parameter() const { return flags & flagMutateParameter; }

  LocalVarData* mutate() const { return const_cast<LocalVarData*>(this); }
  void assign_ir_idx(std::vector<int>&& ir_idx);
  void assign_resolved_type(TypePtr declared_type);
  void assign_inferred_type(TypePtr inferred_type);
};

struct FunctionBodyCode;
struct FunctionBodyAsm;
struct FunctionBodyBuiltin;
struct GenericsDeclaration;

typedef std::variant<
  FunctionBodyCode*,
  FunctionBodyAsm*,
  FunctionBodyBuiltin*
> FunctionBody;

struct FunctionData final : Symbol {
  static constexpr int EMPTY_TVM_METHOD_ID = -10;

  enum {
    flagInline = 1,             // marked `@inline`
    flagInlineRef = 2,          // marked `@inline_ref`
    flagTypeInferringDone = 4,  // type inferring step of function's body (all AST nodes assigning v->inferred_type) is done
    flagUsedAsNonCall = 8,      // used not only as `f()`, but as a 1-st class function (assigned to var, pushed to tuple, etc.)
    flagMarkedAsPure = 16,      // declared as `pure`, can't call impure and access globals, unused invocations are optimized out
    flagImplicitReturn = 32,    // control flow reaches end of function, so it needs implicit return at the end
    flagContractGetter = 64,    // was declared via `get func(): T`, tvm_method_id is auto-assigned
    flagIsEntrypoint = 128,     // it's `main` / `onExternalMessage` / etc.
    flagHasMutateParams = 256,  // has parameters declared as `mutate`
    flagAcceptsSelf = 512,      // is a member function (has `self` first parameter)
    flagReturnsSelf = 1024,     // return type is `self` (returns the mutated 1st argument), calls can be chainable
    flagReallyUsed = 2048,      // calculated via dfs from used functions; declared but unused functions are not codegenerated
    flagCompileTimeOnly = 4096, // calculated only at compile-time for constant arguments: `ton("0.05")`, `stringCrc32`, and others
  };

  int tvm_method_id = EMPTY_TVM_METHOD_ID;
  int flags;

  std::string method_name;                    // for `fun Container<T>.store<U>` here is "store"
  AnyTypeV receiver_type_node;                // for `fun Container<T>.store<U>` here is `Container<T>`
  TypePtr receiver_type = nullptr;            // = resolved receiver_type_node

  std::vector<LocalVarData> parameters;
  std::vector<int> arg_order, ret_order;
  AnyTypeV return_type_node;                  // may be nullptr, meaning "auto infer"
  TypePtr declared_return_type = nullptr;     // = resolved return_type_node
  TypePtr inferred_return_type = nullptr;     // assigned on type inferring
  TypePtr inferred_full_type = nullptr;       // assigned on type inferring, it's TypeDataFunCallable(params -> return)

  const GenericsDeclaration* genericTs;
  const GenericsSubstitutions* substitutedTs;
  FunctionPtr base_fun_ref = nullptr;             // for `f<int>`, here is `f<T>`
  FunctionBody body;
  AnyV ast_root;                                  // V<ast_function_declaration> for user-defined (not builtin)

  FunctionData(std::string name, SrcLocation loc, std::string method_name, AnyTypeV receiver_type_node, AnyTypeV return_type_node, std::vector<LocalVarData> parameters, int initial_flags, const GenericsDeclaration* genericTs, const GenericsSubstitutions* substitutedTs, FunctionBody body, AnyV ast_root)
    : Symbol(std::move(name), loc)
    , flags(initial_flags)
    , method_name(std::move(method_name))
    , receiver_type_node(receiver_type_node)
    , parameters(std::move(parameters))
    , return_type_node(return_type_node)
    , genericTs(genericTs)
    , substitutedTs(substitutedTs)
    , body(body)
    , ast_root(ast_root) {
  }
  FunctionData(std::string name, SrcLocation loc, std::string method_name, TypePtr receiver_type, TypePtr declared_return_type, std::vector<LocalVarData> parameters, int initial_flags, const GenericsDeclaration* genericTs, const GenericsSubstitutions* substitutedTs, FunctionBody body, AnyV ast_root)
    : Symbol(std::move(name), loc)
    , flags(initial_flags)
    , method_name(std::move(method_name))
    , receiver_type_node(nullptr)
    , receiver_type(receiver_type)
    , parameters(std::move(parameters))
    , return_type_node(nullptr)            // for built-in functions, defined in sources
    , declared_return_type(declared_return_type)
    , genericTs(genericTs)
    , substitutedTs(substitutedTs)
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
  bool is_method() const { return !method_name.empty(); }
  bool is_static_method() const { return is_method() && !does_accept_self(); }

  bool is_generic_function() const { return genericTs != nullptr; }
  bool is_instantiation_of_generic_function() const { return substitutedTs != nullptr; }

  bool is_inline() const { return flags & flagInline; }
  bool is_inline_ref() const { return flags & flagInlineRef; }
  bool is_type_inferring_done() const { return flags & flagTypeInferringDone; }
  bool is_used_as_noncall() const { return flags & flagUsedAsNonCall; }
  bool is_marked_as_pure() const { return flags & flagMarkedAsPure; }
  bool is_implicit_return() const { return flags & flagImplicitReturn; }
  bool is_contract_getter() const { return flags & flagContractGetter; }
  bool has_tvm_method_id() const { return tvm_method_id != EMPTY_TVM_METHOD_ID; }
  bool is_entrypoint() const { return flags & flagIsEntrypoint; }
  bool has_mutate_params() const { return flags & flagHasMutateParams; }
  bool does_accept_self() const { return flags & flagAcceptsSelf; }
  bool does_return_self() const { return flags & flagReturnsSelf; }
  bool does_mutate_self() const { return (flags & flagAcceptsSelf) && parameters[0].is_mutate_parameter(); }
  bool is_really_used() const { return flags & flagReallyUsed; }
  bool is_compile_time_only() const { return flags & flagCompileTimeOnly; }

  bool does_need_codegen() const;

  FunctionData* mutate() const { return const_cast<FunctionData*>(this); }
  void assign_resolved_receiver_type(TypePtr receiver_type, std::string&& name_prefix);
  void assign_resolved_genericTs(const GenericsDeclaration* genericTs);
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

  AnyTypeV type_node;                 // `global a: int;` always exists, declaring globals without type is prohibited
  TypePtr declared_type = nullptr;    // = resolved type_node
  int flags = 0;

  GlobalVarData(std::string name, SrcLocation loc, AnyTypeV type_node)
    : Symbol(std::move(name), loc)
    , type_node(type_node) {
  }

  bool is_really_used() const { return flags & flagReallyUsed; }

  GlobalVarData* mutate() const { return const_cast<GlobalVarData*>(this); }
  void assign_resolved_type(TypePtr declared_type);
  void assign_is_really_used();
};

struct GlobalConstData final : Symbol {
  AnyTypeV type_node;                 // exists for `const op: int = rhs`, otherwise nullptr
  TypePtr declared_type = nullptr;    // = resolved type_node
  TypePtr inferred_type = nullptr;
  AnyExprV init_value;

  GlobalConstData(std::string name, SrcLocation loc, AnyTypeV type_node, AnyExprV init_value)
    : Symbol(std::move(name), loc)
    , type_node(type_node)
    , init_value(init_value) {
  }

  GlobalConstData* mutate() const { return const_cast<GlobalConstData*>(this); }
  void assign_resolved_type(TypePtr declared_type);
  void assign_inferred_type(TypePtr inferred_type);
  void assign_init_value(AnyExprV init_value);
};

struct AliasDefData final : Symbol {
  enum {
    flagVisitedByResolver = 1,
  };

  AnyTypeV underlying_type_node;
  TypePtr underlying_type = nullptr;    // = resolved underlying_type_node
  int flags = 0;

  const GenericsDeclaration* genericTs;
  const GenericsSubstitutions* substitutedTs;
  AliasDefPtr base_alias_ref = nullptr;           // for `Response<int>`, here is `Response<T>`
  AnyV ast_root;                                  // V<ast_type_alias_declaration>

  AliasDefData(std::string name, SrcLocation loc, AnyTypeV underlying_type_node, const GenericsDeclaration* genericTs, const GenericsSubstitutions* substitutedTs, AnyV ast_root)
    : Symbol(std::move(name), loc)
    , underlying_type_node(underlying_type_node)
    , genericTs(genericTs)
    , substitutedTs(substitutedTs)
    , ast_root(ast_root) {
  }

  std::string as_human_readable() const;

  bool is_generic_alias() const { return genericTs != nullptr; }
  bool is_instantiation_of_generic_alias() const { return substitutedTs != nullptr; }

  bool was_visited_by_resolver() const { return flags & flagVisitedByResolver; }

  AliasDefData* mutate() const { return const_cast<AliasDefData*>(this); }
  void assign_visited_by_resolver();
  void assign_resolved_genericTs(const GenericsDeclaration* genericTs);
  void assign_resolved_type(TypePtr underlying_type);
};

struct StructFieldData final : Symbol {
  int field_idx;
  AnyTypeV type_node;
  TypePtr declared_type = nullptr;      // = resolved type_node
  AnyExprV default_value;               // nullptr if no default

  bool has_default_value() const { return default_value != nullptr; }

  StructFieldData* mutate() const { return const_cast<StructFieldData*>(this); }
  void assign_resolved_type(TypePtr declared_type);
  void assign_default_value(AnyExprV default_value);

  StructFieldData(std::string name, SrcLocation loc, int field_idx, AnyTypeV type_node, AnyExprV default_value)
    : Symbol(std::move(name), loc)
    , field_idx(field_idx)
    , type_node(type_node)
    , default_value(default_value) {
  }
};

struct StructData final : Symbol {
  enum {
    flagVisitedByResolver = 1,
  };

  std::vector<StructFieldPtr> fields;
  int flags = 0;

  const GenericsDeclaration* genericTs;
  const GenericsSubstitutions* substitutedTs;
  StructPtr base_struct_ref = nullptr;            // for `Container<int>`, here is `Container<T>`
  AnyV ast_root;                                  // V<ast_struct_declaration>

  int get_num_fields() const { return static_cast<int>(fields.size()); }
  StructFieldPtr get_field(int i) const { return fields.at(i); }
  StructFieldPtr find_field(std::string_view field_name) const;

  bool is_generic_struct() const { return genericTs != nullptr; }
  bool is_instantiation_of_generic_struct() const { return substitutedTs != nullptr; }

  bool was_visited_by_resolver() const { return flags & flagVisitedByResolver; }

  StructData* mutate() const { return const_cast<StructData*>(this); }
  void assign_visited_by_resolver();
  void assign_resolved_genericTs(const GenericsDeclaration* genericTs);

  StructData(std::string name, SrcLocation loc, std::vector<StructFieldPtr>&& fields, const GenericsDeclaration* genericTs, const GenericsSubstitutions* substitutedTs, AnyV ast_root)
    : Symbol(std::move(name), loc)
    , fields(std::move(fields))
    , genericTs(genericTs)
    , substitutedTs(substitutedTs)
    , ast_root(ast_root) {
  }

  std::string as_human_readable() const;
};

struct TypeReferenceUsedAsSymbol final : Symbol {
  TypePtr resolved_type;

  TypeReferenceUsedAsSymbol(std::string name, SrcLocation loc, TypePtr resolved_type)
    : Symbol(std::move(name), loc)
    , resolved_type(resolved_type) {
  }
};

class GlobalSymbolTable {
  std::unordered_map<uint64_t, const Symbol*> entries;

  static uint64_t key_hash(std::string_view name_key) {
    return std::hash<std::string_view>{}(name_key);
  }

public:
  void add_function(FunctionPtr f_sym);
  void add_global_var(GlobalVarPtr g_sym);
  void add_global_const(GlobalConstPtr c_sym);
  void add_type_alias(AliasDefPtr a_sym);
  void add_struct(StructPtr s_sym);

  const Symbol* lookup(std::string_view name) const {
    const auto it = entries.find(key_hash(name));
    return it == entries.end() ? nullptr : it->second;
  }
};

const Symbol* lookup_global_symbol(std::string_view name);
FunctionPtr lookup_function(std::string_view name);

}  // namespace tolk
