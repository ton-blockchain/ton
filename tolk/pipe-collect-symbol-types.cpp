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
#include "compiler-state.h"
#include "contract-directive.h"
#include "tolk.h"

/*
 *    todo comment here and all functions below
 */

namespace tolk {

static void collect_from_debug_mark(const DebugMarkInfo& debug_mark) {
  if (const auto* m_enter = std::get_if<DebugMarkEnterFunction>(&debug_mark)) {
    G.symbol_types_pool.register_used_function(m_enter->fun_ref);
  } else if (const auto* m_leave = std::get_if<DebugMarkLeaveFunction>(&debug_mark)) {
    G.symbol_types_pool.register_used_function(m_leave->fun_ref);
  } else if (const auto* m_local = std::get_if<DebugMarkLocalVar>(&debug_mark)) {
    G.symbol_types_pool.register_used_type(m_local->local_ref->declared_type);
  } else if (const auto* m_sc = std::get_if<DebugMarkSmartCast>(&debug_mark)) {
    G.symbol_types_pool.register_used_type(m_sc->smart_cast_type);
  } else if (const auto* m_sg = std::get_if<DebugMarkSetGlob>(&debug_mark)) {
    G.symbol_types_pool.register_used_type(m_sg->glob_ref->declared_type);
  }
}

static void walk_ops_collect_symbol_types(const OpList& ops) {
  for (const std::unique_ptr<Op>& op : ops.list) {
    if (!std::holds_alternative<std::nullptr_t>(op->debug_mark)) {
      collect_from_debug_mark(op->debug_mark);
    }
    walk_ops_collect_symbol_types(op->block0);
    walk_ops_collect_symbol_types(op->block1);
  }
}

void pipeline_collect_symbol_types() {
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

  for (FunctionPtr fun_ref : G.all_functions) {
    if (!fun_ref->is_code_function() || !fun_ref->does_need_codegen()) {
      continue;
    }

    G.symbol_types_pool.register_used_function(fun_ref);
    walk_ops_collect_symbol_types(std::get<FunctionBodyCode*>(fun_ref->body)->code->ops);
  }
}

} // namespace tolk
