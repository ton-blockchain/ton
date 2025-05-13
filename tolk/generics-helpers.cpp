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

// given orig `(int, T)` and substitutions [slice], return `(int, slice)`
static TypePtr replace_genericT_with_deduced(TypePtr orig, const GenericsSubstitutions* substitutedTs, bool apply_defaultTs = false, std::string_view* out_unknownT = nullptr) {
  if (!orig || !orig->has_genericT_inside()) {
    return orig;
  }

  return orig->replace_children_custom([substitutedTs, apply_defaultTs, &out_unknownT](TypePtr child) {
    if (const TypeDataGenericT* asT = child->try_as<TypeDataGenericT>()) {
      TypePtr typeT = substitutedTs->get_substitution_for_nameT(asT->nameT);
      if (typeT == nullptr && apply_defaultTs) {
        typeT = substitutedTs->get_default_for_nameT(asT->nameT);
      }
      if (typeT == nullptr) {     // T was not deduced yet, leave T as generic
        typeT = child;
        if (out_unknownT && out_unknownT->empty()) {
          *out_unknownT = asT->nameT;
        }
      }
      return typeT;
    }
    if (const TypeDataGenericTypeWithTs* as_instTs = child->try_as<TypeDataGenericTypeWithTs>(); as_instTs && !as_instTs->has_genericT_inside()) {
      if (StructPtr struct_ref = as_instTs->struct_ref) {
        struct_ref = instantiate_generic_struct(struct_ref, GenericsSubstitutions(struct_ref->genericTs, as_instTs->type_arguments));
        return TypeDataStruct::create(struct_ref);
      }
      if (AliasDefPtr alias_ref = as_instTs->alias_ref) {
        alias_ref = instantiate_generic_alias(as_instTs->alias_ref, GenericsSubstitutions(alias_ref->genericTs, as_instTs->type_arguments));
        return TypeDataAlias::create(alias_ref);
      }
    }
    return child;
  });
}

GenericsSubstitutions::GenericsSubstitutions(const GenericsDeclaration* genericTs, const std::vector<TypePtr>& type_arguments)
  : genericTs(genericTs)
  , valuesTs(genericTs->size()) {
  provide_type_arguments(type_arguments);
}

std::string GenericsSubstitutions::as_human_readable(bool show_nullptr) const {
  std::string result;
  for (int i = 0; i < size(); ++i) {
    if (valuesTs[i] == nullptr && !show_nullptr) {
      continue;;
    }
    if (!result.empty()) {
      result += ", ";
    }
    result += genericTs->get_nameT(i);
    if (valuesTs[i] == nullptr) {
      result += "=nullptr";
    } else {
      result += "=`";
      result += valuesTs[i]->as_human_readable();
      result += "`";
    }
  }
  return result;
}

void GenericsSubstitutions::set_typeT(std::string_view nameT, TypePtr typeT) {
  for (int i = 0; i < size(); ++i) {
    if (genericTs->get_nameT(i) == nameT) {
      if (valuesTs[i] == nullptr) {
        tolk_assert(!typeT->has_genericT_inside());
        valuesTs[i] = typeT;
      }
      break;
    }
  }
}

void GenericsSubstitutions::provide_type_arguments(const std::vector<TypePtr>& type_arguments) {
  tolk_assert(genericTs != nullptr);
  int start_from = genericTs->n_from_receiver;    // for `Container<T>.wrap<U>` user should specify only U
  tolk_assert(static_cast<int>(type_arguments.size()) + start_from == genericTs->size());
  for (int i = start_from; i < genericTs->size(); ++i) {
    valuesTs[i] = type_arguments[i - start_from];
  }
}

void GenericsSubstitutions::rewrite_missing_with_defaults() {
  for (int i = 0; i < size(); ++i) {
    if (valuesTs[i] == nullptr) {
      valuesTs[i] = genericTs->get_defaultT(i);   // if no default, left nullptr
    }
  }
}

GenericSubstitutionsDeducing::GenericSubstitutionsDeducing(FunctionPtr fun_ref)
  : fun_ref(fun_ref)
  , struct_ref(nullptr)
  , deducedTs(fun_ref->genericTs) {
}

GenericSubstitutionsDeducing::GenericSubstitutionsDeducing(StructPtr struct_ref)
  : fun_ref(nullptr)
  , struct_ref(struct_ref)
  , deducedTs(struct_ref->genericTs) {
}

// purpose: having `f<T>(value: T)` and call `f(5)`, deduce T = int
// generally, there may be many generic Ts for declaration, and many arguments
// for every argument, `consider_next_condition()` is called
// example: `f<T1, T2>(a: int, b: T1, c: (T1, T2))` and call `f(6, 7, (8, cs))`
// - `a` does not affect, it doesn't depend on generic Ts
// - next condition: param_type = `T1`, arg_type = `int`, deduce T1 = int
// - next condition: param_type = `(T1, T2)` = `(int, T2)`, arg_type = `(int, slice)`, deduce T2 = slice
// for call `f(6, cs, (8, cs))` both T1 and T2 will become `slice`, firing a type mismatch error later
void GenericSubstitutionsDeducing::consider_next_condition(TypePtr param_type, TypePtr arg_type) {
  // all Ts deduced up to this point are apriori
  param_type = replace_genericT_with_deduced(param_type, &deducedTs);
  if (!param_type->has_genericT_inside()) {
    return;
  }

  if (const auto* asT = param_type->try_as<TypeDataGenericT>()) {
    // `(arg: T)` called as `f([1, 2])` => T is [int, int]
    deducedTs.set_typeT(asT->nameT, arg_type);
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
    // `arg: int | T1` called as `f(builderOrIntOrSlice)` => T1 is builder|slice
    if (const auto* a_union = arg_type->unwrap_alias()->try_as<TypeDataUnion>()) {
      std::vector<TypePtr> p_generic;
      std::vector<TypePtr> a_sub_p = a_union->variants;
      bool is_sub_correct = true;
      for (TypePtr p_variant : p_union->variants) {
        if (!p_variant->has_genericT_inside()) {
          auto it = std::find(a_sub_p.begin(), a_sub_p.end(), p_variant);
          if (it != a_sub_p.end()) {
            a_sub_p.erase(it);
          } else {
            is_sub_correct = false;
          }
        } else {
          p_generic.push_back(p_variant);
        }
      }
      if (is_sub_correct && p_generic.size() == 1 && a_sub_p.size() > 1) {
        consider_next_condition(p_generic[0], TypeDataUnion::create(std::move(a_sub_p)));
      } else if (is_sub_correct && p_generic.size() == a_sub_p.size()) {
        for (int i = 0; i < static_cast<int>(p_generic.size()); ++i) {
          consider_next_condition(p_generic[i], a_sub_p[i]);
        }
      }
    }
    // `arg: int | MyData<T>` called as `f(MyData<int>)` => T is int
    else {
      for (TypePtr p_variant : p_union->variants) {
        consider_next_condition(p_variant, arg_type);
      }
    }
  } else if (const auto* p_instSt = param_type->try_as<TypeDataGenericTypeWithTs>(); p_instSt && p_instSt->struct_ref) {
    // `arg: Wrapper<T>` called as `f(wrappedInt)` => T is int
    if (const auto* a_struct = arg_type->try_as<TypeDataStruct>(); a_struct && a_struct->struct_ref->is_instantiation_of_generic_struct() && a_struct->struct_ref->base_struct_ref == p_instSt->struct_ref) {
      tolk_assert(p_instSt->size() == a_struct->struct_ref->substitutedTs->size());
      for (int i = 0; i < p_instSt->size(); ++i) {
        consider_next_condition(p_instSt->type_arguments[i], a_struct->struct_ref->substitutedTs->typeT_at(i));
      }
    }
  } else if (const auto* p_instAl = param_type->try_as<TypeDataGenericTypeWithTs>(); p_instAl && p_instAl->alias_ref) {
    // `arg: WrapperAlias<T>` called as `f(wrappedInt)` => T is int
    if (const auto* a_alias = arg_type->try_as<TypeDataAlias>(); a_alias && a_alias->alias_ref->is_instantiation_of_generic_alias() && a_alias->alias_ref->base_alias_ref == p_instAl->alias_ref) {
      tolk_assert(p_instAl->size() == a_alias->alias_ref->substitutedTs->size());
      for (int i = 0; i < p_instAl->size(); ++i) {
        consider_next_condition(p_instAl->type_arguments[i], a_alias->alias_ref->substitutedTs->typeT_at(i));
      }
    }
  }
}

TypePtr GenericSubstitutionsDeducing::replace_Ts_with_currently_deduced(TypePtr orig) const {
  return replace_genericT_with_deduced(orig, &deducedTs);
}

TypePtr GenericSubstitutionsDeducing::auto_deduce_from_argument(TypePtr param_type, TypePtr arg_type) {
  consider_next_condition(param_type, arg_type);
  return replace_genericT_with_deduced(param_type, &deducedTs);
}

TypePtr GenericSubstitutionsDeducing::auto_deduce_from_argument(FunctionPtr cur_f, SrcLocation loc, TypePtr param_type, TypePtr arg_type) {
  std::string_view unknown_nameT;
  consider_next_condition(param_type, arg_type);
  param_type = replace_genericT_with_deduced(param_type, &deducedTs, true, &unknown_nameT);
  if (param_type->has_genericT_inside()) {
    fire_error_can_not_deduce(cur_f, loc, unknown_nameT);
  }
  return param_type;
}

std::string_view GenericSubstitutionsDeducing::get_first_not_deduced_nameT() const {
  for (int i = 0; i < deducedTs.size(); ++i) {
    if (deducedTs.typeT_at(i) == nullptr) {
      return deducedTs.nameT_at(i);
    }
  }
  return "";
}

void GenericSubstitutionsDeducing::apply_defaults_from_declaration() {
  deducedTs.rewrite_missing_with_defaults();
}

void GenericSubstitutionsDeducing::fire_error_can_not_deduce(FunctionPtr cur_f, SrcLocation loc, std::string_view nameT) const {
  if (fun_ref) {
    fire(cur_f, loc, "can not deduce " + static_cast<std::string>(nameT) + " for generic function `" + fun_ref->as_human_readable() + "`; instantiate it manually with `" + fun_ref->name + "<...>()`");
  } else {
    fire(cur_f, loc, "can not deduce " + static_cast<std::string>(nameT) + " for generic struct `" + struct_ref->as_human_readable() + "`; instantiate it manually with `" + struct_ref->name + "<...>`");
  }
}

std::string GenericsDeclaration::as_human_readable(bool include_from_receiver) const {
  std::string result;
  if (int start_from = include_from_receiver ? 0 : n_from_receiver; start_from < size()) {
    result += '<';
    for (int i = start_from; i < size(); ++i) {
      if (result.size() > 1) {
        result += ", ";
      }
      result += itemsT[i].nameT;
    }
    result += '>';
  }
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

bool GenericsSubstitutions::has_nameT(std::string_view nameT) const {
  return genericTs->find_nameT(nameT) != -1;
}

TypePtr GenericsSubstitutions::get_substitution_for_nameT(std::string_view nameT) const {
  int idx = genericTs->find_nameT(nameT);
  return idx == -1 ? nullptr : valuesTs[idx];
}

TypePtr GenericsSubstitutions::get_default_for_nameT(std::string_view nameT) const {
  int idx = genericTs->find_nameT(nameT);
  return idx == -1 ? nullptr : genericTs->get_defaultT(idx);
}

// given this=<T1> and rhs=<T2>, check that T1 is equal to T2 in terms of "equal_to" of TypePtr
// for example, `Wrapper<WrapperAlias<int>>` / `Wrapper<Wrapper<int>>` / `Wrapper<WrappedInt>` are equal
bool GenericsSubstitutions::equal_to(const GenericsSubstitutions* rhs) const {
  if (size() != rhs->size()) {
    return false;
  }
  for (int i = 0; i < size(); ++i) {
    if (!valuesTs[i]->equal_to(rhs->valuesTs[i])) {
      return false;
    }
  }
  return true;
}

// when cloning `f<T>`, original name is "f", we need a new name for symtable and output
// name of an instantiated function will be "f<int>" and similar (yes, with "<" symbol, it's okay to Fift)
static std::string generate_instantiated_name(const std::string& orig_name, const GenericsSubstitutions& substitutedTs, bool allow_spaces, int size_from_receiver = 0) {
  // an instantiated function name will be "{orig_name}<{T1,T2,...}>"
  std::string name = orig_name;
  if (size_from_receiver < substitutedTs.size()) {
    name += '<';
    for (int i = size_from_receiver; i < substitutedTs.size(); ++i) {
      if (name.size() > orig_name.size() + 1) {
        name += ", ";
      }
      name += substitutedTs.typeT_at(i)->as_human_readable();
    }
    name += '>';
  }
  if (!allow_spaces) {
    name.erase(std::remove(name.begin(), name.end(), ' '), name.end());
  }
  return name;
}

FunctionPtr instantiate_generic_function(FunctionPtr fun_ref, GenericsSubstitutions&& substitutedTs) {
  tolk_assert(fun_ref->is_generic_function() && !fun_ref->has_tvm_method_id());

  // fun_ref->name = "f", inst_name will be "f<int>" and similar
  std::string fun_name = fun_ref->name;
  if (fun_ref->is_method() && fun_ref->receiver_type->has_genericT_inside()) {
    fun_name = replace_genericT_with_deduced(fun_ref->receiver_type, &substitutedTs)->as_human_readable() + "." + fun_ref->method_name;
  }
  std::string new_name = generate_instantiated_name(fun_name, substitutedTs, false, fun_ref->genericTs->n_from_receiver);
  if (const Symbol* existing_sym = lookup_global_symbol(new_name)) {
    FunctionPtr existing_ref = existing_sym->try_as<FunctionPtr>();
    tolk_assert(existing_ref);
    return existing_ref;
  }

  // to store permanently, allocate an object in heap
  const GenericsSubstitutions* allocatedTs = new GenericsSubstitutions(std::move(substitutedTs));

  // built-in functions don't have AST to clone, types of parameters don't exist in AST, etc.
  // nevertheless, for outer code to follow a single algorithm,
  // when calling `debugPrint(x)`, we clone it as "debugPrint<int>", replace types, and insert into symtable
  if (fun_ref->is_builtin_function()) {
    std::vector<LocalVarData> new_parameters;
    new_parameters.reserve(fun_ref->get_num_params());
    for (const LocalVarData& orig_p : fun_ref->parameters) {
      TypePtr new_param_type = replace_genericT_with_deduced(orig_p.declared_type, allocatedTs);
      new_parameters.emplace_back(orig_p.name, orig_p.loc, new_param_type, orig_p.flags, orig_p.param_idx);
    }
    TypePtr new_return_type = replace_genericT_with_deduced(fun_ref->declared_return_type, allocatedTs);
    TypePtr new_receiver_type = replace_genericT_with_deduced(fun_ref->receiver_type, allocatedTs);
    FunctionData* new_fun_ref = new FunctionData(new_name, fun_ref->loc, fun_ref->method_name, new_receiver_type, new_return_type, std::move(new_parameters), fun_ref->flags, nullptr, allocatedTs, fun_ref->body, fun_ref->ast_root);
    new_fun_ref->arg_order = fun_ref->arg_order;
    new_fun_ref->ret_order = fun_ref->ret_order;
    new_fun_ref->base_fun_ref = fun_ref;
    G.symtable.add_function(new_fun_ref);
    return new_fun_ref;
  }

  // for `f<T>` (both asm and regular), create "f<int>" with AST fully cloned
  // it means, that types still contain T: `f<int>(v: T): T`, but since type resolving knows
  // it's instantiation, when resolving types, it substitutes T=int
  V<ast_function_declaration> orig_root = fun_ref->ast_root->as<ast_function_declaration>();
  V<ast_function_declaration> new_root = ASTReplicator::clone_function_ast(orig_root);

  FunctionPtr new_fun_ref = pipeline_register_instantiated_generic_function(fun_ref, new_root, std::move(new_name), allocatedTs);
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

StructPtr instantiate_generic_struct(StructPtr struct_ref, GenericsSubstitutions&& substitutedTs) {
  tolk_assert(struct_ref->is_generic_struct());

  // if `Wrapper<int>` was earlier instantiated, return it
  std::string new_name = generate_instantiated_name(struct_ref->name, substitutedTs, true);
  if (const Symbol* existing_sym = lookup_global_symbol(new_name)) {
    StructPtr existing_ref = existing_sym->try_as<StructPtr>();
    tolk_assert(existing_ref);
    return existing_ref;
  }

  const GenericsSubstitutions* allocatedTs = new GenericsSubstitutions(std::move(substitutedTs));
  V<ast_struct_declaration> orig_root = struct_ref->ast_root->as<ast_struct_declaration>();
  V<ast_identifier> new_name_ident = createV<ast_identifier>(orig_root->get_identifier()->loc, new_name);
  V<ast_struct_declaration> new_root = ASTReplicator::clone_struct_ast(orig_root, new_name_ident);

  StructPtr new_struct_ref = pipeline_register_instantiated_generic_struct(struct_ref, new_root, std::move(new_name), allocatedTs);
  tolk_assert(new_struct_ref);
  pipeline_resolve_identifiers_and_assign_symbols(new_struct_ref);
  pipeline_resolve_types_and_aliases(new_struct_ref);
  return new_struct_ref;
}

AliasDefPtr instantiate_generic_alias(AliasDefPtr alias_ref, GenericsSubstitutions&& substitutedTs) {
  tolk_assert(alias_ref->is_generic_alias());

  // if `Response<int>` was earlier instantiated, return it
  std::string new_name = generate_instantiated_name(alias_ref->name, substitutedTs, true);
  if (const Symbol* existing_sym = lookup_global_symbol(new_name)) {
    AliasDefPtr existing_ref = existing_sym->try_as<AliasDefPtr>();
    tolk_assert(existing_ref);
    return existing_ref;
  }

  const GenericsSubstitutions* allocatedTs = new GenericsSubstitutions(std::move(substitutedTs));
  V<ast_type_alias_declaration> orig_root = alias_ref->ast_root->as<ast_type_alias_declaration>();
  V<ast_identifier> new_name_ident = createV<ast_identifier>(orig_root->get_identifier()->loc, new_name);
  V<ast_type_alias_declaration> new_root = ASTReplicator::clone_type_alias_ast(orig_root, new_name_ident);

  AliasDefPtr new_alias_ref = pipeline_register_instantiated_generic_alias(alias_ref, new_root, std::move(new_name), allocatedTs);
  tolk_assert(new_alias_ref);
  pipeline_resolve_types_and_aliases(new_alias_ref);
  return new_alias_ref;
}

// find `builder.storeInt` for called_receiver = "builder" and called_name = "storeInt"
// most practical case, when a direct method for receiver exists
FunctionPtr match_exact_method_for_call_not_generic(TypePtr called_receiver, std::string_view called_name) {
  FunctionPtr exact_found = nullptr;

  for (FunctionPtr method_ref : G.all_methods) {
    if (method_ref->method_name == called_name && !method_ref->receiver_type->has_genericT_inside()) {
      if (method_ref->receiver_type->equal_to(called_receiver)) {
        if (exact_found) {
          return nullptr;
        }
        exact_found = method_ref;
      }
    }
  }

  return exact_found;
}

// find `int?.copy` / `T.copy` for called_receiver = "int" and called_name = "copy"
std::vector<MethodCallCandidate> match_methods_for_call_including_generic(TypePtr called_receiver, std::string_view called_name) {
  std::vector<MethodCallCandidate> candidates;

  // step1: find all methods where a receiver equals to provided, e.g. `MInt.copy`
  for (FunctionPtr method_ref : G.all_methods) {
    if (method_ref->method_name == called_name && !method_ref->receiver_type->has_genericT_inside()) {
      if (method_ref->receiver_type->equal_to(called_receiver)) {
        candidates.emplace_back(method_ref, GenericsSubstitutions(method_ref->genericTs));
      }
    }
  }
  if (!candidates.empty()) {
    return candidates;
  }

  // step2: find all methods where a receiver can accept provided, e.g. `int8.copy` / `int?.copy` / `(int|slice).copy`
  for (FunctionPtr method_ref : G.all_methods) {
    if (method_ref->method_name == called_name && !method_ref->receiver_type->has_genericT_inside()) {
      if (method_ref->receiver_type->can_rhs_be_assigned(called_receiver)) {
        candidates.emplace_back(method_ref, GenericsSubstitutions(method_ref->genericTs));
      }
    }
  }
  if (!candidates.empty()) {
    return candidates;
  }

  // step 3: try to match generic receivers, e.g. `Container<T>.copy` / `(T?|slice).copy` but NOT `T.copy`
  for (FunctionPtr method_ref : G.all_methods) {
    if (method_ref->method_name == called_name && method_ref->receiver_type->has_genericT_inside() && !method_ref->receiver_type->try_as<TypeDataGenericT>()) {
      try {
        GenericSubstitutionsDeducing deducingTs(method_ref);
        TypePtr replaced = deducingTs.auto_deduce_from_argument(method_ref->receiver_type, called_receiver);
        if (!replaced->has_genericT_inside()) {
          candidates.emplace_back(method_ref, deducingTs.flush());
        }
      } catch (...) {}
    }
  }
  if (!candidates.empty()) {
    return candidates;
  }

  // step 4: try to match `T.copy`
  for (FunctionPtr method_ref : G.all_methods) {
    if (method_ref->method_name == called_name && method_ref->receiver_type->try_as<TypeDataGenericT>()) {
      try {
        GenericSubstitutionsDeducing deducingTs(method_ref);
        TypePtr replaced = deducingTs.auto_deduce_from_argument(method_ref->receiver_type, called_receiver);
        if (!replaced->has_genericT_inside()) {
          candidates.emplace_back(method_ref, deducingTs.flush());
        }
      } catch (...) {}
    }
  }
  return candidates;
}

} // namespace tolk
