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
#include "src-file.h"
#include "generics-helpers.h"
#include "ast.h"
#include "ast-visitor.h"
#include "type-system.h"
#include <unordered_map>

/*
 *   This pipe resolves identifiers (local variables and types) in all functions bodies.
 *   It happens before type inferring, but after all global symbols are registered.
 * It means, that for any symbol `x` we can look up whether it's a global name or not.
 *
 *   About resolving variables.
 *   Example: `var x = 10; x = 20;` both `x` point to one LocalVarData.
 *   Example: `x = 20` undefined symbol `x` is also here (unless it's a global)
 *   Variables scoping and redeclaration are also here.
 *   Note, that `x` is stored as `ast_reference (ast_identifier "x")`. More formally, "references" are resolved.
 * "Reference" in AST, besides the identifier, stores optional generics instantiation. `x<int>` is grammar-valid.
 *
 *   About resolving types. At the moment of parsing, `int`, `cell` and other predefined are parsed as TypeDataInt, etc.
 * All the others are stored as TypeDataUnresolved, to be resolved here, after global symtable is filled.
 *   Example: `var x: T = 0` unresolved "T" is replaced by TypeDataGenericT inside `f<T>`.
 *   Example: `f<MyAlias>()` unresolved "MyAlias" is replaced by TypeDataAlias inside the reference.
 *   Example: `fun f(): KKK` unresolved "KKK" fires an error "unknown type name".
 *   When structures and type aliases are implemented, their resolving will also be done here.
 *   See finalize_type_data().
 *
 *   Note, that functions/methods binding is NOT here.
 *   In other words, for ast_function_call `beginCell()` and `t.tupleAt(0)`, their fun_ref is NOT filled here.
 * Functions/methods binding is done later, simultaneously with type inferring and generics instantiation.
 * For instance, to call a generic function `t.tuplePush(1)`, we need types of `t` and `1` to be inferred,
 * as well as `tuplePush<int>` to be instantiated, and fun_ref to point at that exact instantiations.
 *
 *   As a result of this step,
 *   * every V<ast_reference>::sym is filled, pointing either to a local var/parameter, or to a global symbol
 *     (exceptional for function calls and methods, their references are bound later)
 *   * all TypeData in all symbols is ready for analyzing, TypeDataUnresolved won't occur later in pipeline
 */

namespace tolk {

GNU_ATTRIBUTE_NORETURN GNU_ATTRIBUTE_COLD
static void fire_error_undefined_symbol(V<ast_identifier> v) {
  if (v->name == "self") {
    v->error("using `self` in a non-member function (it does not accept the first `self` parameter)");
  } else {
    v->error("undefined symbol `" + static_cast<std::string>(v->name) + "`");
  }
}

GNU_ATTRIBUTE_NORETURN GNU_ATTRIBUTE_COLD
static void fire_error_unknown_type_name(SrcLocation loc, const std::string &text) {
  throw ParseError(loc, "unknown type name `" + text + "`");
}

static void check_import_exists_when_using_sym(AnyV v_usage, const Symbol* used_sym) {
  SrcLocation sym_loc = used_sym->loc;
  if (!v_usage->loc.is_symbol_from_same_or_builtin_file(sym_loc)) {
    const SrcFile* declared_in = sym_loc.get_src_file();
    bool has_import = false;
    for (const SrcFile::ImportDirective& import : v_usage->loc.get_src_file()->imports) {
      if (import.imported_file == declared_in) {
        has_import = true;
      }
    }
    if (!has_import) {
      v_usage->error("Using a non-imported symbol `" + used_sym->name + "`. Forgot to import \"" + declared_in->rel_filename + "\"?");
    }
  }
}

struct NameAndScopeResolver {
  std::vector<std::unordered_map<uint64_t, const Symbol*>> scopes;

  static uint64_t key_hash(std::string_view name_key) {
    return std::hash<std::string_view>{}(name_key);
  }

  void open_scope([[maybe_unused]] SrcLocation loc) {
    // std::cerr << "open_scope " << scopes.size() + 1 << " at " << loc << std::endl;
    scopes.emplace_back();
  }

  void close_scope([[maybe_unused]] SrcLocation loc) {
    // std::cerr << "close_scope " << scopes.size() << " at " << loc << std::endl;
    if (UNLIKELY(scopes.empty())) {
      throw Fatal{"cannot close the outer scope"};
    }
    scopes.pop_back();
  }

  const Symbol* lookup_symbol(std::string_view name) const {
    uint64_t key = key_hash(name);
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) { // NOLINT(*-loop-convert)
      const auto& scope = *it;
      if (auto it_sym = scope.find(key); it_sym != scope.end()) {
        return it_sym->second;
      }
    }
    return G.symtable.lookup(name);
  }

  void add_local_var(const LocalVarData* v_sym) {
    if (UNLIKELY(scopes.empty())) {
      throw Fatal("unexpected scope_level = 0");
    }
    if (v_sym->name.empty()) {    // underscore
      return;
    }

    uint64_t key = key_hash(v_sym->name);
    const auto& [_, inserted] = scopes.rbegin()->emplace(key, v_sym);
    if (UNLIKELY(!inserted)) {
      throw ParseError(v_sym->loc, "redeclaration of local variable `" + v_sym->name + "`");
    }
  }
};

struct TypeDataResolver {
  GNU_ATTRIBUTE_NOINLINE
  static TypePtr resolve_identifiers_in_type_data(TypePtr type_data, const GenericsDeclaration* genericTs) {
    return type_data->replace_children_custom([genericTs](TypePtr child) {
      if (const TypeDataUnresolved* un = child->try_as<TypeDataUnresolved>()) {
        if (genericTs && genericTs->has_nameT(un->text)) {
          std::string nameT = un->text;
          return TypeDataGenericT::create(std::move(nameT));
        }
        if (un->text == "auto") {
          throw ParseError(un->loc, "`auto` type does not exist; just omit a type for local variable (will be inferred from assignment); parameters should always be typed");
        }
        if (un->text == "self") {
          throw ParseError(un->loc, "`self` type can be used only as a return type of a function (enforcing it to be chainable)");
        }
        fire_error_unknown_type_name(un->loc, un->text);
      }
      return child;
    });
  }
};

static TypePtr finalize_type_data(TypePtr type_data, const GenericsDeclaration* genericTs) {
  if (!type_data || !type_data->has_unresolved_inside()) {
    return type_data;
  }
  return TypeDataResolver::resolve_identifiers_in_type_data(type_data, genericTs);
}


class AssignSymInsideFunctionVisitor final : public ASTVisitorFunctionBody {
  // more correctly this field shouldn't be static, but currently there is no need to make it a part of state
  static NameAndScopeResolver current_scope;
  static const FunctionData* current_function;

  static const LocalVarData* create_local_var_sym(std::string_view name, SrcLocation loc, TypePtr declared_type, bool immutable) {
    LocalVarData* v_sym = new LocalVarData(static_cast<std::string>(name), loc, declared_type, immutable * LocalVarData::flagImmutable, -1);
    current_scope.add_local_var(v_sym);
    return v_sym;
  }

  static void process_catch_variable(AnyExprV catch_var) {
    if (auto v_ref = catch_var->try_as<ast_reference>()) {
      const LocalVarData* var_ref = create_local_var_sym(v_ref->get_name(), catch_var->loc, nullptr, true);
      v_ref->mutate()->assign_sym(var_ref);
    }
  }

protected:
  void visit(V<ast_local_var_lhs> v) override {
    if (v->marked_as_redef) {
      const Symbol* sym = current_scope.lookup_symbol(v->get_name());
      if (sym == nullptr) {
        v->error("`redef` for unknown variable");
      }
      const LocalVarData* var_ref = sym->try_as<LocalVarData>();
      if (!var_ref) {
        v->error("`redef` for unknown variable");
      }
      v->mutate()->assign_var_ref(var_ref);
    } else {
      TypePtr declared_type = finalize_type_data(v->declared_type, current_function->genericTs);
      const LocalVarData* var_ref = create_local_var_sym(v->get_name(), v->loc, declared_type, v->is_immutable);
      v->mutate()->assign_resolved_type(declared_type);
      v->mutate()->assign_var_ref(var_ref);
    }
  }

  void visit(V<ast_assign> v) override {
    parent::visit(v->get_rhs());    // in this order, so that `var x = x` is invalid, "x" on the right unknown
    parent::visit(v->get_lhs());
  }

  void visit(V<ast_reference> v) override {
    const Symbol* sym = current_scope.lookup_symbol(v->get_name());
    if (!sym) {
      fire_error_undefined_symbol(v->get_identifier());
    }
    v->mutate()->assign_sym(sym);

    // for global functions, global vars and constants, `import` must exist
    if (!sym->try_as<LocalVarData>()) {
      check_import_exists_when_using_sym(v, sym);
    }

    // for `f<int, MyAlias>` / `f<T>`, resolve "MyAlias" and "T"
    // (for function call `f<T>()`, this v (ast_reference `f<T>`) is callee)
    if (auto v_instantiationTs = v->get_instantiationTs()) {
      for (int i = 0; i < v_instantiationTs->size(); ++i) {
        TypePtr substituted_type = finalize_type_data(v_instantiationTs->get_item(i)->substituted_type, current_function->genericTs);
        v_instantiationTs->get_item(i)->mutate()->assign_resolved_type(substituted_type);
      }
    }
  }

  void visit(V<ast_dot_access> v) override {
    // for `t.tupleAt<MyAlias>` / `obj.method<T>`, resolve "MyAlias" and "T"
    // (for function call `t.tupleAt<MyAlias>()`, this v (ast_dot_access `t.tupleAt<MyAlias>`) is callee)
    if (auto v_instantiationTs = v->get_instantiationTs()) {
      for (int i = 0; i < v_instantiationTs->size(); ++i) {
        TypePtr substituted_type = finalize_type_data(v_instantiationTs->get_item(i)->substituted_type, current_function->genericTs);
        v_instantiationTs->get_item(i)->mutate()->assign_resolved_type(substituted_type);
      }
    }
    parent::visit(v->get_obj());
  }

  void visit(V<ast_cast_as_operator> v) override {
    TypePtr cast_to_type = finalize_type_data(v->cast_to_type, current_function->genericTs);
    v->mutate()->assign_resolved_type(cast_to_type);
    parent::visit(v->get_expr());
  }

  void visit(V<ast_sequence> v) override {
    if (v->empty()) {
      return;
    }
    current_scope.open_scope(v->loc);
    parent::visit(v);
    current_scope.close_scope(v->loc_end);
  }

  void visit(V<ast_do_while_statement> v) override {
    current_scope.open_scope(v->loc);
    parent::visit(v->get_body());
    parent::visit(v->get_cond()); // in 'while' condition it's ok to use variables declared inside do
    current_scope.close_scope(v->get_body()->loc_end);
  }

  void visit(V<ast_try_catch_statement> v) override {
    visit(v->get_try_body());
    current_scope.open_scope(v->get_catch_body()->loc);
    const std::vector<AnyExprV>& catch_items = v->get_catch_expr()->get_items();
    tolk_assert(catch_items.size() == 2);
    process_catch_variable(catch_items[1]);
    process_catch_variable(catch_items[0]);
    parent::visit(v->get_catch_body());
    current_scope.close_scope(v->get_catch_body()->loc_end);
  }

public:
  bool should_visit_function(const FunctionData* fun_ref) override {
    // this pipe is done just after parsing
    // visit both asm and code functions, resolve identifiers in parameter/return types everywhere
    // for generic functions, unresolved "T" will be replaced by TypeDataGenericT
    return true;
  }

  void start_visiting_function(const FunctionData* fun_ref, V<ast_function_declaration> v) override {
    current_function = fun_ref;

    for (int i = 0; i < v->get_num_params(); ++i) {
      const LocalVarData& param_var = fun_ref->parameters[i];
      TypePtr declared_type = finalize_type_data(param_var.declared_type, fun_ref->genericTs);
      v->get_param(i)->mutate()->assign_param_ref(&param_var);
      v->get_param(i)->mutate()->assign_resolved_type(declared_type);
      param_var.mutate()->assign_resolved_type(declared_type);
    }
    TypePtr return_type = finalize_type_data(fun_ref->declared_return_type, fun_ref->genericTs);
    v->mutate()->assign_resolved_type(return_type);
    fun_ref->mutate()->assign_resolved_type(return_type);

    if (fun_ref->is_code_function()) {
      auto v_seq = v->get_body()->as<ast_sequence>();
      current_scope.open_scope(v->loc);
      for (int i = 0; i < v->get_num_params(); ++i) {
        current_scope.add_local_var(&fun_ref->parameters[i]);
      }
      parent::visit(v_seq);
      current_scope.close_scope(v_seq->loc_end);
      tolk_assert(current_scope.scopes.empty());
    }

    current_function = nullptr;
  }
};

NameAndScopeResolver AssignSymInsideFunctionVisitor::current_scope;
const FunctionData* AssignSymInsideFunctionVisitor::current_function = nullptr;

void pipeline_resolve_identifiers_and_assign_symbols() {
  AssignSymInsideFunctionVisitor visitor;
  for (const SrcFile* file : G.all_src_files) {
    for (AnyV v : file->ast->as<ast_tolk_file>()->get_toplevel_declarations()) {
      if (auto v_func = v->try_as<ast_function_declaration>()) {
        tolk_assert(v_func->fun_ref);
        visitor.start_visiting_function(v_func->fun_ref, v_func);

      } else if (auto v_global = v->try_as<ast_global_var_declaration>()) {
        TypePtr declared_type = finalize_type_data(v_global->var_ref->declared_type, nullptr);
        v_global->mutate()->assign_resolved_type(declared_type);
        v_global->var_ref->mutate()->assign_resolved_type(declared_type);

      } else if (auto v_const = v->try_as<ast_constant_declaration>(); v_const && v_const->declared_type) {
        TypePtr declared_type = finalize_type_data(v_const->const_ref->declared_type, nullptr);
        v_const->mutate()->assign_resolved_type(declared_type);
        v_const->const_ref->mutate()->assign_resolved_type(declared_type);
      }
    }
  }
}

void pipeline_resolve_identifiers_and_assign_symbols(const FunctionData* fun_ref) {
  AssignSymInsideFunctionVisitor visitor;
  if (visitor.should_visit_function(fun_ref)) {
    visitor.start_visiting_function(fun_ref, fun_ref->ast_root->as<ast_function_declaration>());
  }
}

} // namespace tolk
