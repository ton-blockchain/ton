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

#include <functional>

#include "td/utils/Span.h"
#include "td/utils/int_types.h"
#include "td/utils/logging.h"
#include "vm/cells/CellTraits.h"

namespace vm {

class Cell;
class DataCell;
struct LoadedCell;
class MerkleUpdateLeftStat;

class CellUsageTree : public std::enable_shared_from_this<CellUsageTree> {
 public:
  using NodeId = td::uint32;

  class Recorder;

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
    NodePtr create_child(unsigned ref_id, td::Ref<Cell> cell) const;
    CellUsageTree* find_from_trees(td::Span<CellUsageTree*> trees) const;
    bool mark_path(CellUsageTree* master_tree) const;
    bool is_from_tree(const CellUsageTree* master_tree) const;
    NodeId node_id() const {
      return node_id_;
    }

   private:
    std::weak_ptr<CellUsageTree> tree_weak_;
    NodeId node_id_{0};
  };

  explicit CellUsageTree(td::Ref<Cell> root);

  NodePtr root_ptr();
  NodeId root_id() const;
  bool is_loaded(NodeId node_id) const;
  bool has_mark(NodeId node_id) const;
  void set_mark(NodeId node_id, bool mark = true);
  void mark_path(NodeId node_id);
  void add_path_to_proof_stat(NodeId node_id, MerkleUpdateLeftStat& stat);
  NodeId get_parent(NodeId node_id) const;
  NodeId get_child(NodeId node_id, unsigned ref_id) const;
  NodeId create_child(NodeId node_id, unsigned ref_id, td::Ref<Cell> cell);

  using callback_t = std::function<void(NodeId, const LoadedCell&)>;
  void set_cell_load_callback(std::function<void(const LoadedCell&)> f);
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
    bool has_stat_mark{false};
    NodeId parent{0};
    std::array<td::uint32, CellTraits::max_refs> children{};
    td::Ref<Cell> cell;
    unsigned merkle_depth = 0;
  };
  std::vector<Node> nodes_;
  callback_t cell_load_callback_;

  void on_load(NodeId node_id, const LoadedCell& loaded_cell);
  NodeId create_node(NodeId parent_id, td::Ref<Cell> cell);
  int ignore_loads_ = 0;
};

class CellUsageTree::Recorder {
 public:
  using Checkpoint = size_t;

  void begin_epoch(CellUsageTree* tree);
  Checkpoint checkpoint() const;
  void commit_epoch();
  void rollback(Checkpoint checkpoint);

 private:
  CellUsageTree* tree_ = nullptr;
  callback_t previous_callback_ = nullptr;
  std::vector<NodeId> journal_;
};
}  // namespace vm
