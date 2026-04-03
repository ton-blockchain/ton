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

#include "src-file.h"
#include "symtable.h"
#include <unordered_map>

namespace tolk {

class ErrorCollector;  // forward declaration

// CompilerState contains a mutable state that is changed while the compilation is going on.
// It's reset at the start of each tolk_proceed() call.
// Note: objects allocated with `new` during previous compilations (SrcFile, Symbol, etc.)
// are intentionally leaked — proper lifetime management (memory arena) is a separate future task.
struct CompilerState {
  GlobalSymbolTable symtable;

  std::vector<FunctionPtr> all_builtins;        // all built-in functions
  std::vector<FunctionPtr> all_functions;       // all user-defined (not built-in) global-scope functions, with generic instantiations, with lambdas
  std::vector<FunctionPtr> all_methods;         // all user-defined and built-in extension methods for arbitrary types (receivers)
  std::vector<GlobalVarPtr> all_global_vars;
  std::vector<GlobalConstPtr> all_constants;
  std::vector<StructPtr> all_structs;
  std::vector<EnumDefPtr> all_enums;
  AllRegisteredSrcFiles all_src_files;

  ErrorCollector* error_collector = nullptr;  // when set, errors can be collected instead of thrown

  int last_type_id = 128;                            // below 128 reserved for built-in types
  std::unordered_map<TypePtr, int> map_type_to_id;   // for assign_type_id() in type-system.cpp
};

// G is the per-compilation mutable state, reset before each compilation pipeline
extern thread_local CompilerState G;

struct ThrownParseError;

struct TolkCompilationResult {
  std::vector<ThrownParseError> errors;
  std::string fatal_msg;      // some Fatal happened, it has no location and can't be pretty formatted
  std::string fift_code;      // fift code exists only if no compilation errors
};

// starts all the compilation pipeline, called from tolk-main and tolk-wasm
TolkCompilationResult tolk_proceed(const std::string &entrypoint_filename);

}  // namespace tolk
