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
#include "ast.h"
#include "ast-replicator.h"
#include "compilation-errors.h"
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

GenericSubstitutionsDeducing::GenericSubstitutionsDeducing(const GenericsDeclaration* genericTs)
  : fun_ref(nullptr)
  , struct_ref(nullptr)
  , deducedTs(genericTs) {
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
    // `arg: T?` called as `f(T1|T2|null)` => T is T1|T2
    if (const auto* a_nullable = arg_type->unwrap_alias()->try_as<TypeDataUnion>(); a_nullable && a_nullable->has_null()) {
      std::vector<TypePtr> rest_but_null;
      rest_but_null.reserve(a_nullable->size() - 1);
      for (TypePtr a_variant : a_nullable->variants) {
        if (a_variant != TypeDataNullLiteral::create()) {
          rest_but_null.push_back(a_variant);
        }
      }
      consider_next_condition(p_nullable->or_null, TypeDataUnion::create(std::move(rest_but_null)));
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
          auto it = std::find_if(a_sub_p.begin(), a_sub_p.end(), [p_variant](TypePtr a) {
            return a->equal_to(p_variant);
          });
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
    if (const auto* a_struct = arg_type->unwrap_alias()->try_as<TypeDataStruct>(); a_struct && a_struct->struct_ref->is_instantiation_of_generic_struct() && a_struct->struct_ref->base_struct_ref == p_instSt->struct_ref) {
      tolk_assert(p_instSt->size() == a_struct->struct_ref->substitutedTs->size());
      for (int i = 0; i < p_instSt->size(); ++i) {
        consider_next_condition(p_instSt->type_arguments[i], a_struct->struct_ref->substitutedTs->typeT_at(i));
      }
    }
    // `arg: Wrapper<T>` called as `f(Wrapper<Wrapper<T>>)` => T is Wrapper<T>
    if (const auto* a_instSt = arg_type->try_as<TypeDataGenericTypeWithTs>(); a_instSt && a_instSt->struct_ref == p_instSt->struct_ref) {
      tolk_assert(p_instSt->size() == a_instSt->size());
      for (int i = 0; i < p_instSt->size(); ++i) {
        consider_next_condition(p_instSt->type_arguments[i], a_instSt->type_arguments[i]);
      }
    }
    // `arg: Wrapper<T>?` called as `f(Wrapper<int>)` => T is int
    if (const auto* a_union = arg_type->unwrap_alias()->try_as<TypeDataUnion>()) {
      TypePtr variant_matches = nullptr;
      int n_matches = 0;
      for (TypePtr a_variant : a_union->variants) {
        if (const auto* a_struct = a_variant->unwrap_alias()->try_as<TypeDataStruct>(); a_struct && a_struct->struct_ref->is_instantiation_of_generic_struct() && a_struct->struct_ref->base_struct_ref == p_instSt->struct_ref) {
          variant_matches = a_variant;
          n_matches++;
        }
      }
      if (n_matches == 1) {
        consider_next_condition(param_type, variant_matches);
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
  } else if (const auto* p_map = param_type->try_as<TypeDataMapKV>()) {
    // `arg: map<K, V>` called as `f(someMapInt32Slice)` => K = int32, V = slice
    if (const auto* a_map = arg_type->unwrap_alias()->try_as<TypeDataMapKV>()) {
      consider_next_condition(p_map->TKey, a_map->TKey);
      consider_next_condition(p_map->TValue, a_map->TValue);
    }
    // `arg: map<K, V>?` called as `f(someMapInt32Slice)` => K = int32, V = slice
    if (const auto* a_union = arg_type->unwrap_alias()->try_as<TypeDataUnion>()) {
      TypePtr variant_matches = nullptr;
      int n_matches = 0;
      for (TypePtr a_variant : a_union->variants) {
        if (a_variant->unwrap_alias()->try_as<TypeDataMapKV>()) {
          variant_matches = a_variant;
          n_matches++;
        }
      }
      if (n_matches == 1) {
        consider_next_condition(param_type, variant_matches);
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

TypePtr GenericSubstitutionsDeducing::auto_deduce_from_argument(FunctionPtr cur_f, SrcRange range, TypePtr param_type, TypePtr arg_type) {
  std::string_view unknown_nameT;
  consider_next_condition(param_type, arg_type);
  param_type = replace_genericT_with_deduced(param_type, &deducedTs, true, &unknown_nameT);
  if (param_type->has_genericT_inside()) {
    err_can_not_deduce(unknown_nameT).fire(range, cur_f);
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

Error GenericSubstitutionsDeducing::err_can_not_deduce(std::string_view nameT) const {
  if (fun_ref) {
    return err("can not deduce {} for generic function `{}`; instantiate it manually with `{}<...>()`", nameT, fun_ref, fun_ref->name);
  } else {
    return err("can not deduce {} for generic struct `{}`; instantiate it manually with `{}<...>`", nameT, struct_ref, struct_ref->name);
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

// for `f<T1, T2, T3 = int>` return 2 (mandatory type arguments when instantiating manually)
int GenericsDeclaration::size_no_defaults() const {
  for (int i = size(); i > 0; --i) {
     if (itemsT[i - 1].default_type == nullptr) {
       return i;
     }
  }
  return 0;
}

int GenericsDeclaration::find_nameT(std::string_view nameT) const {
  for (int i = 0; i < static_cast<int>(itemsT.size()); ++i) {
    if (itemsT[i].nameT == nameT) {
      return i;
    }
  }
  return -1;
}

// given `fun f<T1, T2, T3 = int>` and a call `f<builder,slice>()`, append `int`;
// similarly, for structures: when a user missed default type arguments, append them
void GenericsDeclaration::append_defaults(std::vector<TypePtr>& manually_provided) const {
  for (int i = n_from_receiver + static_cast<int>(manually_provided.size()); i < size(); ++i) {
    tolk_assert(itemsT[i].default_type);
    manually_provided.push_back(itemsT[i].default_type);
  }
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

GNU_ATTRIBUTE_NOINLINE
// body of a cloned generic/lambda function (it's cloned at type inferring step) needs the previous pipeline to run
// for example, all local vars need to be registered as symbols, etc.
// these pipes are exactly the same as in tolk.cpp — all preceding (and including) type inferring
static void run_pipeline_for_cloned_function(FunctionPtr new_fun_ref) {
  pipeline_resolve_identifiers_and_assign_symbols(new_fun_ref);
  pipeline_resolve_types_and_aliases(new_fun_ref);
  pipeline_calculate_rvalue_lvalue(new_fun_ref);
  pipeline_infer_types_and_calls_and_fields(new_fun_ref);
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
  if (fun_ref->is_builtin()) {
    std::vector<LocalVarData> new_parameters;
    new_parameters.reserve(fun_ref->get_num_params());
    for (const LocalVarData& orig_p : fun_ref->parameters) {
      TypePtr new_param_type = replace_genericT_with_deduced(orig_p.declared_type, allocatedTs);
      new_parameters.emplace_back(orig_p.name, nullptr, new_param_type, orig_p.default_value, orig_p.flags, orig_p.param_idx);
    }
    TypePtr new_return_type = replace_genericT_with_deduced(fun_ref->declared_return_type, allocatedTs);
    TypePtr new_receiver_type = replace_genericT_with_deduced(fun_ref->receiver_type, allocatedTs);
    FunctionData* new_fun_ref = new FunctionData(new_name, nullptr, fun_ref->method_name, new_receiver_type, new_return_type, std::move(new_parameters), fun_ref->flags, fun_ref->inline_mode, nullptr, allocatedTs, fun_ref->body, fun_ref->ast_root);
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
  run_pipeline_for_cloned_function(new_fun_ref);
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
  V<ast_identifier> new_name_ident = createV<ast_identifier>(orig_root->get_identifier()->range, new_name);
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
  V<ast_identifier> new_name_ident = createV<ast_identifier>(orig_root->get_identifier()->range, new_name);
  V<ast_type_alias_declaration> new_root = ASTReplicator::clone_type_alias_ast(orig_root, new_name_ident);

  AliasDefPtr new_alias_ref = pipeline_register_instantiated_generic_alias(alias_ref, new_root, std::move(new_name), allocatedTs);
  tolk_assert(new_alias_ref);
  pipeline_resolve_types_and_aliases(new_alias_ref);
  return new_alias_ref;
}

// instantiating a lambda is very similar to instantiating a generic function, it's also done at type inferring;
// when an expression `fun(params) { body }` is reached, this function is instantiated as a standalone function
// and travels the pipeline separately; essentially, it's the same as if such a global function existed:
// > fun globalF(params) { body }
// and this expression is just a reference to it
FunctionPtr instantiate_lambda_function(AnyV v_lambda, FunctionPtr parent_fun_ref, const std::vector<TypePtr>& params_types, TypePtr return_type) {
  auto v = v_lambda->try_as<ast_lambda_fun>();
  tolk_assert(v && !v->lambda_ref && v->get_body()->kind == ast_block_statement);

  int n_lambdas = 1;
  for (FunctionPtr fun_ref : G.all_functions) {
    n_lambdas += fun_ref->is_lambda();
  }

  // parent_fun_ref always exists actually (and will be `lambda_ref->base_fun_ref`);
  // the only way it may be nullptr is when a lambda occurs as a constant value for example, which will fire an error later
  std::string lambda_name = "lambda_in_" + (parent_fun_ref ? parent_fun_ref->name : "") + "@" + std::to_string(n_lambdas);
  tolk_assert(!lookup_global_symbol(lambda_name));

  V<ast_function_declaration> lambda_root = ASTReplicator::clone_lambda_as_standalone(v);
  FunctionPtr lambda_ref = pipeline_register_instantiated_lambda_function(parent_fun_ref, lambda_root, std::move(lambda_name));
  tolk_assert(lambda_ref);

  // parameters of a lambda are allowed to be untyped: they are inferred before instantiation, e.g.
  // > fun call(f: (int) -> slice) { ... }
  // > call(fun(i) { ... })
  // then params_types=[int], return_type=slice, and we assign them for an instantiated lambda
  tolk_assert(lambda_ref->get_num_params() == static_cast<int>(params_types.size()));
  for (int i = 0; i < lambda_ref->get_num_params(); ++i) {
    lambda_ref->get_param(i).mutate()->assign_resolved_type(params_types[i]);
  }
  lambda_ref->mutate()->assign_resolved_type(return_type);

  run_pipeline_for_cloned_function(lambda_ref);
  return lambda_ref;  
}

// a function `tuple.push<T>(self, v: T) asm "TPUSH"` can't be called with T=Point (2 stack slots);
// almost all asm/built-in generic functions expect one stack slot, but there are exceptions
bool is_allowed_asm_generic_function_with_non1_width_T(FunctionPtr fun_ref, int idxT) {
  // if a built-in function is marked with a special flag
  if (fun_ref->is_variadic_width_T_allowed()) {
    return true;
  }

  // allow "Cell<T>.hash", "map<K, V>.isEmpty" and other methods that don't depend on internal structure
  if (fun_ref->is_method() && idxT < fun_ref->genericTs->n_from_receiver) {
    TypePtr receiver = fun_ref->receiver_type->unwrap_alias(); 
    if (const auto* r_withTs = receiver->try_as<TypeDataGenericTypeWithTs>()) {
      return r_withTs->struct_ref && r_withTs->struct_ref->name == "Cell";
    }
    if (receiver->try_as<TypeDataMapKV>()) {
      return true;
    }
  }

  return false;
}

} // namespace tolk
