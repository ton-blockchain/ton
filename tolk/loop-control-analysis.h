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
#include "tail-routing.h"
#include <vector>

namespace tolk {

// Per-loop plan for structural `continue`; analogous to InlineReturnPlan.
// See the cpp file for comments.
struct LoopContinuePlan {
  const char* cant_continue_because = nullptr;
  std::vector<TailRoutingNode> nodes;

  bool ok() const {
    return cant_continue_because == nullptr;
  }

  const TailRoutingNode* find_branching(AnyV stmt_pivot) const {
    for (const TailRoutingNode& node : nodes) {
      if (node.pivot == stmt_pivot) {
        return &node;
      }
    }
    return nullptr;
  }
};

LoopContinuePlan build_continue_plan_for_loop(AnyV loop_body_block);

} // namespace tolk
