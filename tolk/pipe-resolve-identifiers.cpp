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
#include "src-file.h"

/*
 *   This pipe resolves identifiers (local variables, globals, constants, etc.) in all functions bodies.
 *   It happens before type inferring, but after all global symbols are registered.
 * It means, that for any symbol `x` we can look up whether it's a global name or not.
 *
 *   Example: `var x = 10; x = 20;` both `x` point to one LocalVarData.
 *   Example: `x = 20` undefined symbol `x` is also here (unless it's a global)
 *   Variables scoping and redeclaration are also here.
 *   Note, that `x` is stored as `ast_reference (ast_identifier "x")`. More formally, "references" are resolved.
 * "Reference" in AST, besides the identifier, stores optional generics instantiation. `x<int>` is grammar-valid.
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
 */

namespace tolk {

static Error err_undefined_symbol(V<ast_identifier> v) {
  if (v->name == "self") {
    return err("using `self` in a non-member function (it does not accept the first `self` parameter)");
  } else {
    return err("undefined symbol `{}`", v->name);
  }
}

static Error err_type_used_as_symbol(V<ast_identifier> v) {
  if (v->name == "random") {    // calling `random()`, but it's a struct, correct is `random.uint256()`
    return err("`random` is not a function, you probably want `random.uint256()`");
  } else {
    return err("`{}` only refers to a type, but is being used as a value here", v->name);
  }
}

static Error err_using_self_not_in_method(FunctionPtr cur_f) {
  if (cur_f->is_static_method()) {
    return err("using `self` in a static method");
  } else {
    return err("using `self` in a regular function (not a method)");
  }
}

struct NameAndScopeResolver {
  std::vector<std::unordered_map<uint64_t, const Symbol*>> scopes;

  static uint64_t key_hash(std::string_view name_key) {
    return std::hash<std::string_view>{}(name_key);
  }

  void open_scope() {
    scopes.emplace_back();
  }

  void close_scope() {
    // std::cerr << "close_scope " << scopes.size() << " at " << loc << std::endl;
    tolk_assert(!scopes.empty());
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

  void add_local_var(LocalVarPtr v_sym) {
    tolk_assert(!scopes.empty());
    if (v_sym->name.empty()) {    // underscore
      return;
    }

    uint64_t key = key_hash(v_sym->name);
    const auto& [_, inserted] = scopes.rbegin()->emplace(key, v_sym);
    if (!inserted) {
      err("redeclaration of local variable `{}`", v_sym).fire(v_sym->ident_anchor);
    }
  }
};

class AssignSymInsideFunctionVisitor final : public ASTVisitorFunctionBody {
  NameAndScopeResolver current_scope;

  LocalVarPtr create_local_var_sym(std::string_view name, AnyV ident_anchor, AnyTypeV declared_type_node, bool immutable, bool lateinit) {
    LocalVarData* v_sym = new LocalVarData(static_cast<std::string>(name), ident_anchor, declared_type_node, nullptr, immutable * LocalVarData::flagImmutable + lateinit * LocalVarData::flagLateInit, -1);
    current_scope.add_local_var(v_sym);
    return v_sym;
  }

  void process_catch_variable(AnyExprV catch_var) {
    if (auto v_ref = catch_var->try_as<ast_reference>()) {
      LocalVarPtr var_ref = create_local_var_sym(v_ref->get_name(), catch_var, nullptr, true, false);
      v_ref->mutate()->assign_sym(var_ref);
    }
  }

  void visit(V<ast_local_var_lhs> v) override {
    if (v->marked_as_redef) {
      const Symbol* sym = current_scope.lookup_symbol(v->get_name());
      if (sym == nullptr) {
        err("`redef` for unknown variable").fire(v, cur_f);
      }
      LocalVarPtr var_ref = sym->try_as<LocalVarPtr>();
      if (!var_ref) {
        err("`redef` for unknown variable").fire(v, cur_f);
      }
      v->mutate()->assign_var_ref(var_ref);
    } else {
      LocalVarPtr var_ref = create_local_var_sym(v->get_name(), v, v->type_node, v->is_immutable, v->is_lateinit);
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
      err_undefined_symbol(v->get_identifier()).fire(v->get_identifier(), cur_f);
    }
    if (sym->try_as<AliasDefPtr>() || sym->try_as<StructPtr>() || sym->try_as<EnumDefPtr>()) {
      err_type_used_as_symbol(v->get_identifier()).fire(v->get_identifier(), cur_f);
    }
    v->mutate()->assign_sym(sym);

    // for global functions, global vars and constants, `import` must exist
    if (!sym->try_as<LocalVarPtr>()) {
      bool allow_no_import = sym->is_builtin() || sym->ident_anchor->range.is_file_id_same_or_stdlib_common(v->range);
      if (!allow_no_import) {
        sym->check_import_exists_when_used_from(cur_f, v);
      }
    }
  }

  void visit(V<ast_dot_access> v) override {
    try {
      parent::visit(v->get_obj());
    } catch (const ThrownParseError&) {
      if (auto v_type_name = v->get_obj()->try_as<ast_reference>()) {
        // for `Point.create` / `int.zero` / `Color.Red`, "undefined symbol" is fired for Point/int/Color
        // suppress this exception till a later pipe, it will be tried to be resolved as a type
        if (v_type_name->get_identifier()->name == "self") {
          err_using_self_not_in_method(cur_f).fire(v_type_name, cur_f);
        }
        return;
      }
      throw;
    }
  }

  void visit(V<ast_braced_expression> v) override {
    current_scope.open_scope();
    parent::visit(v->get_block_statement());
    current_scope.close_scope();
  }

  void visit(V<ast_match_expression> v) override {
    current_scope.open_scope();   // `match (var a = init_val) { ... }`
    parent::visit(v);             // then `a` exists only inside `match` arms
    current_scope.close_scope();
  }

  void visit(V<ast_match_arm> v) override {
    // resolve identifiers after => at first
    visit(v->get_body());
    // because handling lhs of => is comprehensive

    switch (v->pattern_kind) {
      case MatchArmKind::exact_type: {
        if (auto maybe_ident = v->pattern_type_node->try_as<ast_type_leaf_text>()) {
          if (const Symbol* sym = current_scope.lookup_symbol(maybe_ident->text); sym && sym->try_as<GlobalConstPtr>()) {
            auto v_ident = createV<ast_identifier>(maybe_ident->range, sym->name);
            AnyExprV pattern_expr = createV<ast_reference>(v_ident->range, v_ident, nullptr);
            parent::visit(pattern_expr);
            v->mutate()->assign_resolved_pattern(MatchArmKind::const_expression, pattern_expr);
          }
        }
        break;
      }
      case MatchArmKind::const_expression: {
        parent::visit(v->get_pattern_expr());
        break;
      }
      default:
        // for `else` match branch, do nothing: its body was already traversed above
        break;
    }
  }

  void visit(V<ast_block_statement> v) override {
    current_scope.open_scope();
    if (v == cur_f->ast_root->as<ast_function_declaration>()->get_body()) {
      for (int i = 0; i < cur_f->get_num_params(); ++i) {
        LocalVarPtr param_ref = &cur_f->parameters[i];
        current_scope.add_local_var(param_ref);
        if (param_ref->has_default_value()) {
          parent::visit(param_ref->default_value);
        }
      }
    }

    parent::visit(v);
    current_scope.close_scope();
  }

  void visit(V<ast_do_while_statement> v) override {
    current_scope.open_scope();
    parent::visit(v->get_body());
    parent::visit(v->get_cond()); // in 'while' condition it's ok to use variables declared inside do
    current_scope.close_scope();
  }

  void visit(V<ast_try_catch_statement> v) override {
    visit(v->get_try_body());
    current_scope.open_scope();
    const std::vector<AnyExprV>& catch_items = v->get_catch_expr()->get_items();
    tolk_assert(catch_items.size() == 2);
    process_catch_variable(catch_items[1]);
    process_catch_variable(catch_items[0]);
    parent::visit(v->get_catch_body());
    current_scope.close_scope();
  }

  void visit(V<ast_lambda_fun> v) override {
    // we are at `fun() { ... }` expression - a lambda (an anonymous functions);
    // lambdas do not capture anything (neither manually nor automatically), they are not closures;
    // moreover, ast_lambda_fun is a leaf, without direct children (since parameters/body are not expressions);
    // we do not traverse body of a lambda here, because it would be traversed later,
    // when a lambda is registered as a standalone function itself, and that function will travel the pipeline itself;
    // hence, local symbols from a parent scope will not be available, as expected
  }

public:
  bool should_visit_function(FunctionPtr fun_ref) override {
    return fun_ref->is_code_function();
  }

  void on_exit_function(V<ast_function_declaration> v_function) override {
    tolk_assert(current_scope.scopes.empty());
  }

  void start_visiting_constant(GlobalConstPtr const_ref) {
    // `const a = b`, resolve `b`
    parent::visit(const_ref->init_value);
  }

  void start_visiting_struct_fields(StructPtr struct_ref) {
    // field `a: int = C`, resolve `C`
    for (StructFieldPtr field_ref : struct_ref->fields) {
      if (field_ref->has_default_value()) {
        parent::visit(field_ref->default_value);
      }
    }
  }

  void start_visiting_enum_members(EnumDefPtr enum_ref) {
    // member `Red = Another.Blue`, resolve `Another` 
    for (EnumMemberPtr member_ref : enum_ref->members) {
      if (member_ref->has_init_value()) {
        parent::visit(member_ref->init_value);
      }
    }
  }
};

void pipeline_resolve_identifiers_and_assign_symbols() {
  AssignSymInsideFunctionVisitor visitor;
  for (const SrcFile* file : G.all_src_files) {
    for (AnyV v : file->ast->as<ast_tolk_file>()->get_toplevel_declarations()) {
      if (auto v_func = v->try_as<ast_function_declaration>(); v_func && !v_func->is_builtin_function()) {
        tolk_assert(v_func->fun_ref);
        if (visitor.should_visit_function(v_func->fun_ref)) {
          visitor.start_visiting_function(v_func->fun_ref, v_func);
        }

      } else if (auto v_const = v->try_as<ast_constant_declaration>()) {
        tolk_assert(v_const->const_ref);
        visitor.start_visiting_constant(v_const->const_ref);

      } else if (auto v_struct = v->try_as<ast_struct_declaration>()) {
        tolk_assert(v_struct->struct_ref);
        visitor.start_visiting_struct_fields(v_struct->struct_ref);

      } else if (auto v_enum = v->try_as<ast_enum_declaration>()) {
        tolk_assert(v_enum->enum_ref);
        visitor.start_visiting_enum_members(v_enum->enum_ref);
      }
    }
  }
}

void pipeline_resolve_identifiers_and_assign_symbols(FunctionPtr fun_ref) {
  AssignSymInsideFunctionVisitor visitor;
  if (visitor.should_visit_function(fun_ref)) {
    visitor.start_visiting_function(fun_ref, fun_ref->ast_root->as<ast_function_declaration>());
  }
}

void pipeline_resolve_identifiers_and_assign_symbols(StructPtr struct_ref) {
  AssignSymInsideFunctionVisitor().start_visiting_struct_fields(struct_ref);
}

} // namespace tolk
