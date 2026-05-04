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
#pragma once

#include <string>
#include <vector>
#include <variant>
#include "src-file.h"
#include "type-export-json.h"

namespace tolk {

struct TmpVar;

struct DebugMarkLocation {
  SrcRange range;
};

struct DebugMarkCurrentStack {
  struct StackSlot {
    const TmpVar* ir_var;
    td::RefInt256 int_val;
  };
  
  std::vector<StackSlot> stack_slots;
};

struct DebugMarkEnterFunction {
  FunctionPtr fun_ref;
  bool is_inlined;
  bool is_builtin;
  SrcRange range;           // location of the `f()` call expression
  std::vector<var_idx_t> ir_import;
};

struct DebugMarkLeaveFunction {
  FunctionPtr fun_ref;
  std::vector<var_idx_t> ir_return;
  SrcRange range;           // location of the `return` statement (zero range for void functions)
};

struct DebugMarkLocalVar {
  LocalVarPtr local_ref;    // is_parameter, name, declared_type are taken from there
  std::vector<var_idx_t> ir_slots;
  var_idx_t ir_lazy_slice = -1;  // -1 is "not lazy"; otherwise, IR slot of the deserialized slice
};

struct DebugMarkScopeStart {
  SrcRange range;           // range of the { ... } block, for scope unwinding on exceptions
};

struct DebugMarkScopeEnd {
};

struct DebugMarkSmartCast {
  LocalVarPtr local_ref;
  TypePtr smart_cast_type;      // the narrowed type at this point (may equal declared_type on de-cast)
  std::vector<var_idx_t> ir_slots;
};

struct DebugMarkSetGlob {
  GlobalVarPtr glob_ref;
  std::vector<var_idx_t> ir_slots;
};

typedef std::variant<
  std::nullptr_t,
  DebugMarkLocation,
  DebugMarkCurrentStack,
  DebugMarkEnterFunction,
  DebugMarkLeaveFunction,
  DebugMarkLocalVar,
  DebugMarkScopeStart,
  DebugMarkScopeEnd,
  DebugMarkSmartCast,
  DebugMarkSetGlob
> DebugMarkInfo;

// Collects shared files/types/functions declarations emitted as out.symbolTypes.json.
// Debug marks reference this table by "ty_idx" indices.
class SymbolTypesCollecting {
  JsonTypeExporter json_types;
  std::vector<SrcFilePtr> all_files;
  std::vector<FunctionPtr> used_functions;

public:
  void seed_primitive_types() { json_types.seed_primitive_types(); }
  void register_seen_file(SrcFilePtr src_file);
  void register_used_function(FunctionPtr fun_ref);
  void register_used_type(TypePtr type);

  int get_fun_idx(FunctionPtr fun_ref) const;
  int get_type_idx(TypePtr type) const;

  void to_pretty_json(std::ostream& os) const;
};

// Collects MARK_* metadata emitted as out.debugMarks.json when debug marks are enabled.
class DebugMarksCollecting {
  std::vector<DebugMarkInfo> debug_marks;

public:
  int register_debug_mark(DebugMarkInfo info) {
    debug_marks.push_back(std::move(info));
    int mark_id = static_cast<int>(debug_marks.size()) - 1;
    return mark_id;  // both in Fift and in JSON they start from 0
  }

  void to_pretty_json(std::ostream& os, const SymbolTypesCollecting& symbol_types) const;
};


} // namespace tolk
