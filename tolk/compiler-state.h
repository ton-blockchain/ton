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
#include "td/utils/Status.h"
#include <functional>
#include <set>
#include <string>

namespace tolk {

// with cmd option -x, the user can pass experimental options to use
class ExperimentalOption {
  friend struct CompilerSettings;

  const std::string_view name;
  bool enabled = false;
  const char* deprecated_from_v = nullptr;  // when an option becomes deprecated (after the next compiler release),
  const char* deprecated_reason = nullptr;  // but the user still passes it, we'll warn to stderr

public:
  explicit ExperimentalOption(std::string_view name) : name(name) {}

  void mark_deprecated(const char* deprecated_from_v, const char* deprecated_reason);

  explicit operator bool() const { return enabled; }
};

// CompilerSettings contains settings that can be passed via cmd line or (partially) wasm envelope.
// They are filled once at start and are immutable since the compilation started.
struct CompilerSettings {
  enum class FsReadCallbackKind { Realpath, ReadFile };

  using FsReadCallback = std::function<td::Result<std::string>(FsReadCallbackKind, const char*)>;

  int verbosity = 0;
  int optimization_level = 2;
  bool stack_layout_comments = true;
  bool tolk_src_as_line_comments = true;

  std::string output_filename;
  std::string boc_output_filename;
  std::string stdlib_folder;    // a path to tolk-stdlib/; files imported via @stdlib/xxx are there

  FsReadCallback read_callback;

  ExperimentalOption remove_unused_functions{"remove-unused-functions"};

  void enable_experimental_option(std::string_view name);
  void parse_experimental_options_cmd_arg(const std::string& cmd_arg);
};

// AST nodes contain std::string_view referencing to contents of .tolk files (kept in memory after reading).
// It's more than enough, except a situation when we create new AST nodes inside the compiler
// and want some "persistent place" for std::string_view to point to.
// This class copies strings to heap, so that they remain valid after closing scope.
class PersistentHeapAllocator {
  struct ChunkInHeap {
    const char* allocated;
    std::unique_ptr<ChunkInHeap> next;

    ChunkInHeap(const char* allocated, std::unique_ptr<ChunkInHeap>&& next)
      : allocated(allocated), next(std::move(next)) {}
  };

  std::unique_ptr<ChunkInHeap> head = nullptr;

public:
  std::string_view copy_string_to_persistent_memory(std::string_view str_in_tmp_memory);
  void clear();
};

// CompilerState contains a mutable state that is changed while the compilation is going on.
// It's a "global state" of all compilation.
// Historically, in FunC, this global state was spread along many global C++ variables.
// Now, no global C++ variables except `CompilerState G` are present.
struct CompilerState {
  CompilerSettings settings;

  GlobalSymbolTable symtable;
  PersistentHeapAllocator persistent_mem;

  std::vector<FunctionPtr> all_functions;       // all user-defined (not built-in) global-scope functions, with generic instantiations
  std::vector<FunctionPtr> all_methods;         // all user-defined and built-in extension methods for arbitrary types (receivers)
  std::vector<FunctionPtr> all_contract_getters;
  std::vector<GlobalVarPtr> all_global_vars;
  std::vector<GlobalConstPtr> all_constants;
  std::vector<StructPtr> all_structs;
  AllRegisteredSrcFiles all_src_files;

  bool is_verbosity(int gt_eq) const { return settings.verbosity >= gt_eq; }
};

extern CompilerState G;

}  // namespace tolk
