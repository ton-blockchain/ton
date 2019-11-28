#pragma once
#include "stack.hpp"

#include <vector>

namespace vm {

td::Result<std::vector<vm::StackEntry>> parse_stack_entries(td::Slice str, bool prefix_only = false);
td::Result<vm::StackEntry> parse_stack_entry(td::Slice str, bool prefix_only = false);

}  // namespace vm
