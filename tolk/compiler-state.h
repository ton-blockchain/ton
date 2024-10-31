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
#include <set>
#include <string>

namespace tolk {

extern std::string tolk_version;

class GlobalPragma {
  std::string name_;
  bool enabled_ = false;
  const char* deprecated_from_v_ = nullptr;

public:
  explicit GlobalPragma(std::string name) : name_(std::move(name)) { }

  const std::string& name() const { return name_; }

  bool enabled() const { return enabled_; }
  void enable(SrcLocation loc);
  void always_on_and_deprecated(const char* deprecated_from_v);
};

// CompilerSettings contains settings that can be passed via cmd line or (partially) wasm envelope.
// They are filled once at start and are immutable since the compilation started.
struct CompilerSettings {
  enum class FsReadCallbackKind { Realpath, ReadFile };

  using FsReadCallback = std::function<td::Result<std::string>(FsReadCallbackKind, const char*)>;

  int verbosity = 0;
  int optimization_level = 2;
  bool stack_layout_comments = true;

  std::string entrypoint_filename;
  std::string output_filename;
  std::string boc_output_filename;
  std::string stdlib_filename;

  FsReadCallback read_callback;
};

// CompilerState contains a mutable state that is changed while the compilation is going on.
// It's a "global state" of all compilation.
// Historically, in FunC, this global state was spread along many global C++ variables.
// Now, no global C++ variables except `CompilerState G` are present.
struct CompilerState {
  CompilerSettings settings;

  SymTable symbols;
  int scope_level = 0;
  SymDef* sym_def[SymTable::SIZE_PRIME + 1]{};
  SymDef* global_sym_def[SymTable::SIZE_PRIME + 1]{};
  std::vector<std::pair<int, SymDef>> symbol_stack;
  std::vector<SrcLocation> scope_opened_at;

  AllRegisteredSrcFiles all_src_files;

  int glob_func_cnt = 0, glob_var_cnt = 0, const_cnt = 0;
  std::vector<SymDef*> glob_func, glob_vars, glob_get_methods;
  std::set<std::string> prohibited_var_names;

  std::string generated_from;
  GlobalPragma pragma_allow_post_modification{"allow-post-modification"};
  GlobalPragma pragma_compute_asm_ltr{"compute-asm-ltr"};
  GlobalPragma pragma_remove_unused_functions{"remove-unused-functions"};

  bool is_verbosity(int gt_eq) const { return settings.verbosity >= gt_eq; }
};

extern CompilerState G;

}  // namespace tolk
