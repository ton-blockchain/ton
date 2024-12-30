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
#include "generics-helpers.h"
#include "tolk.h"
#include "ast.h"
#include "ast-replicator.h"
#include "type-system.h"
#include "compiler-state.h"
#include "pipeline.h"

namespace tolk {

// given orig = "(int, T)" and substitutions = [slice], return "(int, slice)"
static TypePtr replace_genericT_with_deduced(TypePtr orig, const GenericsDeclaration* genericTs, const std::vector<TypePtr>& substitutionTs) {
  if (!orig || !orig->has_genericT_inside()) {
    return orig;
  }
  tolk_assert(genericTs->size() == substitutionTs.size());

  return orig->replace_children_custom([genericTs, substitutionTs](TypePtr child) {
    if (const TypeDataGenericT* asT = child->try_as<TypeDataGenericT>()) {
      int idx = genericTs->find_nameT(asT->nameT);
      if (idx == -1) {
        throw Fatal("can not replace generic " + asT->nameT);
      }
      return substitutionTs[idx];
    }
    return child;
  });
}

// purpose: having `f<T>(value: T)` and call `f(5)`, deduce T = int
// generally, there may be many generic Ts for declaration, and many arguments
// for every argument, `consider_next_condition()` is called
// example: `f<T1, T2>(a: int, b: T1, c: (T1, T2))` and call `f(6, 7, (8, cs))`
// - `a` does not affect, it doesn't depend on generic Ts
// - next condition: param_type = `T1`, arg_type = `int`, deduce T1 = int
// - next condition: param_type = `(T1, T2)`, arg_type = `(int, slice)`, deduce T1 = int, T2 = slice
// for call `f(6, cs, (8, cs))` T1 will be both `slice` and `int`, fired an error
class GenericSubstitutionsDeduceForFunctionCall final {
  const FunctionData* fun_ref;
  std::vector<TypePtr> substitutions;

  void provideDeducedT(const std::string& nameT, TypePtr deduced) {
    if (deduced == TypeDataNullLiteral::create() || deduced->has_unknown_inside()) {
      return;   // just 'null' doesn't give sensible info
    }

    int idx = fun_ref->genericTs->find_nameT(nameT);
    if (substitutions[idx] == nullptr) {
      substitutions[idx] = deduced;
    } else if (substitutions[idx] != deduced) {
      throw std::runtime_error(nameT + " is both " + substitutions[idx]->as_human_readable() + " and " + deduced->as_human_readable());
    }
  }

public:
  explicit GenericSubstitutionsDeduceForFunctionCall(const FunctionData* fun_ref)
    : fun_ref(fun_ref) {
    substitutions.resize(fun_ref->genericTs->size());  // filled with nullptr (nothing deduced)
  }

  void consider_next_condition(TypePtr param_type, TypePtr arg_type) {
    if (const auto* asT = param_type->try_as<TypeDataGenericT>()) {
      // `(arg: T)` called as `f([1, 2])` => T is [int, int]
      provideDeducedT(asT->nameT, arg_type);
    } else if (const auto* p_tensor = param_type->try_as<TypeDataTensor>()) {
      // `arg: (int, T)` called as `f((5, cs))` => T is slice
      if (const auto* a_tensor = arg_type->try_as<TypeDataTensor>(); a_tensor && a_tensor->size() == p_tensor->size()) {
        for (int i = 0; i < a_tensor->size(); ++i) {
          consider_next_condition(p_tensor->items[i], a_tensor->items[i]);
        }
      }
    } else if (const auto* p_tuple = param_type->try_as<TypeDataTypedTuple>()) {
      // `arg: [int, T]` called as `f([5, cs])` => T is slice
      if (const auto* a_tuple = arg_type->try_as<TypeDataTypedTuple>(); a_tuple && a_tuple->size() == p_tuple->size()) {
        for (int i = 0; i < a_tuple->size(); ++i) {
          consider_next_condition(p_tuple->items[i], a_tuple->items[i]);
        }
      }
    } else if (const auto* p_callable = param_type->try_as<TypeDataFunCallable>()) {
      // `arg: fun(TArg) -> TResult` called as `f(calcTupleLen)` => TArg is tuple, TResult is int
      if (const auto* a_callable = arg_type->try_as<TypeDataFunCallable>(); a_callable && a_callable->params_size() == p_callable->params_size()) {
        for (int i = 0; i < a_callable->params_size(); ++i) {
          consider_next_condition(p_callable->params_types[i], a_callable->params_types[i]);
        }
        consider_next_condition(p_callable->return_type, a_callable->return_type);
      }
    }
  }

  int get_first_not_deduced_idx() const {
    for (int i = 0; i < static_cast<int>(substitutions.size()); ++i) {
      if (substitutions[i] == nullptr) {
        return i;
      }
    }
    return -1;
  }

  std::vector<TypePtr> flush() {
    return {std::move(substitutions)};
  }
};

// clone the body of `f<T>` replacing T everywhere with a substitution
// before: `fun f<T>(v: T) { var cp: [T] = [v]; }`
// after:  `fun f<int>(v: int) { var cp: [int] = [v]; }`
// an instantiated function becomes a deep copy, all AST nodes are copied, no previous pointers left
class GenericFunctionReplicator final : public ASTReplicatorFunction {
  const GenericsDeclaration* genericTs;
  const std::vector<TypePtr>& substitutionTs;

protected:
  using ASTReplicatorFunction::clone;

  TypePtr clone(TypePtr t) override {
    return replace_genericT_with_deduced(t, genericTs, substitutionTs);
  }

public:
  GenericFunctionReplicator(const GenericsDeclaration* genericTs, const std::vector<TypePtr>& substitutionTs)
    : genericTs(genericTs)
    , substitutionTs(substitutionTs) {
  }

  V<ast_function_declaration> clone_function_body(V<ast_function_declaration> v_function) override {
    return createV<ast_function_declaration>(
      v_function->loc,
      clone(v_function->get_identifier()),
      clone(v_function->get_param_list()),
      clone(v_function->get_body()),
      clone(v_function->declared_return_type),
      nullptr,    // a newly-created function is not generic
      v_function->method_id,
      v_function->flags
    );
  }
};

std::string GenericsDeclaration::as_human_readable() const {
  std::string result = "<";
  for (const GenericsItem& item : itemsT) {
    if (result.size() > 1) {
      result += ",";
    }
    result += item.nameT;
  }
  result += ">";
  return result;
}

int GenericsDeclaration::find_nameT(std::string_view nameT) const {
  for (int i = 0; i < static_cast<int>(itemsT.size()); ++i) {
    if (itemsT[i].nameT == nameT) {
      return i;
    }
  }
  return -1;
}

// after creating a deep copy of `f<T>` like `f<int>`, its new and fresh body needs the previous pipeline to run
// for example, all local vars need to be registered as symbols, etc.
static void run_pipeline_for_instantiated_function(const FunctionData* inst_fun_ref) {
  // these pipes are exactly the same as in tolk.cpp — all preceding (and including) type inferring
  pipeline_resolve_identifiers_and_assign_symbols(inst_fun_ref);
  pipeline_calculate_rvalue_lvalue(inst_fun_ref);
  pipeline_detect_unreachable_statements(inst_fun_ref);
  pipeline_infer_types_and_calls_and_fields(inst_fun_ref);
}

std::string generate_instantiated_name(const std::string& orig_name, const std::vector<TypePtr>& substitutions) {
  // an instantiated function name will be "{orig_name}<{T1,T2,...}>"
  std::string name = orig_name;
  name += "<";
  for (TypePtr subs : substitutions) {
    if (name.size() > orig_name.size() + 1) {
      name += ",";
    }
    name += subs->as_human_readable();
  }
  name.erase(std::remove(name.begin(), name.end(), ' '), name.end());
  name += ">";
  return name;
}

td::Result<std::vector<TypePtr>> deduce_substitutionTs_on_generic_func_call(const FunctionData* called_fun, std::vector<TypePtr>&& arg_types, TypePtr return_hint) {
  try {
    GenericSubstitutionsDeduceForFunctionCall deducing(called_fun);
    for (const LocalVarData& param : called_fun->parameters) {
      if (param.declared_type->has_genericT_inside() && param.idx < static_cast<int>(arg_types.size())) {
        deducing.consider_next_condition(param.declared_type, arg_types[param.idx]);
      }
    }
    int idx = deducing.get_first_not_deduced_idx();
    if (idx != -1 && return_hint && called_fun->declared_return_type->has_genericT_inside()) {
      deducing.consider_next_condition(called_fun->declared_return_type, return_hint);
      idx = deducing.get_first_not_deduced_idx();
    }
    if (idx != -1) {
      return td::Status::Error(td::Slice{"can not deduce " + called_fun->genericTs->get_nameT(idx)});
    }
    return deducing.flush();
  } catch (const std::runtime_error& ex) {
    return td::Status::Error(td::Slice{ex.what()});
  }
}

const FunctionData* instantiate_generic_function(SrcLocation loc, const FunctionData* fun_ref, const std::string& inst_name, std::vector<TypePtr>&& substitutionTs) {
  tolk_assert(fun_ref->genericTs);

  // if `f<int>` was earlier instantiated, return it
  if (const auto* existing = lookup_global_symbol(inst_name)) {
    const FunctionData* inst_ref = existing->try_as<FunctionData>();
    tolk_assert(inst_ref);
    return inst_ref;
  }

  std::vector<LocalVarData> parameters;
  parameters.reserve(fun_ref->get_num_params());
  for (const LocalVarData& orig_p : fun_ref->parameters) {
    parameters.emplace_back(orig_p.name, orig_p.loc, replace_genericT_with_deduced(orig_p.declared_type, fun_ref->genericTs, substitutionTs), orig_p.flags, orig_p.idx);
  }
  TypePtr declared_return_type = replace_genericT_with_deduced(fun_ref->declared_return_type, fun_ref->genericTs, substitutionTs);
  const GenericsInstantiation* instantiationTs = new GenericsInstantiation(loc, std::move(substitutionTs));

  if (fun_ref->is_asm_function()) {
    FunctionData* inst_ref = new FunctionData(inst_name, fun_ref->loc, declared_return_type, std::move(parameters), fun_ref->flags, nullptr, instantiationTs, new FunctionBodyAsm, fun_ref->ast_root);
    inst_ref->arg_order = fun_ref->arg_order;
    inst_ref->ret_order = fun_ref->ret_order;
    G.symtable.add_function(inst_ref);
    G.all_functions.push_back(inst_ref);
    run_pipeline_for_instantiated_function(inst_ref);
    return inst_ref;
  }

  if (fun_ref->is_builtin_function()) {
    FunctionData* inst_ref = new FunctionData(inst_name, fun_ref->loc, declared_return_type, std::move(parameters), fun_ref->flags, nullptr, instantiationTs, fun_ref->body, fun_ref->ast_root);
    inst_ref->arg_order = fun_ref->arg_order;
    inst_ref->ret_order = fun_ref->ret_order;
    G.symtable.add_function(inst_ref);
    return inst_ref;
  }

  GenericFunctionReplicator replicator(fun_ref->genericTs, instantiationTs->substitutions);
  V<ast_function_declaration> inst_root = replicator.clone_function_body(fun_ref->ast_root->as<ast_function_declaration>());

  FunctionData* inst_ref = new FunctionData(inst_name, fun_ref->loc, declared_return_type, std::move(parameters), fun_ref->flags, nullptr, instantiationTs, new FunctionBodyCode, inst_root);
  inst_ref->arg_order = fun_ref->arg_order;
  inst_ref->ret_order = fun_ref->ret_order;
  inst_root->mutate()->assign_fun_ref(inst_ref);
  G.symtable.add_function(inst_ref);
  G.all_functions.push_back(inst_ref);
  run_pipeline_for_instantiated_function(inst_ref);
  return inst_ref;
}

} // namespace tolk
