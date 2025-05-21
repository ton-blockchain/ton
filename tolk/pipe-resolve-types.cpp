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
#include "tolk.h"
#include "platform-utils.h"
#include "compiler-state.h"
#include "ast.h"
#include "ast-visitor.h"
#include "generics-helpers.h"
#include "type-system.h"
#include <charconv>

namespace tolk {

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

GNU_ATTRIBUTE_NORETURN GNU_ATTRIBUTE_COLD
static void fire_error_unknown_type_name(FunctionPtr cur_f, SrcLocation loc, std::string_view text) {
  if (text == "auto") {
    fire(cur_f, loc, "`auto` type does not exist; just omit a type for local variable (will be inferred from assignment); parameters should always be typed");
  }
  if (text == "self") {
    fire(cur_f, loc, "`self` type can be used only as a return type of a function (enforcing it to be chainable)");
  }
  fire(cur_f, loc, "unknown type name `" + static_cast<std::string>(text) + "`");
}

GNU_ATTRIBUTE_NORETURN GNU_ATTRIBUTE_COLD
static void fire_void_type_not_allowed_inside_union(FunctionPtr cur_f, SrcLocation loc, TypePtr disallowed_variant) {
  fire(cur_f, loc, "type `" + disallowed_variant->as_human_readable() + "` is not allowed inside a union");
}

GNU_ATTRIBUTE_NORETURN GNU_ATTRIBUTE_COLD
static void fire_error_generic_type_used_without_T(FunctionPtr cur_f, SrcLocation loc, const std::string& type_name_with_Ts) {
  fire(cur_f, loc, "type `" + type_name_with_Ts + "` is generic, you should provide type arguments");
}

static TypePtr parse_intN(std::string_view strN, bool is_unsigned) {
  int n;
  auto result = std::from_chars(strN.data() + 3 + static_cast<int>(is_unsigned), strN.data() + strN.size(), n);
  bool parsed = result.ec == std::errc() && result.ptr == strN.data() + strN.size();
  if (!parsed || n <= 0 || n > 256 + static_cast<int>(is_unsigned)) {
    return nullptr;   // `int1000`, maybe it's user-defined alias, let it be unresolved
  }
  return TypeDataIntN::create(is_unsigned, false, n);
}

static TypePtr parse_bytesN(std::string_view strN, bool is_bits) {
  int n;
  auto result = std::from_chars(strN.data() + 5  - static_cast<int>(is_bits), strN.data() + strN.size(), n);
  bool parsed = result.ec == std::errc() && result.ptr == strN.data() + strN.size();
  if (!parsed || n <= 0 || n > 1024) {
    return nullptr;   // `bytes9999`, maybe it's user-defined alias, let it be unresolved
  }
  return TypeDataBytesN::create(is_bits, n);
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
      if (str == "address") return TypeDataAddress::create();
      break;
    case 8:
      if (str == "varint16") return TypeDataIntN::create(false, true, 16);
      if (str == "varint32") return TypeDataIntN::create(false, true, 32);
      break;
    case 12:
      if (str == "continuation") return TypeDataContinuation::create();
      break;
    default:
      break;
  }

  if (str.starts_with("int")) {
    if (TypePtr intN = parse_intN(str, false)) {
      return intN;
    }
  }
  if (str.size() > 4 && str.starts_with("uint")) {
    if (TypePtr uintN = parse_intN(str, true)) {
      return uintN;
    }
  }
  if (str.size() > 4 && str.starts_with("bits")) {
    if (TypePtr bitsN = parse_bytesN(str, true)) {
      return bitsN;
    }
  }
  if (str.size() > 5 && str.starts_with("bytes")) {
    if (TypePtr bytesN = parse_bytesN(str, false)) {
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
        SrcLocation loc = v->as<ast_type_leaf_text>()->loc;
        if (genericTs && genericTs->find_nameT(text) != -1) {
          // if we're inside `f<T>`, replace "T" with TypeDataGenericT
          return TypeDataGenericT::create(static_cast<std::string>(text));
        }
        if (substitutedTs && substitutedTs->has_nameT(text)) {
          // if we're inside `f<int>`, replace "T" with TypeDataInt
          return substitutedTs->get_substitution_for_nameT(text);
        }
        if (const Symbol* sym = lookup_global_symbol(text)) {
          if (TypePtr custom_type = try_resolve_user_defined_type(cur_f, loc, sym, allow_without_type_arguments)) {
            return custom_type;
          }
        }
        if (TypePtr predefined_type = try_parse_predefined_type(text)) {
          return predefined_type;
        }
        if (treat_unresolved_as_genericT) {
          return TypeDataGenericT::create(static_cast<std::string>(text));
        }
        fire_error_unknown_type_name(cur_f, loc, text);
      }

      case ast_type_question_nullable: {
        TypePtr inner = finalize_type_node(v->as<ast_type_question_nullable>()->get_inner());
        TypePtr result = TypeDataUnion::create_nullable(inner);
        if (const TypeDataUnion* t_union = result->try_as<TypeDataUnion>()) {
          validate_resulting_union_type(t_union, cur_f, v->loc);
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
          validate_resulting_union_type(t_union, cur_f, v->loc);
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
        return instantiate_generic_type_or_fire(cur_f, v->loc, inner, std::move(type_arguments));
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
  static TypePtr try_resolve_user_defined_type(FunctionPtr cur_f, SrcLocation loc, const Symbol* sym, bool allow_without_type_arguments) {
    if (AliasDefPtr alias_ref = sym->try_as<AliasDefPtr>()) {
      if (alias_ref->is_generic_alias() && !allow_without_type_arguments) {
        fire_error_generic_type_used_without_T(cur_f, loc, alias_ref->as_human_readable());
      }
      if (!visited_aliases.contains(alias_ref)) {
        visit_symbol(alias_ref);
      }
      return TypeDataAlias::create(alias_ref);
    }
    if (StructPtr struct_ref = sym->try_as<StructPtr>()) {
      if (struct_ref->is_generic_struct() && !allow_without_type_arguments) {
        fire_error_generic_type_used_without_T(cur_f, loc, struct_ref->as_human_readable());
      }
      if (!visited_structs.contains(struct_ref)) {
        visit_symbol(struct_ref);
      }
      return TypeDataStruct::create(struct_ref);
    }
    return nullptr;
  }

  // given `Wrapper<int>` / `Pair<slice, int>` / `Response<int, cell>`, instantiate a generic struct/alias
  // an error for invalid usage `Pair<int>` / `cell<int>` is also here
  static TypePtr instantiate_generic_type_or_fire(FunctionPtr cur_f, SrcLocation loc, TypePtr type_to_instantiate, std::vector<TypePtr>&& type_arguments) {
    // example: `type WrapperAlias<T> = Wrapper<T>`, we are at `Wrapper<T>`, type_arguments = `<T>`
    // they contain generics, so the struct is not ready to be instantiated yet
    bool is_still_generic = false;
    for (TypePtr argT : type_arguments) {
      is_still_generic |= argT->has_genericT_inside();
    }

    if (const TypeDataStruct* t_struct = type_to_instantiate->try_as<TypeDataStruct>(); t_struct && t_struct->struct_ref->is_generic_struct()) {
      StructPtr struct_ref = t_struct->struct_ref;
      if (struct_ref->genericTs->size() != static_cast<int>(type_arguments.size())) {
        fire(cur_f, loc, "struct `" + struct_ref->as_human_readable() + "` expects " + std::to_string(struct_ref->genericTs->size()) + " type arguments, but " + std::to_string(type_arguments.size()) + " provided");
      }
      if (is_still_generic) {
        return TypeDataGenericTypeWithTs::create(struct_ref, nullptr, std::move(type_arguments));
      }
      return TypeDataStruct::create(instantiate_generic_struct(struct_ref, GenericsSubstitutions(struct_ref->genericTs, type_arguments)));
    }
    if (const TypeDataAlias* t_alias = type_to_instantiate->try_as<TypeDataAlias>(); t_alias && t_alias->alias_ref->is_generic_alias()) {
      AliasDefPtr alias_ref = t_alias->alias_ref;
      if (alias_ref->genericTs->size() != static_cast<int>(type_arguments.size())) {
        fire(cur_f, loc, "type `" + alias_ref->as_human_readable() + "` expects " + std::to_string(alias_ref->genericTs->size()) + " type arguments, but " + std::to_string(type_arguments.size()) + " provided");
      }
      if (is_still_generic) {
        return TypeDataGenericTypeWithTs::create(nullptr, alias_ref, std::move(type_arguments));
      }
      return TypeDataAlias::create(instantiate_generic_alias(alias_ref, GenericsSubstitutions(alias_ref->genericTs, type_arguments)));
    }
    if (const TypeDataGenericT* asT = type_to_instantiate->try_as<TypeDataGenericT>()) {
      fire_error_unknown_type_name(cur_f, loc, asT->nameT);
    }
    // `User<int>` / `cell<cell>`
    fire(cur_f, loc, "type `" + type_to_instantiate->as_human_readable() + "` is not generic");
  }

  static void validate_resulting_union_type(const TypeDataUnion* t_union, FunctionPtr cur_f, SrcLocation loc) {
    for (TypePtr variant : t_union->variants) {
      if (variant == TypeDataVoid::create() || variant == TypeDataNever::create()) {
        fire_void_type_not_allowed_inside_union(cur_f, loc, variant);
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
      throw ParseError(alias_ref->loc, "type `" + alias_ref->name + "` circularly references itself");
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
          v_item->error("duplicate generic parameter `" + static_cast<std::string>(v_item->nameT) + "`");
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

protected:

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
      if (obj_ref->sym == nullptr) {
        std::string_view obj_type_name = obj_ref->get_identifier()->name;
        AnyTypeV obj_type_node = createV<ast_type_leaf_text>(obj_ref->loc, obj_type_name);
        if (obj_ref->has_instantiationTs()) {     // Container<int>.create
          std::vector<AnyTypeV> inner_and_args;
          inner_and_args.reserve(1 + obj_ref->get_instantiationTs()->size());
          inner_and_args.push_back(obj_type_node);
          for (int i = 0; i < obj_ref->get_instantiationTs()->size(); ++i) {
            inner_and_args.push_back(obj_ref->get_instantiationTs()->get_item(i)->type_node);
          }
          obj_type_node = createV<ast_type_triangle_args>(obj_ref->loc, std::move(inner_and_args));
        }
        TypePtr type_as_reference = finalize_type_node(obj_type_node);
        const Symbol* type_as_symbol = new TypeReferenceUsedAsSymbol(static_cast<std::string>(obj_type_name), obj_ref->loc, type_as_reference);
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

public:
  bool should_visit_function(FunctionPtr fun_ref) override {
    return !fun_ref->is_builtin_function();
  }

  void start_visiting_function(FunctionPtr fun_ref, V<ast_function_declaration> v) override {
    if (fun_ref->receiver_type_node) {
      TypeNodesVisitorResolver receiver_visitor(fun_ref, fun_ref->genericTs, fun_ref->substitutedTs, true);
      TypePtr receiver_type = receiver_visitor.finalize_type_node(fun_ref->receiver_type_node);
      std::string name_prefix = receiver_type->as_human_readable();
      bool embrace = receiver_type->try_as<TypeDataUnion>() && !receiver_type->try_as<TypeDataUnion>()->or_null;
      if (embrace) {
        name_prefix = "(" + name_prefix + ")";
      }
      fun_ref->mutate()->assign_resolved_receiver_type(receiver_type, std::move(name_prefix));
      G.symtable.add_function(fun_ref);
    }
    if (v->genericsT_list || (fun_ref->receiver_type && fun_ref->receiver_type->has_genericT_inside())) {
      const GenericsDeclaration* genericTs = TypeNodesVisitorResolver::construct_genericTs(fun_ref->receiver_type, v->genericsT_list);
      fun_ref->mutate()->assign_resolved_genericTs(genericTs);
    }

    type_nodes_visitor = TypeNodesVisitorResolver(fun_ref, fun_ref->genericTs, fun_ref->substitutedTs, false);

    for (int i = 0; i < fun_ref->get_num_params(); ++i) {
      LocalVarPtr param_ref = &fun_ref->parameters[i];
      TypePtr declared_type = finalize_type_node(param_ref->type_node);
      param_ref->mutate()->assign_resolved_type(declared_type);
      if (param_ref->has_default_value()) {
        parent::visit(param_ref->default_value);
      }
    }
    if (fun_ref->return_type_node) {
      TypePtr declared_return_type = finalize_type_node(fun_ref->return_type_node);
      fun_ref->mutate()->assign_resolved_type(declared_return_type);
    }

    if (fun_ref->is_code_function()) {
      parent::visit(v->get_body()->as<ast_block_statement>());
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
      throw ParseError(struct_ref->loc, "struct `" + struct_ref->name + "` size is infinity due to recursive fields");
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
