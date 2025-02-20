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
 *   Example: `fun f(): KKK`                  fires an error "unknown type name"
 *
 *   Types resolving is done everywhere: inside functions bodies, in struct fields, inside globals declaration, etc.
 *   See finalize_type_node().
 *
 *   Note, that resolving T to TypeDataGenericT (and replacing T with substitution when instantiating a generic type)
 * is also done here, see genericTs and instantiationTs.
 */

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
  const GenericsInstantiation* instantiationTs;     // `<int>` if we're inside `f<int>`

  TypePtr parse_ast_type_node(AnyTypeV v) {
    switch (v->kind) {
      case ast_type_leaf_text: {
        std::string_view text = v->as<ast_type_leaf_text>()->text;
        if (TypePtr predefined_type = try_parse_predefined_type(text)) {
          return predefined_type;
        }
        if (genericTs) {
          // if we're inside `f<T>`, replace "T" with TypeDataGenericT
          // if we're inside `f<int>`, replace "T" with TypeDataInt (substitution)
          if (int idx = genericTs->find_nameT(text); idx != -1) {
            if (instantiationTs) {
              return instantiationTs->substitutions[idx];
            }
            return TypeDataGenericT::create(static_cast<std::string>(text));
          }
        }
        if (const Symbol* sym = lookup_global_symbol(text)) {
          if (TypePtr struct_or_other = try_resolve_user_defined_type(sym)) {
            return struct_or_other;
          }
        }
        fire_error_unknown_type_name(cur_f, v->as<ast_type_leaf_text>()->loc, text);
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

      default:
        throw UnexpectedASTNodeKind(v, "resolve_ast_type_node");
    }
  }

  static TypePtr try_resolve_user_defined_type(const Symbol* sym) {
    if (AliasDefPtr alias_ref = sym->try_as<AliasDefPtr>()) {
      if (!alias_ref->was_visited_by_resolver()) {
        visit_symbol(alias_ref);
      }
      return TypeDataAlias::create(alias_ref);
    }
    if (StructPtr struct_ref = sym->try_as<StructPtr>()) {
      if (!struct_ref->was_visited_by_resolver()) {
        visit_symbol(struct_ref);
      }
      return TypeDataStruct::create(struct_ref);
    }
    return nullptr;
  }

  static void validate_resulting_union_type(const TypeDataUnion* t_union, FunctionPtr cur_f, SrcLocation loc) {
    for (TypePtr variant : t_union->variants) {
      if (variant == TypeDataVoid::create() || variant == TypeDataNever::create()) {
        fire_void_type_not_allowed_inside_union(cur_f, loc, variant);
      }
    }
  }

public:

  TypeNodesVisitorResolver(FunctionPtr cur_f, const GenericsDeclaration* genericTs, const GenericsInstantiation* instantiationTs)
    : cur_f(cur_f)
    , genericTs(genericTs)
    , instantiationTs(instantiationTs) {}

  TypePtr finalize_type_node(AnyTypeV type_node) {
#ifdef TOLK_DEBUG
    tolk_assert(type_node != nullptr);
#endif
    TypePtr resolved_type = parse_ast_type_node(type_node);
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
    TypeNodesVisitorResolver visitor(nullptr, nullptr, nullptr);
    TypePtr declared_type = visitor.finalize_type_node(glob_ref->type_node);
    glob_ref->mutate()->assign_resolved_type(declared_type);
  }

  static void visit_symbol(GlobalConstPtr const_ref) {
    if (!const_ref->type_node) {
      return;
    }

    TypeNodesVisitorResolver visitor(nullptr, nullptr, nullptr);
    TypePtr declared_type = visitor.finalize_type_node(const_ref->type_node);
    const_ref->mutate()->assign_resolved_type(declared_type);
  }

  static void visit_symbol(AliasDefPtr alias_ref) {
    static std::vector<AliasDefPtr> called_stack;

    // prevent recursion like `type A = B; type B = A`
    bool contains = std::find(called_stack.begin(), called_stack.end(), alias_ref) != called_stack.end();
    if (contains) {
      throw ParseError(alias_ref->loc, "type `" + alias_ref->name + "` circularly references itself");
    }

    called_stack.push_back(alias_ref);
    TypeNodesVisitorResolver visitor(nullptr, nullptr, nullptr);
    TypePtr underlying_type = visitor.finalize_type_node(alias_ref->underlying_type_node);
    alias_ref->mutate()->assign_resolved_type(underlying_type);
    alias_ref->mutate()->assign_visited_by_resolver();
    called_stack.pop_back();
  }

  static void visit_symbol(StructPtr struct_ref) {
    static std::vector<StructPtr> called_stack;

    // prevent recursion like `struct A { field: A }`
    // currently, a struct is a tensor, and recursion always leads to infinite size (`A?` also, it's also on a stack)
    // if there would be an annotation to store a struct in a tuple, then it has to be reconsidered
    bool contains = std::find(called_stack.begin(), called_stack.end(), struct_ref) != called_stack.end();
    if (contains) {
      throw ParseError(struct_ref->loc, "struct `" + struct_ref->name + "` size is infinity due to recursive fields");
    }

    called_stack.push_back(struct_ref);
    TypeNodesVisitorResolver visitor(nullptr, nullptr, nullptr);
    for (int i = 0; i < struct_ref->get_num_fields(); ++i) {
      StructFieldPtr field_ref = struct_ref->get_field(i);
      TypePtr declared_type = visitor.finalize_type_node(field_ref->type_node);
      field_ref->mutate()->assign_resolved_type(declared_type);
    }
    struct_ref->mutate()->assign_visited_by_resolver();
    called_stack.pop_back();
  }
};

class ResolveTypesInsideFunctionVisitor final : public ASTVisitorFunctionBody {
  static TypeNodesVisitorResolver type_nodes_visitor;

  static TypePtr finalize_type_node(AnyTypeV type_node) {
    return type_nodes_visitor.finalize_type_node(type_node);
  }

protected:

  void visit(V<ast_local_var_lhs> v) override {
    if (v->type_node) {
      TypePtr declared_type = finalize_type_node(v->type_node);
      v->var_ref->mutate()->assign_resolved_type(declared_type);
    }
  }

  void visit(V<ast_reference> v) override {
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
      finalize_type_node(v->pattern_type_node);
    }
    parent::visit(v->get_pattern_expr());
    parent::visit(v->get_body());
  }

  void visit(V<ast_dot_access> v) override {
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
    finalize_type_node(v->type_node);
    parent::visit(v->get_expr());
  }

public:
  bool should_visit_function(FunctionPtr fun_ref) override {
    return !fun_ref->is_builtin_function();
  }

  void start_visiting_function(FunctionPtr fun_ref, V<ast_function_declaration> v) override {
    type_nodes_visitor = TypeNodesVisitorResolver(fun_ref, fun_ref->genericTs, fun_ref->instantiationTs);

    for (int i = 0; i < v->get_num_params(); ++i) {
      const LocalVarData& param_var = fun_ref->parameters[i];
      TypePtr declared_type = finalize_type_node(param_var.type_node);
      param_var.mutate()->assign_resolved_type(declared_type);
    }
    if (fun_ref->return_type_node) {
      TypePtr declared_return_type = finalize_type_node(fun_ref->return_type_node);
      fun_ref->mutate()->assign_resolved_type(declared_return_type);
    }

    if (fun_ref->is_code_function()) {
      parent::visit(v->get_body()->as<ast_block_statement>());
    }

    type_nodes_visitor = TypeNodesVisitorResolver(nullptr, nullptr, nullptr);
  }

  void start_visiting_constant(GlobalConstPtr const_ref) {
    // `const a = 0 as int8`, resolve types there
    // same for struct field `v: int8 = 0 as int8`
    parent::visit(const_ref->init_value);
  }

  void start_visiting_struct_fields(StructPtr struct_ref) {
    // same for struct field `v: int8 = 0 as int8`
    for (StructFieldPtr field_ref : struct_ref->fields) {
      if (field_ref->has_default_value()) {
        parent::visit(field_ref->default_value);
      }
    }
  }
};

TypeNodesVisitorResolver ResolveTypesInsideFunctionVisitor::type_nodes_visitor(nullptr, nullptr, nullptr);

void pipeline_resolve_types_and_aliases() {
  ResolveTypesInsideFunctionVisitor visitor;

  for (const SrcFile* file : G.all_src_files) {
    for (AnyV v : file->ast->as<ast_tolk_file>()->get_toplevel_declarations()) {
      if (auto v_func = v->try_as<ast_function_declaration>()) {
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
        if (!v_alias->alias_ref->was_visited_by_resolver()) {
          TypeNodesVisitorResolver::visit_symbol(v_alias->alias_ref);
        }

      } else if (auto v_struct = v->try_as<ast_struct_declaration>()) {
        if (!v_struct->struct_ref->was_visited_by_resolver()) {
          TypeNodesVisitorResolver::visit_symbol(v_struct->struct_ref);
        }
        visitor.start_visiting_struct_fields(v_struct->struct_ref);
      }
    }
  }
}

void pipeline_resolve_types_and_aliases(FunctionPtr fun_ref) {
  ResolveTypesInsideFunctionVisitor visitor;
  if (visitor.should_visit_function(fun_ref)) {
    visitor.start_visiting_function(fun_ref, fun_ref->ast_root->as<ast_function_declaration>());
  }
}

} // namespace tolk
