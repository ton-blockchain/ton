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

#include "fwd-declarations.h"
#include <vector>

namespace tolk {

// For an `if`/`match` that returns in some of its branches: where the tail of the function goes.
// See the cpp file for comments.
struct InlineReturnBranchNode {
  AnyV pivot = nullptr;     // the `if` or `match` statement itself
  AnyV goes_to = nullptr;   // its branch: a block of `if`, or an arm of `match`; nullptr = implicit `else` of `match`
};

// Per-function plan for every inlined function (auto-detected or marked as `@inline`).
struct InlineReturnPlan {
  const char* cant_inline_because = nullptr;
  bool has_early_returns = false;
  std::vector<InlineReturnBranchNode> nodes;

  bool ok() const {
    return cant_inline_because == nullptr;
  }

  const InlineReturnBranchNode* find_branching(AnyV stmt_pivot) const {
    for (const InlineReturnBranchNode& node : nodes) {
      if (node.pivot == stmt_pivot) {
        return &node;
      }
    }
    return nullptr;
  }
};

InlineReturnPlan build_inlining_plan_for_function(FunctionPtr fun_ref);

} // namespace tolk
