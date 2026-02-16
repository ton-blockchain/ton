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

#include "td/utils/Status.h"
#include <functional>
#include <string>
#include <vector>

namespace tolk {

// Custom path mappings that allow imports "@third_party/utils", mapped to "/absolute/folder/utils".
// Each mapping is appended by a cmd line option, they are resolved before calculating realpath.
// Note, that in wasm (in tolk-js), path mappings are handled in a different way, in a JS resolver.
struct CompilerPathMapping {
  std::string at_prefix;    // "@third_party"
  std::string abs_folder;   // "/absolute/folder"
};

// CompilerSettings contains settings that can be passed via cmd line or (partially) wasm envelope.
// They are filled once at start and are immutable since the compilation started.
struct CompilerSettings {
  enum class FsReadCallbackKind { Realpath, ReadFile };

  using FsReadCallback = std::function<td::Result<std::string>(FsReadCallbackKind, const char*, void* callback_payload)>;

  int verbosity = 0;
  int optimization_level = 2;
  bool stack_layout_comments = true;
  bool tolk_src_as_line_comments = true;
  bool show_errors_as_json = false;
  bool check_only_no_output = false;
  bool allow_no_entrypoint = false;

  std::string output_filename;
  std::string boc_output_filename;
  std::string stdlib_folder;    // path to tolk-stdlib/; note: from tolk-js it's empty! tolk-js reads files via js callback

  std::vector<CompilerPathMapping> path_mappings;    // "@third_party" to "/absolute/folder"

  FsReadCallback read_callback;
  void* callback_payload = nullptr;

  bool parse_path_mapping_cmd_arg(const std::string& cmd_arg);
  std::string_view get_path_mapping(std::string_view at_prefix) const;
};

// G_settings is filled by tolk-main or tolk-wasm before starting compilation
extern thread_local CompilerSettings G_settings;

}  // namespace tolk
