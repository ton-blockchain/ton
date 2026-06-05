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
 *   Walk all reachable code and fill `G.symbol_types_pool` for `out.symbolTypes.json` artifact.
 *   This artifact is emitted always, even if debug marks are not. It's used by Acton toolchain
 * for `println`, FFI assertions, and other type-dependent work.
 *
 *   Note, that we traverse not AST, but IR code already generated from AST.
 *   After this pipe, we have symbol types filled:
 *   - all used functions, with their prototypes
 *   - all unique types and unique declarations inside them
 *   The resulting `out.symbolTypes.json` contains files, functions, unique types, generic monomorphisations.
 * Its core (JsonTypeExporter) is common with ABI export (`out.abi.json` also contains unique types and generics),
 * but they are different artifacts for different purposes:
 *   - out.abi.json is "how the contract is seen by the outer world, how to encode messages and call get methods"
 *   - out.symbolTypes.json is for the toolchain (println/format, build cache using import files, etc.)
 *   - out.debugMarks.json is for the debugger and references ty_idx from symbol types
 */

namespace tolk {

static void collect_from_debug_mark(const DebugMarkInfo& debug_mark) {
  if (const auto* m_enter = std::get_if<DebugMarkEnterFunction>(&debug_mark)) {
    G.symbol_types_pool.register_used_function(m_enter->fun_ref);
  } else if (const auto* m_local = std::get_if<DebugMarkLocalVar>(&debug_mark)) {
    G.symbol_types_pool.register_used_type(m_local->local_ref->declared_type);
  } else if (const auto* m_sc = std::get_if<DebugMarkSmartCast>(&debug_mark)) {
    G.symbol_types_pool.register_used_type(m_sc->smart_cast_type);
  } else if (const auto* m_sg = std::get_if<DebugMarkSetGlob>(&debug_mark)) {
    G.symbol_types_pool.register_used_type(m_sg->glob_ref->declared_type);
  }
}

static void walk_ops_collect_symbol_types(const OpList& ops) {
  for (const auto& op : ops.list) {
    if (!std::holds_alternative<std::nullptr_t>(op->debug_mark)) {
      collect_from_debug_mark(op->debug_mark);
    }
    walk_ops_collect_symbol_types(op->block0);
    walk_ops_collect_symbol_types(op->block1);
  }
}

static void register_contract_directive_type(AnyTypeV type_node) {
  if (type_node) {
    G.symbol_types_pool.register_used_type(type_node->resolved_type);
  }
}

void pipeline_collect_symbol_types() {
  SrcFilePtr entrypoint_file = G.all_src_files.get_entrypoint_file();
  if (entrypoint_file->has_contract_directive()) {
    const ContractDirective* c = entrypoint_file->contract_directive;
    register_contract_directive_type(c->incomingMessages);
    register_contract_directive_type(c->incomingExternal);
    register_contract_directive_type(c->outgoingMessages);
    register_contract_directive_type(c->emittedEvents);
    register_contract_directive_type(c->thrownErrors);
    register_contract_directive_type(c->storage);
    register_contract_directive_type(c->storageAtDeployment);
    register_contract_directive_type(c->forceAbiExport);
  }

  for (SrcFilePtr src_file : G.all_src_files) {
    G.symbol_types_pool.register_seen_file(src_file);
  }

  for (FunctionPtr fun_ref : G.all_functions) {
    if (fun_ref->does_need_codegen() && fun_ref->is_code_function()) {
      G.symbol_types_pool.register_used_function(fun_ref);
      walk_ops_collect_symbol_types(std::get<FunctionBodyCode*>(fun_ref->body)->code->ops);
    }
  }
}

} // namespace tolk
