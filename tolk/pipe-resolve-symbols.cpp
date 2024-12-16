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
#include "src-file.h"
#include "ast.h"
#include "ast-visitor.h"
#include "compiler-state.h"
#include <unordered_map>

/*
 *   This pipe resolves identifiers (local variables) in all functions bodies.
 *   It happens before type inferring, but after all global symbols are registered.
 * It means, that for any symbol `x` we can look up whether it's a global name or not.
 *
 *   Example: `var x = 10; x = 20;` both `x` point to one LocalVarData.
 *   Example: `x = 20` undefined symbol `x` is also here (unless it's a global)
 *   Variables scoping and redeclaration are also here.
 *
 *   As a result of this step, every V<ast_identifier>::sym is filled, pointing either to a local var/parameter,
 * or to a global var / constant / function.
 */

namespace tolk {

static void check_import_exists_when_using_sym(AnyV v_usage, const Symbol* used_sym) {
  SrcLocation sym_loc = used_sym->loc;
  if (!v_usage->loc.is_symbol_from_same_or_builtin_file(sym_loc)) {
    const SrcFile* declared_in = sym_loc.get_src_file();
    bool has_import = false;
    for (const SrcFile::ImportStatement& import_stmt : v_usage->loc.get_src_file()->imports) {
      if (import_stmt.imported_file == declared_in) {
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

  const Symbol* add_local_var(const LocalVarData* v_sym) {
    if (UNLIKELY(scopes.empty())) {
      throw Fatal("unexpected scope_level = 0");
    }
    if (v_sym->name.empty()) {    // underscore
      return v_sym;
    }

    uint64_t key = key_hash(v_sym->name);
    const auto& [_, inserted] = scopes.rbegin()->emplace(key, v_sym);
    if (UNLIKELY(!inserted)) {
      throw ParseError(v_sym->loc, "redeclaration of local variable `" + v_sym->name + "`");
    }
    return v_sym;
  }
};


class AssignSymInsideFunctionVisitor final : public ASTVisitorFunctionBody {
  // more correctly this field shouldn't be static, but currently there is no need to make it a part of state
  static NameAndScopeResolver current_scope;

  static const Symbol* create_local_var_sym(std::string_view name, SrcLocation loc, TypeExpr* var_type, bool immutable) {
    LocalVarData* v_sym = new LocalVarData(static_cast<std::string>(name), loc, -1, var_type);
    if (immutable) {
      v_sym->flags |= LocalVarData::flagImmutable;
    }
    return current_scope.add_local_var(v_sym);
  }

  static void process_catch_variable(AnyV catch_var) {
    if (auto v_ident = catch_var->try_as<ast_identifier>()) {
      const Symbol* sym = create_local_var_sym(v_ident->name, catch_var->loc, TypeExpr::new_hole(), true);
      v_ident->mutate()->assign_sym(sym);
    }
  }

  static void process_function_arguments(const FunctionData* fun_ref, V<ast_argument_list> v, AnyExprV lhs_of_dot_call) {
    int delta_self = lhs_of_dot_call ? 1 : 0;
    int n_arguments = static_cast<int>(v->get_arguments().size()) + delta_self;
    int n_parameters = static_cast<int>(fun_ref->parameters.size());

    // Tolk doesn't have optional parameters currently, so just compare counts
    if (n_parameters < n_arguments) {
      v->error("too many arguments in call to `" + fun_ref->name + "`, expected " + std::to_string(n_parameters - delta_self) + ", have " + std::to_string(n_arguments - delta_self));
    }
    if (n_arguments < n_parameters) {
      v->error("too few arguments in call to `" + fun_ref->name + "`, expected " + std::to_string(n_parameters - delta_self) + ", have " + std::to_string(n_arguments - delta_self));
    }
  }

  void visit(V<ast_local_var> v) override {
    if (v->marked_as_redef) {
      auto v_ident = v->get_identifier()->as<ast_identifier>();
      const Symbol* sym = current_scope.lookup_symbol(v_ident->name);
      if (sym == nullptr) {
        v->error("`redef` for unknown variable");
      }
      if (!sym->try_as<LocalVarData>() && !sym->try_as<GlobalVarData>()) {
        v->error("`redef` for unknown variable");
      }
      v->mutate()->assign_var_ref(sym);
      v_ident->mutate()->assign_sym(sym);
    } else if (auto v_ident = v->get_identifier()->try_as<ast_identifier>()) {
      TypeExpr* var_type = v->declared_type ? v->declared_type : TypeExpr::new_hole();
      const Symbol* sym = create_local_var_sym(v_ident->name, v->loc, var_type, v->is_immutable);
      v->mutate()->assign_var_ref(sym);
      v_ident->mutate()->assign_sym(sym);
    } else {
      // underscore, do nothing, v->sym remains nullptr
    }
  }

  void visit(V<ast_local_vars_declaration> v) override {
    parent::visit(v->get_assigned_val());
    parent::visit(v->get_lhs());
  }

  void visit(V<ast_identifier> v) override {
    const Symbol* sym = current_scope.lookup_symbol(v->name);
    if (!sym) {
      v->error("undefined symbol `" + static_cast<std::string>(v->name) + "`");
    }
    v->mutate()->assign_sym(sym);

    // for global functions, global vars and constants, `import` must exist
    if (!sym->try_as<LocalVarData>()) {
      check_import_exists_when_using_sym(v, sym);
    }
  }

  void visit(V<ast_function_call> v) override {
    parent::visit(v->get_called_f());
    parent::visit(v->get_arg_list());

    // most likely it's a global function, but also may be `some_var(args)` or even `getF()(args)`
    // for such corner cases, sym remains nullptr
    if (auto v_ident = v->get_called_f()->try_as<ast_identifier>()) {
      if (const auto* fun_ref = v_ident->sym->try_as<FunctionData>()) {
        v->mutate()->assign_fun_ref(fun_ref);
        process_function_arguments(fun_ref, v->get_arg_list(), nullptr);
      }
    }
    // for `some_var(args)`, if it's called with wrong arguments count, the error is not here
    // it will be fired later, it's a type checking error
  }

  void visit(V<ast_dot_method_call> v) override {
    const Symbol* sym = lookup_global_symbol(v->method_name);
    if (!sym) {
      v->error("undefined symbol `" + static_cast<std::string>(v->method_name) + "`");
    }
    const auto* fun_ref = sym->try_as<FunctionData>();
    if (!fun_ref) {
      v->error("`" + static_cast<std::string>(v->method_name) + "` is not a method");
    }

    if (fun_ref->parameters.empty()) {
      v->error("`" + static_cast<std::string>(v->method_name) + "` has no parameters and can not be called as method");
    }

    v->mutate()->assign_fun_ref(fun_ref);
    parent::visit(v);
    process_function_arguments(fun_ref, v->get_arg_list(), v->get_obj());
  }

  void visit(V<ast_self_keyword> v) override {
    const Symbol* sym = current_scope.lookup_symbol("self");
    if (!sym) {
      v->error("using `self` in a non-member function (it does not accept the first `self` parameter)");
    }
    v->mutate()->assign_param_ref(sym->as<LocalVarData>());
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
  void start_visiting_function(V<ast_function_declaration> v_function) override {
    auto v_seq = v_function->get_body()->try_as<ast_sequence>();
    tolk_assert(v_seq != nullptr);

    current_scope.open_scope(v_function->loc);

    for (int i = 0; i < v_function->get_num_params(); ++i) {
      current_scope.add_local_var(&v_function->fun_ref->parameters[i]);
      v_function->get_param(i)->mutate()->assign_param_ref(&v_function->fun_ref->parameters[i]);
    }
    parent::visit(v_seq);

    current_scope.close_scope(v_seq->loc_end);
    tolk_assert(current_scope.scopes.empty());
  }
};

NameAndScopeResolver AssignSymInsideFunctionVisitor::current_scope;

void pipeline_resolve_identifiers_and_assign_symbols(const AllSrcFiles& all_src_files) {
  visit_ast_of_all_functions<AssignSymInsideFunctionVisitor>(all_src_files);
}

} // namespace tolk
