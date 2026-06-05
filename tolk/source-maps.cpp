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
#include "source-maps.h"
#include "ast.h"
#include "tolk.h"
#include "compiler-state.h"
#include <algorithm>

/*
 *   Source maps in Tolk make it possible to debug optimized TVM bytecode without
 *   changing the bytecode itself. The compiler still inlines functions, folds
 * constants, and emits production Fift; the source-map artifacts only
 * describe how the original program can be replayed afterwards.
 *
 *   There are two JSON layers.
 *   - `out.symbolTypes.json` is emitted by default. It contains the shared tables
 *     of source files, declarations, unique types, and reachable functions. These
 *     tables are also useful outside the debugger — for example, for `println` in Acton.
 *   - `out.debugMarks.json` is emitted only with `--emit-debug-marks`. It contains
 *     a sequential list of semantic marks. Marks refer back to symbol types by
 *     `f_idx` and `ty_idx`, so debug marks require symbol types to be present.
 *
 *   This file serializes both layers. It does not decide where marks are placed:
 *   AST-to-IR conversion emits marks for function enter/leave, locals, scopes,
 * smart casts, and global writes; codegen emits current stack layouts; Fift output
 * inserts location marks from `Op::origin` before real asm instructions. The same
 * `Op::origin->range` is also used for human line comments in generated Fift.
 *
 *   When debug marks are enabled, `pipe-generate-fif-output.cpp` assigns `mark_id`,
 * stores the C++ `DebugMarkInfo` here, and prints pseudo-instructions like
 * `12 MARK_LOC` or `13 MARK_STACK` into Fift. Asm.fif consumes them as metadata.
 *
 *   Asm.fif records where each mark lands in assembler coordinates: cell hash plus
 * bit offset inside that cell. It writes this lookup as `out.debugMarks.boc`.
 * Method dictionary cells need an extra hash/offset remap later, because TVM sees
 * the code under a hashmap label, not exactly the raw cell hashed by the assembler.
 *
 *   The replayer (debugger) combines four inputs: symbol types JSON, debug marks
 * JSON and BOC, and a VM execution log. For each `(cell_hash, bit_offset)` reached
 * by TVM, it expands all mark ids at that point into ticks: enter or leave a frame,
 * update current source location, refresh the IR-to-stack layout, etc.
 *
 *   The central abstraction is the IR slot. Tolk values are numbered as IR
 * variables, and `MARK_STACK` describes which IR slots currently correspond to the
 * visible part of the TVM stack. Since every variable has a type in symbol types,
 * the replayer can match raw stack values back to named locals and render them in
 * a debugger, including values from inlined calls and "last seen" locals.
 *
 *   Because marks describe compiler intent rather than changing execution, source
 * maps keep working with optimized contracts: stepping can enter inlined functions,
 * variables can be reconstructed from the stack, and the final code stays the same
 * code that will run on chain.
 *
 *   For implementation of debugger/replayer, proceed to Acton repo, search `TolkReplayer`:
 * https://github.com/ton-blockchain/acton.
 */

namespace tolk {

static SrcRange get_function_body_end(FunctionPtr fun_ref) {
  return SrcRange::span_at_end(fun_ref->ast_root->range, 1);
}

static void to_json(JsonPrettyOutput& json, SrcRange range) {
  SrcRange::DecodedRange r = range.decode_offsets();
  json << '['
      << r.file_id << ',' << ' '
      << r.start_line_no << ',' << r.start_char_no << ',' << ' '
      << r.end_line_no << ',' << r.end_char_no
      << ']';
}

static void to_json(JsonPrettyOutput& out, const std::vector<var_idx_t>& ir_idx_arr) {
  out << '[';
  bool first = true;
  for (var_idx_t ir_idx : ir_idx_arr) {
    if (!first) out << ',' << ' ';
    first = false;
    out << ir_idx;
  }
  out << ']';
}

static void to_json(JsonPrettyOutput& out, const DebugMarkCurrentStack& stack) {
  out << '[';
  bool first = true;
  for (const DebugMarkCurrentStack::StackSlot& slot : stack.stack_slots) {
    if (!first) out << ',' << ' ';
    first = false;
    out << slot.ir_var->ir_idx;
  }
  out << ']';
}

void SymbolTypesCollecting::register_seen_file(SrcFilePtr src_file) {
  all_files.push_back(src_file);
}

void SymbolTypesCollecting::register_used_type(TypePtr type) {
  json_types.register_used_type(type);
}

void SymbolTypesCollecting::register_used_function(FunctionPtr fun_ref) {
  tolk_assert(!fun_ref->is_generic_function());
  auto it = std::find(used_functions.begin(), used_functions.end(), fun_ref);
  if (it == used_functions.end()) {
    used_functions.push_back(fun_ref);
    register_used_type(fun_ref->inferred_return_type);
    for (int i = 0; i < fun_ref->get_num_params(); ++i) {
      register_used_type(fun_ref->get_param(i).declared_type);
    }
  }
}

int SymbolTypesCollecting::get_fun_idx(FunctionPtr fun_ref) const {
  for (int i = 0; i < static_cast<int>(used_functions.size()); ++i) {
    if (used_functions[i] == fun_ref) {
      return i;
    }
  }
  tolk_assert(false);
}

int SymbolTypesCollecting::get_type_idx(TypePtr type) const {
  return json_types.get_type_idx(type);
}

void SymbolTypesCollecting::to_pretty_json(std::ostream& os) const {
  JsonPrettyOutput json(os);
  json.start_object();

  json.start_array("files");
  for (SrcFilePtr src_file : all_files) {
    json.next_array_item();
    json.start_object();
    json.key_value("file_id", src_file->file_id);
    json.key_value("file_name", src_file->realpath);
    json.key_value("size_chars", src_file->text.size());
    json.start_array("imports");
    for (const SrcFile::ImportDirective& import : src_file->imports) {
      json.next_array_item();
      json.write_value(import.imported_file->file_id);
    }
    json.end_array();
    json.end_object();
  }
  json.end_array();

  json_types.emit_unique_ty_and_declarations_json(json, {.emit_ident_loc = true});

  int idx = 0;
  json.start_array("functions");
  for (FunctionPtr fun_ref : used_functions) {
    json.next_array_item();
    json.start_object();
    json.key_value("f_idx", idx++);
    json.key_value("name", fun_ref->name);
    json.key_value("return_ty_idx", json_types.get_type_idx(fun_ref->inferred_return_type));
    json.key_value("num_params", fun_ref->get_num_params());
    if (fun_ref->is_code_function()) {
      json.key_value("ident_loc", fun_ref->ident_anchor->range);
      json.key_value("end_loc", get_function_body_end(fun_ref));
    } else {
      json.key_value("ident_loc", JsonPrettyOutput::Unquoted{"[0,0,0,0,0]"});
      json.key_value("end_loc", JsonPrettyOutput::Unquoted{"[0,0,0,0,0]"});
    }
    json.start_array("params");
    for (int i = 0; i < fun_ref->get_num_params(); ++i) {
      const LocalVarData& p = fun_ref->get_param(i);
      json.next_array_item();
      json.start_object();
      json.key_value("name", p.name);
      json.key_value("ty_idx", json_types.get_type_idx(p.declared_type));
      json.end_object();
    }
    json.end_array();
    json.end_object();
  }
  json.end_array();

  json.end_object();
}

void DebugMarksCollecting::to_pretty_json(std::ostream& os, const SymbolTypesCollecting& symbol_types) const {
  JsonPrettyOutput json(os);

  int mark_id = 0;
  json.start_array();
  for (const DebugMarkInfo& mark : debug_marks) {
    json.next_array_item();
    json.start_object();
    json.key_value("mark_id", mark_id++);   // both in Fift and in JSON they start from 0
    if (const DebugMarkLocation* m_loc = std::get_if<DebugMarkLocation>(&mark)) {
      json.key_value("kind", "loc");
      json.key_value("range", m_loc->range);
    } else if (const DebugMarkCurrentStack* m_stack = std::get_if<DebugMarkCurrentStack>(&mark)) {
      json.key_value("kind", "stack");
      json.key_value("stack", *m_stack);
    } else if (const DebugMarkEnterFunction* m_enter = std::get_if<DebugMarkEnterFunction>(&mark)) {
      json.key_value("kind", "enter_fun");
      json.key_value("f_idx", symbol_types.get_fun_idx(m_enter->fun_ref));
      json.key_value("f_name", m_enter->fun_ref->name);
      json.key_value("is_inlined", m_enter->is_inlined);
      json.key_value("is_builtin", m_enter->is_builtin);
      json.key_value("range", m_enter->range);
      json.key_value("ir_import", m_enter->ir_import);
    } else if (const DebugMarkLeaveFunction* m_leave = std::get_if<DebugMarkLeaveFunction>(&mark)) {
      json.key_value("kind", "leave_fun");
      json.key_value("f_idx", symbol_types.get_fun_idx(m_leave->fun_ref));
      json.key_value("f_name", m_leave->fun_ref->name);
      json.key_value("ir_return", m_leave->ir_return);
      json.key_value("range", m_leave->range);
    } else if (const DebugMarkLocalVar* m_local = std::get_if<DebugMarkLocalVar>(&mark)) {
      json.key_value("kind", "var");
      json.key_value("var_name", m_local->local_ref->name);
      json.key_value("is_parameter", m_local->local_ref->is_parameter());
      json.key_value("ty_idx", symbol_types.get_type_idx(m_local->local_ref->declared_type));
      json.key_value("ir_slots", m_local->ir_slots);
      if (m_local->ir_lazy_slice != -1) {
        json.key_value("ir_lazy_slice", m_local->ir_lazy_slice);
      }
    } else if (const DebugMarkScopeStart* m_scope = std::get_if<DebugMarkScopeStart>(&mark)) {
      json.key_value("kind", "scope_start");
      json.key_value("range", m_scope->range);
    } else if (std::get_if<DebugMarkScopeEnd>(&mark)) {
      json.key_value("kind", "scope_end");
    } else if (const DebugMarkSmartCast* m_sc = std::get_if<DebugMarkSmartCast>(&mark)) {
      json.key_value("kind", "smart_cast");
      json.key_value("var_name", m_sc->local_ref->name);
      json.key_value("ty_idx", symbol_types.get_type_idx(m_sc->smart_cast_type));
      json.key_value("ir_slots", m_sc->ir_slots);
    } else if (const DebugMarkSetGlob* m_sg = std::get_if<DebugMarkSetGlob>(&mark)) {
      json.key_value("kind", "set_glob");
      json.key_value("glob_name", m_sg->glob_ref->name);
      json.key_value("ty_idx", symbol_types.get_type_idx(m_sg->glob_ref->declared_type));
      json.key_value("ir_slots", m_sg->ir_slots);
    } else {
      tolk_assert(false);
    }
    json.end_object();
  }
  json.end_array();
}

} // namespace tolk
