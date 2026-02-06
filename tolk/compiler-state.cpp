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
#include "compiler-state.h"
#include "compiler-settings.h"
#include <iostream>

namespace tolk {

bool CompilerSettings::parse_path_mapping_cmd_arg(const std::string& cmd_arg) {
  std::string::size_type pos = cmd_arg.find('=');
  if (!cmd_arg.starts_with('@') || pos <= 1 || pos >= cmd_arg.size() - 1) {
    std::cerr << "invalid path mapping: expected format: @custom-path=/absolute/folder" << std::endl;
    return false;
  }

  std::string at_prefix(cmd_arg.c_str(), pos);
  std::string abs_folder(cmd_arg.c_str() + pos + 1);
  if (at_prefix.find_first_of("/\\") != std::string::npos) {
    std::cerr << "invalid path mapping: alias must not contain path separators, got " << at_prefix << std::endl;
    return false;
  }
  if (!get_path_mapping(at_prefix).empty() || at_prefix == "@stdlib") {
    std::cerr << "invalid path mapping: duplicated " << at_prefix << std::endl;
    return false;
  }

  // normalize "@alias=path/" to "@alias=path"
  while (abs_folder.size() > 1 && (abs_folder.back() == '/' || abs_folder.back() == '\\')) {
    abs_folder.pop_back();
  }

  path_mappings.emplace_back(CompilerPathMapping{std::move(at_prefix), std::move(abs_folder)});
  return true;
}

std::string_view CompilerSettings::get_path_mapping(std::string_view at_prefix) const {
  for (const CompilerPathMapping& m : path_mappings) {
    if (m.at_prefix == at_prefix) {
      return m.abs_folder;
    }
  }
  return "";
}

const std::vector<FunctionPtr>& get_all_builtin_functions() {
  return G.all_builtins;
}

const std::vector<FunctionPtr>& get_all_not_builtin_functions() {
  return G.all_functions;
}

const std::vector<GlobalVarPtr>& get_all_declared_global_vars() {
  return G.all_global_vars;
}

const std::vector<GlobalConstPtr>& get_all_declared_constants() {
  return G.all_constants;
}

const std::vector<StructPtr>& get_all_declared_structs() {
  return G.all_structs;
}

const std::vector<EnumDefPtr>& get_all_declared_enums() {
  return G.all_enums;
}

} // namespace tolk
