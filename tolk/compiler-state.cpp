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
#include <iostream>
#include <sstream>

namespace tolk {

CompilerState G; // the only mutable global variable in tolk internals

void ExperimentalOption::mark_deprecated(const char* deprecated_from_v, const char* deprecated_reason) {
  this->deprecated_from_v = deprecated_from_v;
  this->deprecated_reason = deprecated_reason;
}

std::string_view PersistentHeapAllocator::copy_string_to_persistent_memory(std::string_view str_in_tmp_memory) {
  size_t len = str_in_tmp_memory.size();
  char* allocated = new char[len];
  memcpy(allocated, str_in_tmp_memory.data(), str_in_tmp_memory.size());
  auto new_chunk = std::make_unique<ChunkInHeap>(allocated, std::move(head));
  head = std::move(new_chunk);
  return {head->allocated, len};
}

void PersistentHeapAllocator::clear() {
  head = nullptr;
}

void CompilerSettings::enable_experimental_option(std::string_view name) {
  ExperimentalOption* to_enable = nullptr;

  if (name == remove_unused_functions.name) {
    to_enable = &remove_unused_functions;
  }

  if (to_enable == nullptr) {
    std::cerr << "unknown experimental option: " << name << std::endl;
  } else if (to_enable->deprecated_from_v) {
    std::cerr << "experimental option " << name << " "
              << "is deprecated since Tolk v" << to_enable->deprecated_from_v
              << ": " << to_enable->deprecated_reason << std::endl;
  } else {
    to_enable->enabled = true;
  }
}

void CompilerSettings::parse_experimental_options_cmd_arg(const std::string& cmd_arg) {
  std::istringstream stream(cmd_arg);
  std::string token;
  while (std::getline(stream, token, ',')) {
    enable_experimental_option(token);
  }
}

const std::vector<FunctionPtr>& get_all_not_builtin_functions() {
  return G.all_functions;
}

const std::vector<GlobalConstPtr>& get_all_declared_constants() {
  return G.all_constants;
}

const std::vector<StructPtr>& get_all_declared_structs() {
  return G.all_structs;
}

} // namespace tolk
