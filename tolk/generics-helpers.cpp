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
      if (substitutionTs[idx] == nullptr) {
        throw GenericDeduceError("can not deduce " + asT->nameT);
      }
      return substitutionTs[idx];
    }
    return child;
  });
}

GenericSubstitutionsDeduceForCall::GenericSubstitutionsDeduceForCall(FunctionPtr fun_ref)
  : fun_ref(fun_ref) {
  substitutionTs.resize(fun_ref->genericTs->size());  // filled with nullptr (nothing deduced)
}

void GenericSubstitutionsDeduceForCall::provide_deducedT(const std::string& nameT, TypePtr deduced) {
  if (deduced == TypeDataNullLiteral::create() || deduced->has_unknown_inside()) {
    return;  // just 'null' doesn't give sensible info
  }

  int idx = fun_ref->genericTs->find_nameT(nameT);
  if (substitutionTs[idx] == nullptr) {
    substitutionTs[idx] = deduced;
  } else if (substitutionTs[idx] != deduced) {
    throw GenericDeduceError(nameT + " is both " + substitutionTs[idx]->as_human_readable() + " and " + deduced->as_human_readable());
  }
}

void GenericSubstitutionsDeduceForCall::provide_manually_specified(std::vector<TypePtr>&& substitutionTs) {
  this->substitutionTs = std::move(substitutionTs);
  this->manually_specified = true;
}

// purpose: having `f<T>(value: T)` and call `f(5)`, deduce T = int
// generally, there may be many generic Ts for declaration, and many arguments
// for every argument, `consider_next_condition()` is called
// example: `f<T1, T2>(a: int, b: T1, c: (T1, T2))` and call `f(6, 7, (8, cs))`
// - `a` does not affect, it doesn't depend on generic Ts
// - next condition: param_type = `T1`, arg_type = `int`, deduce T1 = int
// - next condition: param_type = `(T1, T2)`, arg_type = `(int, slice)`, deduce T1 = int, T2 = slice
// for call `f(6, cs, (8, cs))` T1 will be both `slice` and `int`, fired an error
void GenericSubstitutionsDeduceForCall::consider_next_condition(TypePtr param_type, TypePtr arg_type) {
  if (const auto* asT = param_type->try_as<TypeDataGenericT>()) {
    // `(arg: T)` called as `f([1, 2])` => T is [int, int]
    provide_deducedT(asT->nameT, arg_type);
  } else if (const auto* p_nullable = param_type->try_as<TypeDataUnion>(); p_nullable && p_nullable->or_null) {
    // `arg: T?` called as `f(nullableInt)` => T is int
    if (const auto* a_nullable = arg_type->unwrap_alias()->try_as<TypeDataUnion>(); a_nullable && a_nullable->or_null) {
      consider_next_condition(p_nullable->or_null, a_nullable->or_null);
    }
    // `arg: T?` called as `f(int)` => T is int
    else {
      consider_next_condition(p_nullable->or_null, arg_type);
    }
  } else if (const auto* p_tensor = param_type->try_as<TypeDataTensor>()) {
    // `arg: (int, T)` called as `f((5, cs))` => T is slice
    if (const auto* a_tensor = arg_type->unwrap_alias()->try_as<TypeDataTensor>(); a_tensor && a_tensor->size() == p_tensor->size()) {
      for (int i = 0; i < a_tensor->size(); ++i) {
        consider_next_condition(p_tensor->items[i], a_tensor->items[i]);
      }
    }
  } else if (const auto* p_tuple = param_type->try_as<TypeDataBrackets>()) {
    // `arg: [int, T]` called as `f([5, cs])` => T is slice
    if (const auto* a_tuple = arg_type->unwrap_alias()->try_as<TypeDataBrackets>(); a_tuple && a_tuple->size() == p_tuple->size()) {
      for (int i = 0; i < a_tuple->size(); ++i) {
        consider_next_condition(p_tuple->items[i], a_tuple->items[i]);
      }
    }
  } else if (const auto* p_callable = param_type->try_as<TypeDataFunCallable>()) {
    // `arg: fun(TArg) -> TResult` called as `f(calcTupleLen)` => TArg is tuple, TResult is int
    if (const auto* a_callable = arg_type->unwrap_alias()->try_as<TypeDataFunCallable>(); a_callable && a_callable->params_size() == p_callable->params_size()) {
      for (int i = 0; i < a_callable->params_size(); ++i) {
        consider_next_condition(p_callable->params_types[i], a_callable->params_types[i]);
      }
      consider_next_condition(p_callable->return_type, a_callable->return_type);
    }
  } else if (const auto* p_union = param_type->try_as<TypeDataUnion>()) {
    // `arg: T1 | T2` called as `f(intOrBuilder)` => T1 is int, T2 is builder
    if (const auto* a_union = arg_type->unwrap_alias()->try_as<TypeDataUnion>(); a_union && a_union->variants.size() == p_union->variants.size()) {
      for (int i = 0; i < static_cast<int>(p_union->variants.size()); ++i) {
        consider_next_condition(p_union->variants[i], a_union->variants[i]);
      }
    }
  }
}

TypePtr GenericSubstitutionsDeduceForCall::replace_by_manually_specified(TypePtr param_type) const {
  return replace_genericT_with_deduced(param_type, fun_ref->genericTs, substitutionTs);
}

TypePtr GenericSubstitutionsDeduceForCall::auto_deduce_from_argument(FunctionPtr cur_f, SrcLocation loc, TypePtr param_type, TypePtr arg_type) {
  try {
    if (!manually_specified) {
      consider_next_condition(param_type, arg_type);
    }
    return replace_genericT_with_deduced(param_type, fun_ref->genericTs, substitutionTs);
  } catch (const GenericDeduceError& ex) {
    throw ParseError(cur_f, loc, ex.message + " for generic function `" + fun_ref->as_human_readable() + "`; instantiate it manually with `" + fun_ref->name + "<...>()`");
  }
}

int GenericSubstitutionsDeduceForCall::get_first_not_deduced_idx() const {
  for (int i = 0; i < static_cast<int>(substitutionTs.size()); ++i) {
    if (substitutionTs[i] == nullptr) {
      return i;
    }
  }
  return -1;
}

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

// when cloning `f<T>`, original name is "f", we need a new name for symtable and output
// name of an instantiated function will be "f<int>" and similar (yes, with "<" symbol, it's okay to Fift)
static std::string generate_instantiated_name(const std::string& orig_name, const std::vector<TypePtr>& substitutions) {
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

FunctionPtr instantiate_generic_function(SrcLocation loc, FunctionPtr fun_ref, std::vector<TypePtr>&& substitutionTs) {
  tolk_assert(fun_ref->is_generic_function() && !fun_ref->is_method_id_not_empty());

  // fun_ref->name = "f", inst_name will be "f<int>" and similar
  std::string new_name = generate_instantiated_name(fun_ref->name, substitutionTs);
  if (const Symbol* existing_sym = lookup_global_symbol(new_name)) {
    FunctionPtr existing_ref = existing_sym->try_as<FunctionPtr>();
    tolk_assert(existing_ref);
    return existing_ref;
  }

  const GenericsInstantiation* instantiationTs = new GenericsInstantiation(loc, std::move(substitutionTs));
  ASTReplicatorFunction replicator;

  // built-in functions don't have AST to clone, types of parameters don't exist in AST, etc.
  // nevertheless, for outer code to follow a single algorithm,
  // when calling `debugPrint(x)`, we clone it as "debugPrint<int>", replace types, and insert into symtable
  if (fun_ref->is_builtin_function()) {
    std::vector<LocalVarData> new_parameters;
    new_parameters.reserve(fun_ref->get_num_params());
    for (const LocalVarData& orig_p : fun_ref->parameters) {
      TypePtr new_param_type = replace_genericT_with_deduced(orig_p.declared_type, fun_ref->genericTs, instantiationTs->substitutions);
      new_parameters.emplace_back(orig_p.name, orig_p.loc, new_param_type, orig_p.flags, orig_p.param_idx);
    }
    TypePtr new_return_type = replace_genericT_with_deduced(fun_ref->declared_return_type, fun_ref->genericTs, instantiationTs->substitutions);
    FunctionData* new_fun_ref = new FunctionData(new_name, fun_ref->loc, new_return_type, std::move(new_parameters), fun_ref->flags, nullptr, instantiationTs, fun_ref->body, fun_ref->ast_root);
    new_fun_ref->arg_order = fun_ref->arg_order;
    new_fun_ref->ret_order = fun_ref->ret_order;
    G.symtable.add_function(new_fun_ref);
    return new_fun_ref;
  }

  // for `f<T>` (both asm and regular), create "f<int>" with AST fully cloned
  // it means, that types still contain T: `f<int>(v: T): T`, but since type resolving knows
  // it's instantiation, when resolving types, it substitutes T=int
  V<ast_function_declaration> orig_root = fun_ref->ast_root->as<ast_function_declaration>();
  V<ast_identifier> new_name_ident = createV<ast_identifier>(orig_root->get_identifier()->loc, new_name);
  V<ast_function_declaration> new_root = replicator.clone_function_ast(orig_root, new_name_ident);

  FunctionPtr new_fun_ref = pipeline_register_instantiated_generic_function(new_root, instantiationTs);
  tolk_assert(new_fun_ref);
  // body of a cloned function (it's cloned at type inferring step) needs the previous pipeline to run
  // for example, all local vars need to be registered as symbols, etc.
  // these pipes are exactly the same as in tolk.cpp — all preceding (and including) type inferring
  pipeline_resolve_identifiers_and_assign_symbols(new_fun_ref);
  pipeline_resolve_types_and_aliases(new_fun_ref);
  pipeline_calculate_rvalue_lvalue(new_fun_ref);
  pipeline_infer_types_and_calls_and_fields(new_fun_ref);

  return new_fun_ref;
}

} // namespace tolk
