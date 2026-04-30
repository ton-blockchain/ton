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
#include "compiler-state.h"
#include "contract-directive.h"
#include "generics-helpers.h"
#include "tolk.h"
#include <algorithm>

/*
 *    todo comment here and all functions below
 */

namespace tolk {

static void add_function_to_visit(std::vector<FunctionPtr>& functions_to_visit, FunctionPtr fun_ref) {
  if (!fun_ref || !fun_ref->is_code_function() || fun_ref->is_generic_function()) {
    return;
  }
  if (std::find(functions_to_visit.begin(), functions_to_visit.end(), fun_ref) == functions_to_visit.end()) {
    functions_to_visit.push_back(fun_ref);
  }
}

static void collect_from_debug_mark(const DebugMarkInfo& debug_mark, std::vector<FunctionPtr>& functions_to_visit) {
  if (const auto* m_enter = std::get_if<DebugMarkEnterFunction>(&debug_mark)) {
    G.symbol_types_pool.register_used_function(m_enter->fun_ref);
    add_function_to_visit(functions_to_visit, m_enter->fun_ref);
  } else if (const auto* m_leave = std::get_if<DebugMarkLeaveFunction>(&debug_mark)) {
    G.symbol_types_pool.register_used_function(m_leave->fun_ref);
    add_function_to_visit(functions_to_visit, m_leave->fun_ref);
  } else if (const auto* m_local = std::get_if<DebugMarkLocalVar>(&debug_mark)) {
    G.symbol_types_pool.register_used_type(m_local->local_ref->declared_type);
  } else if (const auto* m_sc = std::get_if<DebugMarkSmartCast>(&debug_mark)) {
    G.symbol_types_pool.register_used_type(m_sc->smart_cast_type);
  } else if (const auto* m_sg = std::get_if<DebugMarkSetGlob>(&debug_mark)) {
    G.symbol_types_pool.register_used_type(m_sg->glob_ref->declared_type);
  }
}

static void walk_ops_collect_symbol_types(const OpList& ops, std::vector<FunctionPtr>& functions_to_visit) {
  for (const std::unique_ptr<Op>& op : ops.list) {
    if (!std::holds_alternative<std::nullptr_t>(op->debug_mark)) {
      collect_from_debug_mark(op->debug_mark, functions_to_visit);
    }
    walk_ops_collect_symbol_types(op->block0, functions_to_visit);
    walk_ops_collect_symbol_types(op->block1, functions_to_visit);
  }
}

static bool is_type_abi_json_reflection_call(FunctionPtr fun_ref) {
  if (!fun_ref || !fun_ref->substitutedTs) {
    return false;
  }
  FunctionPtr base_fun_ref = fun_ref->base_fun_ref ? fun_ref->base_fun_ref : fun_ref;
  return base_fun_ref->name == "reflect.typeAbiJsonOf" || base_fun_ref->name == "reflect.typeAbiJsonOfObject";
}

class CollectReflectionTypesVisitor final : public ASTVisitorFunctionBody {
  const std::vector<FunctionPtr>& functions_to_visit;

  void visit(V<ast_function_call> v) override {
    parent::visit(v);

    if (is_type_abi_json_reflection_call(v->fun_maybe)) {
      TypePtr typeT = v->fun_maybe->substitutedTs->typeT_at(0);
      G.symbol_types_pool.register_used_type(typeT);
    }
  }

public:
  explicit CollectReflectionTypesVisitor(const std::vector<FunctionPtr>& functions_to_visit)
    : functions_to_visit(functions_to_visit) {
  }

  bool should_visit_function(FunctionPtr fun_ref) override {
    return std::find(functions_to_visit.begin(), functions_to_visit.end(), fun_ref) != functions_to_visit.end();
  }

  void start_visiting_expression(AnyExprV expr) {
    if (expr) {
      parent::visit(expr);
    }
  }
};

void pipeline_collect_symbol_types() {
  G.symbol_types_pool.seed_primitive_types();

  SrcFilePtr entrypoint_file = G.all_src_files.get_entrypoint_file();
  if (entrypoint_file->has_contract_directive()) {
    const ContractDirective* c = entrypoint_file->contract_directive;
    if (c->incomingMessages) {
      G.symbol_types_pool.register_used_type(c->incomingMessages->resolved_type);
    }
    if (c->incomingExternal) {
      G.symbol_types_pool.register_used_type(c->incomingExternal->resolved_type);
    }
    if (c->storage) {
      G.symbol_types_pool.register_used_type(c->storage->resolved_type);
    }
    if (c->storageAtDeployment) {
      G.symbol_types_pool.register_used_type(c->storageAtDeployment->resolved_type);
    }
    if (c->forceAbiExport) {
      G.symbol_types_pool.register_used_type(c->forceAbiExport->resolved_type);
    }
  }

  std::vector<FunctionPtr> functions_to_visit;

  for (FunctionPtr fun_ref : G.all_functions) {
    if (!fun_ref->is_code_function() || !fun_ref->does_need_codegen()) {
      continue;
    }

    G.symbol_types_pool.register_used_function(fun_ref);
    add_function_to_visit(functions_to_visit, fun_ref);
    walk_ops_collect_symbol_types(std::get<FunctionBodyCode*>(fun_ref->body)->code->ops, functions_to_visit);
  }

  CollectReflectionTypesVisitor visitor(functions_to_visit);
  visit_ast_of_all_functions(visitor);

  for (FunctionPtr fun_ref : functions_to_visit) {
    for (const LocalVarData& param_ref : fun_ref->parameters) {
      if (param_ref.has_default_value()) {
        visitor.start_visiting_expression(param_ref.default_value);
      }
    }
  }

  for (GlobalConstPtr const_ref : get_all_declared_constants()) {
    visitor.start_visiting_expression(const_ref->init_value);
  }
}

} // namespace tolk
