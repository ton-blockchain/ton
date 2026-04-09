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
#include "type-export-json.h"

namespace tolk {

struct SourceMapForOutput {
  JsonTypeExporter json_types;
  std::vector<SrcFilePtr> used_files;
  std::vector<FunctionPtr> used_functions;

  void register_used_file(SrcRange range) {
    tolk_assert(range.is_valid());
    SrcFilePtr file_ref = range.get_src_file();
    auto it = std::find(used_files.begin(), used_files.end(), file_ref);
    if (it == used_files.end()) {
      used_files.push_back(file_ref);
    }
  }

  void register_used_function(FunctionPtr fun_ref) {
    tolk_assert(!fun_ref->is_generic_function());
    auto it = std::find(used_functions.begin(), used_functions.end(), fun_ref);
    if (it == used_functions.end()) {
      used_functions.push_back(fun_ref);
      if (fun_ref->is_code_function()) {
        register_used_file(fun_ref->ident_anchor->range);
      }
      json_types.register_used_type(fun_ref->inferred_return_type);
      for (int i = 0; i < fun_ref->get_num_params(); ++i) {
        json_types.register_used_type(fun_ref->get_param(i).declared_type);
      }
    }
  }

  int get_fun_idx(FunctionPtr fun_ref) const {
    for (int i = 0; i < static_cast<int>(used_functions.size()); ++i) {
      if (used_functions[i] == fun_ref) {
        return i;
      }
    }
    tolk_assert(false);
  }
};

static SrcRange get_function_body_end(FunctionPtr fun_ref) {
  return SrcRange::span_at_end(fun_ref->ast_root->range, 1);
}

static void to_json(JsonPrettyOutput& out, SrcRange range) {
  SrcRange::DecodedRange r = range.decode_offsets();
  out << '['
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
  for (DebugMarkCurrentStack::StackSlot slot : stack.stack_slots) {
    if (!first) out << ',' << ' ';
    first = false;
    out << slot.ir_var->ir_idx;
  }
  out << ']';
}


void SourceMapCollecting::to_pretty_json(std::ostream& os) const {
  JsonPrettyOutput json(os);
  json.start_object();

  SourceMapForOutput total;
  total.json_types.seed_primitive_types();

  for (const DebugMarkInfo& mark : debug_marks) {
    if (const DebugMarkEnterFunction* m_enter = std::get_if<DebugMarkEnterFunction>(&mark)) {
      total.register_used_function(m_enter->fun_ref);
    } else if (const DebugMarkLocalVar* m_local = std::get_if<DebugMarkLocalVar>(&mark)) {
      total.json_types.register_used_type(m_local->local_ref->declared_type);
    } else if (const DebugMarkSmartCast* m_sc = std::get_if<DebugMarkSmartCast>(&mark)) {
      total.json_types.register_used_type(m_sc->smart_cast_type);
    } else if (const DebugMarkSetGlob* m_sg = std::get_if<DebugMarkSetGlob>(&mark)) {
      total.json_types.register_used_type(m_sg->glob_ref->declared_type);
    }
  }

  json.start_array("files");
  for (SrcFilePtr src_file : total.used_files) {
    json.start_object();
    json.key_value("file_id", src_file->file_id);
    json.key_value("file_name", src_file->realpath);
    json.key_value("size_chars", src_file->text.size());
    json.end_object();
  }
  json.end_array();

  total.json_types.emit_declarations_json(json, {.emit_ident_loc = true});

  total.json_types.emit_unique_ty_json(json);

  json.start_array("global_vars");
  json.end_array();

  int idx = 0;
  json.start_array("functions");
  for (FunctionPtr fun_ref : total.used_functions) {
    json.start_object();
    json.key_value("f_idx", idx++);
    json.key_value("name", fun_ref->name);
    json.key_value("return_ty_idx", total.json_types.get_type_idx(fun_ref->inferred_return_type));
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
      json.start_object();
      json.key_value("name", p.name);
      json.key_value("ty_idx", total.json_types.get_type_idx(p.declared_type));
      json.end_object();
    }
    json.end_array();
    json.end_object();
  }
  json.end_array();

  idx = 0;
  json.start_array("debug_marks");
  for (const DebugMarkInfo& mark : debug_marks) {
    json.start_object();
    json.key_value("mark_id", idx++);   // both in Fift and in JSON they start from 0
    if (const DebugMarkLocation* m_loc = std::get_if<DebugMarkLocation>(&mark)) {
      json.key_value("kind", "loc");
      json.key_value("range", m_loc->range);
    } else if (const DebugMarkCurrentStack* m_stack = std::get_if<DebugMarkCurrentStack>(&mark)) {
      json.key_value("kind", "stack");
      json.key_value("stack", *m_stack);
    } else if (const DebugMarkEnterFunction* m_enter = std::get_if<DebugMarkEnterFunction>(&mark)) {
      json.key_value("kind", "enter_fun");
      json.key_value("f_idx", total.get_fun_idx(m_enter->fun_ref));
      json.key_value("f_name", m_enter->fun_ref->name);
      json.key_value("is_inlined", m_enter->is_inlined);
      json.key_value("is_builtin", m_enter->is_builtin);
      json.key_value("range", m_enter->range);
      json.key_value("ir_import", m_enter->ir_import);
    } else if (const DebugMarkLeaveFunction* m_leave = std::get_if<DebugMarkLeaveFunction>(&mark)) {
      json.key_value("kind", "leave_fun");
      json.key_value("f_idx", total.get_fun_idx(m_leave->fun_ref));
      json.key_value("f_name", m_leave->fun_ref->name);
      json.key_value("ir_return", m_leave->ir_return);
      json.key_value("range", m_leave->range);
    } else if (const DebugMarkLocalVar* m_local = std::get_if<DebugMarkLocalVar>(&mark)) {
      json.key_value("kind", "var");
      json.key_value("var_name", m_local->local_ref->name);
      json.key_value("is_parameter", m_local->local_ref->is_parameter());
      json.key_value("ty_idx", total.json_types.get_type_idx(m_local->local_ref->declared_type));
      json.key_value("ir_slots", m_local->ir_slots);
      if (m_local->is_lazy) {
        json.key_value("is_lazy", true);
      }
    } else if (const DebugMarkScopeStart* m_scope = std::get_if<DebugMarkScopeStart>(&mark)) {
      json.key_value("kind", "scope_start");
      json.key_value("range", m_scope->range);
    } else if (std::get_if<DebugMarkScopeEnd>(&mark)) {
      json.key_value("kind", "scope_end");
    } else if (const DebugMarkSmartCast* m_sc = std::get_if<DebugMarkSmartCast>(&mark)) {
      json.key_value("kind", "smart_cast");
      json.key_value("var_name", m_sc->local_ref->name);
      json.key_value("ty_idx", total.json_types.get_type_idx(m_sc->smart_cast_type));
      json.key_value("ir_slots", m_sc->ir_slots);
    } else if (const DebugMarkSetGlob* m_sg = std::get_if<DebugMarkSetGlob>(&mark)) {
      json.key_value("kind", "set_glob");
      json.key_value("glob_name", m_sg->glob_ref->name);
      json.key_value("ty_idx", total.json_types.get_type_idx(m_sg->glob_ref->declared_type));
      json.key_value("ir_slots", m_sg->ir_slots);
    } else {
      tolk_assert(false);
    }
    json.end_object();
  }
  json.end_array();

  json.end_object();
}

void pipeline_collect_source_maps_output(std::ostream& os) {
  G.source_map.to_pretty_json(os);
}

} // namespace tolk
