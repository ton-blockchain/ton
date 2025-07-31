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

    Copyright 2017-2020 Telegram Systems LLP
*/
#pragma once

#include "vm/cells/CellTraits.h"

#include "td/utils/int_types.h"
#include "td/utils/logging.h"
#include <functional>

namespace vm {

class DataCell;
struct LoadedCell;

class CellUsageTree : public std::enable_shared_from_this<CellUsageTree> {
 public:
  using NodeId = td::uint32;

  struct NodePtr {
   public:
    NodePtr() = default;
    NodePtr(std::weak_ptr<CellUsageTree> tree_weak, NodeId node_id)
        : tree_weak_(std::move(tree_weak)), node_id_(node_id) {
    }
    bool empty() const {
      return node_id_ == 0 || tree_weak_.expired();
    }

    bool on_load(const LoadedCell& loaded_cell) const;
    NodePtr create_child(unsigned ref_id) const;
    bool mark_path(CellUsageTree* master_tree) const;
    bool is_from_tree(const CellUsageTree* master_tree) const;

   private:
    std::weak_ptr<CellUsageTree> tree_weak_;
    NodeId node_id_{0};
  };

  NodePtr root_ptr();
  NodeId root_id() const;
  bool is_loaded(NodeId node_id) const;
  bool has_mark(NodeId node_id) const;
  void set_mark(NodeId node_id, bool mark = true);
  void mark_path(NodeId node_id);
  NodeId get_parent(NodeId node_id) const;
  NodeId get_child(NodeId node_id, unsigned ref_id) const;
  void set_use_mark_for_is_loaded(bool use_mark = true);
  NodeId create_child(NodeId node_id, unsigned ref_id);

  void set_cell_load_callback(std::function<void(const LoadedCell&)> f) {
    cell_load_callback_ = std::move(f);
  }
  void set_ignore_loads(bool value) {
    if (value) {
      ++ignore_loads_;
    } else {
      --ignore_loads_;
    }
  }

 private:
  struct Node {
    bool is_loaded{false};
    bool has_mark{false};
    NodeId parent{0};
    std::array<td::uint32, CellTraits::max_refs> children{};
  };
  bool use_mark_{false};
  std::vector<Node> nodes_{2};
  std::function<void(const LoadedCell&)> cell_load_callback_;

  void on_load(NodeId node_id, const LoadedCell& loaded_cell);
  NodeId create_node(NodeId parent);
  int ignore_loads_ = 0;
};
}  // namespace vm
