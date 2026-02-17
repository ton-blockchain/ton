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
#include "ast.h"
#include "ast-visitor.h"
#include "compilation-errors.h"
#include "compiler-state.h"
#include "generics-helpers.h"
#include "type-system.h"
#include <charconv>

namespace tolk {

void patch_builtins_after_stdlib_loaded();

/*
 *   This pipe transforms AST of types into TypePtr.
 *   It happens after all global symbols were registered, and all local references were bound.
 *
 *   At the moment of parsing, `int`, `cell` and other were parsed as AnyTypeV (ast_type_leaf_text and others).
 *   Example: `var x: int = ...`              to TypeDataInt
 *   Example: `fun f(a: cell): (int, User)`   param to TypeDataCell, return type to TypeDataTensor(TypeDataInt, TypeDataStruct)
 *   Example: `var x: T = 0`                  to TypeDataGenericT inside `f<T>`
 *   Example: `f<MyAlias>()`                  to TypeDataAlias inside instantiation list
 *   Example: `arg: Wrapper<int>`             instantiates "Wrapper<int>" right here and returns TypeDataStruct to it
 *   Example: `fun f(): KKK`                  fires an error "unknown type name"
 *
 *   Types resolving is done everywhere: inside functions bodies, in struct fields, inside globals declaration, etc.
 *   See finalize_type_node().
 *
 *   Note, that resolving T to TypeDataGenericT (and replacing T with substitution when instantiating a generic type)
 * is also done here, see genericTs and substitutedTs.
 *   Note, that instantiating generic structs and aliases is also done here (if they don't have generic Ts inside).
 * Example: `type OkInt = Ok<int>`, struct "Ok<int>" is instantiated (as a clone of `Ok<T>` substituting T=int)
 * Example: `type A<T> = Ok<T>`, then `Ok<T>` is not ready yet, it's left as TypeDataGenericTypeWithTs.
 */

static std::unordered_map<StructPtr, bool> visited_structs;
static std::unordered_map<AliasDefPtr, bool> visited_aliases;

static Error err_unknown_type_name(std::string_view text) {
  if (text == "auto") {
    return err("`auto` type does not exist; just omit a type for local variable (will be inferred from assignment); parameters should always be typed");
  }
  if (text == "self") {
    return err("`self` type can be used only as a return type of a method `fun T.methodForT(self)`");
  }
  return err("unknown type name `{}`", text);
}

static Error err_void_type_not_allowed_inside_union(TypePtr disallowed_variant) {
  return err("type `{}` is not allowed inside a union", disallowed_variant);
}

static Error err_generic_type_used_without_T(const std::string& type_name_with_Ts) {
  return err("type `{}` is generic, you should provide type arguments", type_name_with_Ts);
}

static TypePtr parse_intN_uintN(std::string_view strN, bool is_unsigned) {
  int n;
  auto result = std::from_chars(strN.data(), strN.data() + strN.size(), n);
  bool parsed = result.ec == std::errc() && result.ptr == strN.data() + strN.size();
  if (!parsed || n <= 0 || n > 257 - static_cast<int>(is_unsigned)) {
    return nullptr;   // `int1000`, maybe it's user-defined alias, let it be unresolved
  }
  return TypeDataIntN::create(n, is_unsigned, false);
}

static TypePtr parse_bytesN_bitsN(std::string_view strN, bool is_bits) {
  int n;
  auto result = std::from_chars(strN.data(), strN.data() + strN.size(), n);
  bool parsed = result.ec == std::errc() && result.ptr == strN.data() + strN.size();
  if (!parsed || n <= 0 || n > 1024) {
    return nullptr;   // `bytes9999`, maybe it's user-defined alias, let it be unresolved
  }
  return TypeDataBitsN::create(n, is_bits);
}

static TypePtr try_parse_predefined_type(std::string_view str) {
  switch (str.size()) {
    case 3:
      if (str == "int") return TypeDataInt::create();
      break;
    case 4:
      if (str == "cell") return TypeDataCell::create();
      if (str == "void") return TypeDataVoid::create();
      if (str == "bool") return TypeDataBool::create();
      if (str == "null") return TypeDataNullLiteral::create();
      break;
    case 5:
      if (str == "slice") return TypeDataSlice::create();
      if (str == "tuple") return TypeDataTuple::create();
      if (str == "coins") return TypeDataCoins::create();
      if (str == "never") return TypeDataNever::create();
      break;
    case 7:
      if (str == "builder") return TypeDataBuilder::create();
      if (str == "address") return TypeDataAddress::internal();
      break;
    case 8:
      if (str == "varint16") return TypeDataIntN::create(16, false, true);
      if (str == "varint32") return TypeDataIntN::create(32, false, true);
      break;
    case 9:
      if (str == "varuint16") return TypeDataIntN::create(16, true, true);
      if (str == "varuint32") return TypeDataIntN::create(32, true, true);
      break;
    case 11:
      if (str == "any_address") return TypeDataAddress::any();
      break;
    case 12:
      if (str == "continuation") return TypeDataContinuation::create();
      break;
    default:
      break;
  }

  if (str.starts_with("int")) {
    if (TypePtr intN = parse_intN_uintN(str.substr(3), false)) {
      return intN;
    }
  }
  if (str.starts_with("uint")) {
    if (TypePtr uintN = parse_intN_uintN(str.substr(4), true)) {
      return uintN;
    }
  }
  if (str.starts_with("bits")) {
    if (TypePtr bitsN = parse_bytesN_bitsN(str.substr(4), true)) {
      return bitsN;
    }
  }
  if (str.starts_with("bytes")) {
    if (TypePtr bytesN = parse_bytesN_bitsN(str.substr(5), false)) {
      return bytesN;
    }
  }

  return nullptr;
}

class TypeNodesVisitorResolver {
  FunctionPtr cur_f;                                // exists if we're inside its body
  const GenericsDeclaration* genericTs;             // `<T>` if we're inside `f<T>` or `f<int>`
  const GenericsSubstitutions* substitutedTs;       // `T=int` if we're inside `f<int>`
  bool treat_unresolved_as_genericT;                // used for receivers `fun Container<T>.create()`, T becomes generic

  TypePtr parse_ast_type_node(AnyTypeV v, bool allow_without_type_arguments) {
    switch (v->kind) {
      case ast_type_leaf_text: {
        std::string_view text = v->as<ast_type_leaf_text>()->text;
        if (genericTs && genericTs->find_nameT(text) != -1) {
          // if we're inside `f<T>`, replace "T" with TypeDataGenericT
          return TypeDataGenericT::create(static_cast<std::string>(text));
        }
        if (substitutedTs && substitutedTs->has_nameT(text)) {
          // if we're inside `f<int>`, replace "T" with TypeDataInt
          return substitutedTs->get_substitution_for_nameT(text);
        }
        if (text == "map") {
          if (!allow_without_type_arguments) {
            err_generic_type_used_without_T("map<K,V>").fire(v, cur_f);
          }
          return TypeDataMapKV::create(TypeDataGenericT::create("K"), TypeDataGenericT::create("V"));
        }
        if (const Symbol* sym = lookup_global_symbol(text)) {
          if (TypePtr custom_type = try_resolve_user_defined_type(cur_f, v->range, sym, allow_without_type_arguments)) {
            bool allow_no_import = sym->is_builtin() || sym->ident_anchor->range.is_file_id_same_or_stdlib_common(v->range);
            if (!allow_no_import) {
              sym->check_import_exists_when_used_from(cur_f, v);
            }
            return custom_type;
          }
        }
        if (TypePtr predefined_type = try_parse_predefined_type(text)) {
          return predefined_type;
        }
        if (treat_unresolved_as_genericT) {
          return TypeDataGenericT::create(static_cast<std::string>(text));
        }
        err_unknown_type_name(text).fire(v, cur_f);
      }

      case ast_type_question_nullable: {
        TypePtr inner = finalize_type_node(v->as<ast_type_question_nullable>()->get_inner());
        TypePtr result = TypeDataUnion::create_nullable(inner);
        if (const TypeDataUnion* t_union = result->try_as<TypeDataUnion>()) {
          validate_resulting_union_type(t_union, cur_f, v->range);
        }
        return result;
      }

      case ast_type_parenthesis_tensor: {
        std::vector<TypePtr> items = finalize_type_node(v->as<ast_type_parenthesis_tensor>()->get_items());
        if (items.size() == 1) {
          return items.front();
        }
        return TypeDataTensor::create(std::move(items));
      }

      case ast_type_bracket_tuple: {
        std::vector<TypePtr> items = finalize_type_node(v->as<ast_type_bracket_tuple>()->get_items());
        return TypeDataBrackets::create(std::move(items));
      }

      case ast_type_arrow_callable: {
        std::vector<TypePtr> params_and_return = finalize_type_node(v->as<ast_type_arrow_callable>()->get_params_and_return());
        TypePtr return_type = params_and_return.back();
        params_and_return.pop_back();
        return TypeDataFunCallable::create(std::move(params_and_return), return_type);
      }

      case ast_type_vertical_bar_union: {
        std::vector<TypePtr> variants = finalize_type_node(v->as<ast_type_vertical_bar_union>()->get_variants());
        TypePtr result = TypeDataUnion::create(std::move(variants));
        if (const TypeDataUnion* t_union = result->try_as<TypeDataUnion>()) {
          validate_resulting_union_type(t_union, cur_f, v->range);
        }
        return result;
      }

      case ast_type_triangle_args: {
        const std::vector<AnyTypeV>& inner_and_args = v->as<ast_type_triangle_args>()->get_inner_and_args();
        TypePtr inner = finalize_type_node(inner_and_args.front(), true);
        std::vector<TypePtr> type_arguments;
        type_arguments.reserve(inner_and_args.size() - 1);
        for (size_t i = 1; i < inner_and_args.size(); ++i) {
          type_arguments.push_back(finalize_type_node(inner_and_args[i]));
        }
        return instantiate_generic_type_or_fire(cur_f, inner_and_args.front()->range, inner, std::move(type_arguments));
      }

      default:
        throw UnexpectedASTNodeKind(v, "parse_ast_type_node");
    }
  }

  // given `dict` / `User` / `Wrapper` / `WrapperAlias`, find it in a symtable
  // for generic types, like `Wrapper`, fire that it's used without type arguments (unless allowed)
  // example: `var w: Wrapper = ...`, here will be an error of generic usage without T
  // example: `w is Wrapper`, here not, it's allowed (instantiated at type inferring later)
  // example: `var w: KKK`, nullptr will be returned
  static TypePtr try_resolve_user_defined_type(FunctionPtr cur_f, SrcRange range, const Symbol* sym, bool allow_without_type_arguments) {
    if (AliasDefPtr alias_ref = sym->try_as<AliasDefPtr>()) {
      if (alias_ref->is_generic_alias() && !allow_without_type_arguments) {
        err_generic_type_used_without_T(alias_ref->as_human_readable()).fire(range, cur_f);
      }
      if (!visited_aliases.contains(alias_ref)) {
        visit_symbol(alias_ref);
      }
      return TypeDataAlias::create(alias_ref);
    }
    if (StructPtr struct_ref = sym->try_as<StructPtr>()) {
      if (struct_ref->is_generic_struct() && !allow_without_type_arguments) {
        err_generic_type_used_without_T(struct_ref->as_human_readable()).fire(range, cur_f);
      }
      if (!visited_structs.contains(struct_ref)) {
        visit_symbol(struct_ref);
      }
      return TypeDataStruct::create(struct_ref);
    }
    if (EnumDefPtr enum_ref = sym->try_as<EnumDefPtr>()) {
      return TypeDataEnum::create(enum_ref);
    }
    return nullptr;
  }

  // given `Wrapper<int>` / `Pair<slice, int>` / `Response<int, cell>`, instantiate a generic struct/alias
  // an error for invalid usage `Pair<int>` / `cell<int>` is also here
  static TypePtr instantiate_generic_type_or_fire(FunctionPtr cur_f, SrcRange range, TypePtr type_to_instantiate, std::vector<TypePtr>&& type_arguments) {
    // example: `type WrapperAlias<T> = Wrapper<T>`, we are at `Wrapper<T>`, type_arguments = `<T>`
    // they contain generics, so the struct is not ready to be instantiated yet
    bool is_still_generic = false;
    for (TypePtr argT : type_arguments) {
      is_still_generic |= argT->has_genericT_inside();
    }

    if (const TypeDataStruct* t_struct = type_to_instantiate->try_as<TypeDataStruct>(); t_struct && t_struct->struct_ref->is_generic_struct()) {
      StructPtr struct_ref = t_struct->struct_ref;
      int n_provided = static_cast<int>(type_arguments.size()); 
      if (n_provided < struct_ref->genericTs->size_no_defaults() || n_provided > struct_ref->genericTs->size()) {
        err("struct `{}` expects {} type arguments, but {} provided", struct_ref, struct_ref->genericTs->size(), type_arguments.size()).fire(range, cur_f);
      }
      struct_ref->genericTs->append_defaults(type_arguments);
      if (is_still_generic) {
        return TypeDataGenericTypeWithTs::create(struct_ref, nullptr, std::move(type_arguments));
      }
      return TypeDataStruct::create(instantiate_generic_struct(struct_ref, GenericsSubstitutions(struct_ref->genericTs, type_arguments)));
    }
    if (const TypeDataAlias* t_alias = type_to_instantiate->try_as<TypeDataAlias>(); t_alias && t_alias->alias_ref->is_generic_alias()) {
      AliasDefPtr alias_ref = t_alias->alias_ref;
      int n_provided = static_cast<int>(type_arguments.size()); 
      if (n_provided < alias_ref->genericTs->size_no_defaults() || n_provided > alias_ref->genericTs->size()) {
        err("type `{}` expects {} type arguments, but {} provided", alias_ref, alias_ref->genericTs->size(), type_arguments.size()).fire(range, cur_f);
      }
      alias_ref->genericTs->append_defaults(type_arguments);
      if (is_still_generic) {
        return TypeDataGenericTypeWithTs::create(nullptr, alias_ref, std::move(type_arguments));
      }
      return TypeDataAlias::create(instantiate_generic_alias(alias_ref, GenericsSubstitutions(alias_ref->genericTs, type_arguments)));
    }
    if (const TypeDataMapKV* t_map = type_to_instantiate->try_as<TypeDataMapKV>(); t_map && t_map->TKey->try_as<TypeDataGenericT>()) {
      if (type_arguments.size() != 2) {
        err("type `map<K,V>` expects 2 type arguments, but {} provided", type_arguments.size()).fire(range, cur_f);
      }
      return TypeDataMapKV::create(type_arguments[0], type_arguments[1]);
    }
    if (const TypeDataGenericT* asT = type_to_instantiate->try_as<TypeDataGenericT>()) {
      err_unknown_type_name(asT->nameT).fire(range, cur_f);
    }
    // `User<int>` / `cell<cell>`
    err("type `{}` is not generic", type_to_instantiate).fire(range, cur_f);
  }

  static void validate_resulting_union_type(const TypeDataUnion* t_union, FunctionPtr cur_f, SrcRange range) {
    for (TypePtr variant : t_union->variants) {
      if (variant == TypeDataVoid::create() || variant == TypeDataNever::create()) {
        err_void_type_not_allowed_inside_union(variant).fire(range, cur_f);
      }
    }
  }

public:

  TypeNodesVisitorResolver(FunctionPtr cur_f, const GenericsDeclaration* genericTs, const GenericsSubstitutions* substitutedTs, bool treat_unresolved_as_genericT)
    : cur_f(cur_f)
    , genericTs(genericTs)
    , substitutedTs(substitutedTs)
    , treat_unresolved_as_genericT(treat_unresolved_as_genericT) {}

  TypePtr finalize_type_node(AnyTypeV type_node, bool allow_without_type_arguments = false) {
#ifdef TOLK_DEBUG
    tolk_assert(type_node != nullptr);
#endif
    TypePtr resolved_type = parse_ast_type_node(type_node, allow_without_type_arguments);
    type_node->mutate()->assign_resolved_type(resolved_type);
    return resolved_type;
  }

  std::vector<TypePtr> finalize_type_node(const std::vector<AnyTypeV>& type_node_array) {
    std::vector<TypePtr> result;
    result.reserve(type_node_array.size());
    for (AnyTypeV v : type_node_array) {
      result.push_back(finalize_type_node(v));
    }
    return result;
  }

  static void visit_symbol(GlobalVarPtr glob_ref) {
    TypeNodesVisitorResolver visitor(nullptr, nullptr, nullptr, false);
    TypePtr declared_type = visitor.finalize_type_node(glob_ref->type_node);
    glob_ref->mutate()->assign_resolved_type(declared_type);
  }

  static void visit_symbol(GlobalConstPtr const_ref) {
    if (!const_ref->type_node) {
      return;
    }

    TypeNodesVisitorResolver visitor(nullptr, nullptr, nullptr, false);
    TypePtr declared_type = visitor.finalize_type_node(const_ref->type_node);
    const_ref->mutate()->assign_resolved_type(declared_type);
  }

  static void visit_symbol(AliasDefPtr alias_ref) {
    static std::vector<AliasDefPtr> called_stack;

    // prevent recursion like `type A = B; type B = A` (we can't create TypeDataAlias without a resolved underlying type)
    bool contains = std::find(called_stack.begin(), called_stack.end(), alias_ref) != called_stack.end();
    if (contains) {
      err("type `{}` circularly references itself", alias_ref).fire(alias_ref->ident_anchor);
    }

    if (auto v_genericsT_list = alias_ref->ast_root->as<ast_type_alias_declaration>()->genericsT_list) {
      const GenericsDeclaration* genericTs = construct_genericTs(nullptr, v_genericsT_list);
      alias_ref->mutate()->assign_resolved_genericTs(genericTs);
    }

    called_stack.push_back(alias_ref);
    TypeNodesVisitorResolver visitor(nullptr, alias_ref->genericTs, alias_ref->substitutedTs, false);
    TypePtr underlying_type = visitor.finalize_type_node(alias_ref->underlying_type_node);
    alias_ref->mutate()->assign_resolved_type(underlying_type);
    visited_aliases.insert({alias_ref, 1});
    called_stack.pop_back();
  }

  static void visit_symbol(StructPtr struct_ref) {
    visited_structs.insert({struct_ref, 1});

    if (auto v_genericsT_list = struct_ref->ast_root->as<ast_struct_declaration>()->genericsT_list) {
      const GenericsDeclaration* genericTs = construct_genericTs(nullptr, v_genericsT_list);
      struct_ref->mutate()->assign_resolved_genericTs(genericTs);
    }

    TypeNodesVisitorResolver visitor(nullptr, struct_ref->genericTs, struct_ref->substitutedTs, false);
    for (int i = 0; i < struct_ref->get_num_fields(); ++i) {
      StructFieldPtr field_ref = struct_ref->get_field(i);
      TypePtr declared_type = visitor.finalize_type_node(field_ref->type_node);
      field_ref->mutate()->assign_resolved_type(declared_type);
    }
  }

  static const GenericsDeclaration* construct_genericTs(TypePtr receiver_type, V<ast_genericsT_list> v_list) {
    std::vector<GenericsDeclaration::ItemT> itemsT;
    if (receiver_type && receiver_type->has_genericT_inside()) {
      receiver_type->replace_children_custom([&itemsT](TypePtr child) {
        if (const TypeDataGenericT* asT = child->try_as<TypeDataGenericT>()) {
          auto it_existing = std::find_if(itemsT.begin(), itemsT.end(), [asT](const GenericsDeclaration::ItemT& prevT) {
            return prevT.nameT == asT->nameT;
          });
          if (it_existing == itemsT.end()) {
            itemsT.emplace_back(asT->nameT, nullptr);
          }
        }
        return child;
      });
    }
    int n_from_receiver = static_cast<int>(itemsT.size());

    if (v_list) {
      TypeNodesVisitorResolver visitor(nullptr, nullptr, nullptr, false);
      for (int i = 0; i < v_list->size(); ++i) {
        auto v_item = v_list->get_item(i);
        auto it_existing = std::find_if(itemsT.begin(), itemsT.end(), [v_item](const GenericsDeclaration::ItemT& prevT) {
          return prevT.nameT == v_item->nameT;
        });
        if (it_existing != itemsT.end()) {
          err("duplicate generic parameter `{}`", v_item->nameT).fire(v_item);
        }
        TypePtr default_type = nullptr;
        if (v_list->get_item(i)->default_type_node) {
          default_type = visitor.finalize_type_node(v_list->get_item(i)->default_type_node);
        }
        itemsT.emplace_back(v_item->nameT, default_type);
      }
    }

    return new GenericsDeclaration(std::move(itemsT), n_from_receiver);
  }
};

class ResolveTypesInsideFunctionVisitor final : public ASTVisitorFunctionBody {
  TypeNodesVisitorResolver type_nodes_visitor{nullptr, nullptr, nullptr, false};

  TypePtr finalize_type_node(AnyTypeV type_node, bool allow_without_type_arguments = false) {
    return type_nodes_visitor.finalize_type_node(type_node, allow_without_type_arguments);
  }

  void visit(V<ast_local_var_lhs> v) override {
    if (v->type_node) {
      TypePtr declared_type = finalize_type_node(v->type_node);
      v->var_ref->mutate()->assign_resolved_type(declared_type);
    }
  }

  void visit(V<ast_reference> v) override {
    tolk_assert(v->sym != nullptr);

    // for `f<int, MyAlias>` / `f<T>`, resolve "MyAlias" and "T"
    // (for function call `f<T>()`, this v (ast_reference `f<T>`) is callee)
    if (auto v_instantiationTs = v->get_instantiationTs()) {
      for (int i = 0; i < v_instantiationTs->size(); ++i) {
        finalize_type_node(v_instantiationTs->get_item(i)->type_node);
      }
    }
  }

  void visit(V<ast_match_arm> v) override {
    if (v->pattern_type_node) {
      // before `=>` we allow referencing generic types, type inferring will guess
      // example: `struct Ok<T>` + `type Response<T> = Ok<T> | ErrCode` + `match (resp) { Ok => ... }`
      finalize_type_node(v->pattern_type_node, true);
    }
    parent::visit(v->get_pattern_expr());
    parent::visit(v->get_body());
  }

  void visit(V<ast_dot_access> v) override {
    // for static method calls, like "int.zero()" or "Point.create()", dot obj symbol is unresolved for now
    // so, resolve it as a type and store as a "type reference symbol"
    if (auto obj_ref = v->get_obj()->try_as<ast_reference>()) {
      // also, `someFn.prop` doesn't make any sense, show "unknown type"; it also forces `address.staticMethod()` to work
      if (obj_ref->sym == nullptr || obj_ref->sym->try_as<FunctionPtr>()) {
        std::string_view obj_type_name = obj_ref->get_identifier()->name;
        AnyTypeV obj_type_node = createV<ast_type_leaf_text>(obj_ref->get_identifier()->range, obj_type_name);
        if (obj_ref->has_instantiationTs()) {     // Container<int>.create
          std::vector<AnyTypeV> inner_and_args;
          inner_and_args.reserve(1 + obj_ref->get_instantiationTs()->size());
          inner_and_args.push_back(obj_type_node);
          for (int i = 0; i < obj_ref->get_instantiationTs()->size(); ++i) {
            inner_and_args.push_back(obj_ref->get_instantiationTs()->get_item(i)->type_node);
          }
          obj_type_node = createV<ast_type_triangle_args>(obj_ref->range, std::move(inner_and_args));
        }
        TypePtr type_as_reference = finalize_type_node(obj_type_node);
        const Symbol* type_as_symbol = new TypeReferenceUsedAsSymbol(static_cast<std::string>(obj_type_name), obj_ref->get_identifier(), type_as_reference);
        obj_ref->mutate()->assign_sym(type_as_symbol);
      }
    }

    // for `t.tupleAt<MyAlias>` / `obj.method<T>`, resolve "MyAlias" and "T"
    // (for function call `t.tupleAt<MyAlias>()`, this v (ast_dot_access `t.tupleAt<MyAlias>`) is callee)
    if (auto v_instantiationTs = v->get_instantiationTs()) {
      for (int i = 0; i < v_instantiationTs->size(); ++i) {
        finalize_type_node(v_instantiationTs->get_item(i)->type_node);
      }
    }
    parent::visit(v->get_obj());
  }

  void visit(V<ast_cast_as_operator> v) override {
    finalize_type_node(v->type_node);
    parent::visit(v->get_expr());
  }

  void visit(V<ast_is_type_operator> v) override {
    finalize_type_node(v->type_node, true);
    parent::visit(v->get_expr());
  }

  void visit(V<ast_object_literal> v) override {
    if (v->type_node) {
      finalize_type_node(v->type_node, true);
    }
    parent::visit(v->get_body());
  }

  void visit(V<ast_lambda_fun> v) override {
    for (int i = 0; i < v->get_num_params(); ++i) {
      if (AnyTypeV param_type_node = v->get_param(i)->type_node) {
        finalize_type_node(param_type_node);
      }
    }
    if (v->return_type_node) {
      finalize_type_node(v->return_type_node);
    }
  }

public:
  bool should_visit_function(FunctionPtr fun_ref) override {
    return !fun_ref->is_builtin();
  }

  void on_enter_function(V<ast_function_declaration> v) override {
    if (cur_f->receiver_type_node) {
      TypeNodesVisitorResolver receiver_visitor(cur_f, cur_f->genericTs, cur_f->substitutedTs, true);
      TypePtr receiver_type = receiver_visitor.finalize_type_node(cur_f->receiver_type_node);
      std::string name_prefix = receiver_type->as_human_readable();
      bool embrace = receiver_type->try_as<TypeDataUnion>() && !receiver_type->try_as<TypeDataUnion>()->or_null;
      if (embrace) {
        name_prefix = "(" + name_prefix + ")";
      }
      cur_f->mutate()->assign_resolved_receiver_type(receiver_type, std::move(name_prefix));
      G.symtable.add_function(cur_f);
    }
    if (v->genericsT_list || (cur_f->receiver_type && cur_f->receiver_type->has_genericT_inside())) {
      const GenericsDeclaration* genericTs = TypeNodesVisitorResolver::construct_genericTs(cur_f->receiver_type, v->genericsT_list);
      cur_f->mutate()->assign_resolved_genericTs(genericTs);
    }

    type_nodes_visitor = TypeNodesVisitorResolver(cur_f, cur_f->genericTs, cur_f->substitutedTs, false);

    for (int i = 0; i < cur_f->get_num_params(); ++i) {
      LocalVarPtr param_ref = &cur_f->parameters[i];
      // types for parameters in regular functions are mandatory: `fun f(a: int)`, so type_node always exists;
      // but types for lambdas may be missed out; they are inferred at usage, and declared_type filled before instantiation 
      if (param_ref->type_node) {
        TypePtr declared_type = finalize_type_node(param_ref->type_node);
        param_ref->mutate()->assign_resolved_type(declared_type);
      } else {
        tolk_assert(param_ref->declared_type);
      }
      if (param_ref->has_default_value()) {
        parent::visit(param_ref->default_value);
      }
    }
    if (cur_f->return_type_node) {
      TypePtr declared_return_type = finalize_type_node(cur_f->return_type_node);
      cur_f->mutate()->assign_resolved_type(declared_return_type);
    }
  }

  void start_visiting_constant(GlobalConstPtr const_ref) {
    type_nodes_visitor = TypeNodesVisitorResolver(nullptr, nullptr, nullptr, false);

    // `const a = 0 as int8`, resolve types there
    // same for struct field `v: int8 = 0 as int8`
    parent::visit(const_ref->init_value);
  }

  void start_visiting_struct_fields(StructPtr struct_ref) {
    type_nodes_visitor = TypeNodesVisitorResolver(nullptr, struct_ref->genericTs, struct_ref->substitutedTs, false);

    // same for struct field `v: int8 = 0 as int8`
    for (StructFieldPtr field_ref : struct_ref->fields) {
      if (field_ref->has_default_value()) {
        parent::visit(field_ref->default_value);
      }
    }
  }

  void start_visiting_enum_members(EnumDefPtr enum_ref) {
    type_nodes_visitor = TypeNodesVisitorResolver(nullptr, nullptr, nullptr, false);

    // same for struct field `v: int8 = 0 as int8`
    for (EnumMemberPtr member_ref : enum_ref->members) {
      if (member_ref->has_init_value()) {
        parent::visit(member_ref->init_value);
      }
    }

    // serialization type: `enum Role: int8`
    if (enum_ref->colon_type_node) {
      TypePtr colon_type = finalize_type_node(enum_ref->colon_type_node);
      enum_ref->mutate()->assign_resolved_colon_type(colon_type); // later it will be checked to be intN
    }
  }
};

// prevent recursion like `struct A { field: A }`;
// currently, a struct is a tensor, and recursion always leads to infinite size (`A?` also, it's also on a stack);
// if there is an annotation to store a struct in a tuple, then it has to be reconsidered;
// it's crucial to detect it here; otherwise, get_width_on_stack() will silently face stack overflow
class InfiniteStructSizeDetector {
  static TypePtr visit_type_deeply(TypePtr type) {
    return type->replace_children_custom([](TypePtr child) {
      if (const TypeDataStruct* child_struct = child->try_as<TypeDataStruct>()) {
        check_struct_for_infinite_size(child_struct->struct_ref);
      }
      if (const TypeDataAlias* child_alias = child->try_as<TypeDataAlias>()) {
        return visit_type_deeply(child_alias->underlying_type);
      }
      return child;
    });
  };

  static void check_struct_for_infinite_size(StructPtr struct_ref) {
    static std::vector<StructPtr> called_stack;

    bool contains = std::find(called_stack.begin(), called_stack.end(), struct_ref) != called_stack.end();
    if (contains) {
      err("struct `{}` size is infinity due to recursive fields", struct_ref).fire(struct_ref->ident_anchor);
    }

    called_stack.push_back(struct_ref);
    for (StructFieldPtr field_ref : struct_ref->fields) {
      visit_type_deeply(field_ref->declared_type);
    }
    called_stack.pop_back();
  }

public:
  static void detect_and_fire_if_any_struct_is_infinite() {
    for (auto [struct_ref, _] : visited_structs) {
      check_struct_for_infinite_size(struct_ref);
    }
  }
};

void pipeline_resolve_types_and_aliases() {
  ResolveTypesInsideFunctionVisitor visitor;

  for (const SrcFile* file : G.all_src_files) {
    for (AnyV v : file->ast->as<ast_tolk_file>()->get_toplevel_declarations()) {
      if (auto v_func = v->try_as<ast_function_declaration>(); v_func && !v_func->is_builtin_function()) {
        tolk_assert(v_func->fun_ref);
        if (visitor.should_visit_function(v_func->fun_ref)) {
          visitor.start_visiting_function(v_func->fun_ref, v_func);
        }

      } else if (auto v_global = v->try_as<ast_global_var_declaration>()) {
        TypeNodesVisitorResolver::visit_symbol(v_global->glob_ref);

      } else if (auto v_const = v->try_as<ast_constant_declaration>()) {
        if (v_const->type_node) {
          TypeNodesVisitorResolver::visit_symbol(v_const->const_ref);
        }
        visitor.start_visiting_constant(v_const->const_ref);

      } else if (auto v_alias = v->try_as<ast_type_alias_declaration>()) {
        if (!visited_aliases.contains(v_alias->alias_ref)) {
          TypeNodesVisitorResolver::visit_symbol(v_alias->alias_ref);
        }

      } else if (auto v_struct = v->try_as<ast_struct_declaration>()) {
        if (!visited_structs.contains(v_struct->struct_ref)) {
          TypeNodesVisitorResolver::visit_symbol(v_struct->struct_ref);
        }
        visitor.start_visiting_struct_fields(v_struct->struct_ref);

      } else if (auto v_enum = v->try_as<ast_enum_declaration>()) {
        visitor.start_visiting_enum_members(v_enum->enum_ref);
      }
    }
  }

  InfiniteStructSizeDetector::detect_and_fire_if_any_struct_is_infinite();
  visited_structs.clear();
  visited_aliases.clear();

  patch_builtins_after_stdlib_loaded();
}

void pipeline_resolve_types_and_aliases(FunctionPtr fun_ref) {
  ResolveTypesInsideFunctionVisitor visitor;
  if (visitor.should_visit_function(fun_ref)) {
    visitor.start_visiting_function(fun_ref, fun_ref->ast_root->as<ast_function_declaration>());
  }
}

void pipeline_resolve_types_and_aliases(StructPtr struct_ref) {
  ResolveTypesInsideFunctionVisitor().start_visiting_struct_fields(struct_ref);
  TypeNodesVisitorResolver::visit_symbol(struct_ref);
}

void pipeline_resolve_types_and_aliases(AliasDefPtr alias_ref) {
  TypeNodesVisitorResolver::visit_symbol(alias_ref);
}

} // namespace tolk
