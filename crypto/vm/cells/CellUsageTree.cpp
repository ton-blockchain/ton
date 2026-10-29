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
#include <algorithm>

#include "vm/boc.h"
#include "vm/cells/CellUsageTree.h"

#include "DataCell.h"

namespace vm {
//
// CellUsageTree::NodePtr
//
bool CellUsageTree::NodePtr::on_load(const Cell::LoadedCell& loaded_cell) const {
  auto tree = tree_weak_.lock();
  if (!tree) {
    return false;
  }
  tree->on_load(node_id_, loaded_cell);
  return true;
}

CellUsageTree::NodePtr CellUsageTree::NodePtr::create_child(unsigned ref_id, Ref<Cell> cell) const {
  auto tree = tree_weak_.lock();
  if (!tree) {
    return {};
  }
  return {tree_weak_, tree->create_child(node_id_, ref_id, std::move(cell))};
}

CellUsageTree* CellUsageTree::NodePtr::find_from_trees(td::Span<CellUsageTree*> trees) const {
  auto tree = tree_weak_.lock();
  if (tree && std::find(trees.begin(), trees.end(), tree.get()) != trees.end()) {
    return tree.get();
  }
  return nullptr;
}

bool CellUsageTree::NodePtr::is_from_tree(const CellUsageTree* master_tree) const {
  DCHECK(master_tree);
  auto tree = tree_weak_.lock();
  if (tree.get() != master_tree) {
    return false;
  }
  return true;
}

bool CellUsageTree::NodePtr::mark_path(CellUsageTree* master_tree) const {
  DCHECK(master_tree);
  auto tree = tree_weak_.lock();
  if (tree.get() != master_tree) {
    return false;
  }
  master_tree->mark_path(node_id_);
  return true;
}

//
// CellUsageTree
//
CellUsageTree::CellUsageTree(Ref<Cell> root) : nodes_(2) {
  CHECK(root.not_null());
  nodes_[1].cell = std::move(root);
}

CellUsageTree::NodePtr CellUsageTree::root_ptr() {
  return {shared_from_this(), 1};
}

CellUsageTree::NodeId CellUsageTree::root_id() const {
  return 1;
};

bool CellUsageTree::is_loaded(NodeId node_id) const {
  return nodes_[node_id].is_loaded;
}

bool CellUsageTree::has_mark(NodeId node_id) const {
  return nodes_[node_id].has_mark;
}

void CellUsageTree::set_mark(NodeId node_id, bool mark) {
  if (node_id == 0) {
    return;
  }
  nodes_[node_id].has_mark = mark;
}

void CellUsageTree::mark_path(NodeId node_id) {
  auto cur_node_id = get_parent(node_id);
  while (cur_node_id != 0) {
    if (has_mark(cur_node_id)) {
      break;
    }
    set_mark(cur_node_id);
    cur_node_id = get_parent(cur_node_id);
  }
}

void CellUsageTree::add_path_to_proof_stat(NodeId node_id, MerkleUpdateLeftStat& stat) {
  auto cur_node_id = get_parent(node_id);
  while (cur_node_id != 0 && !nodes_[cur_node_id].has_stat_mark) {
    nodes_[cur_node_id].has_stat_mark = true;
    stat.add_loaded_cell(nodes_[cur_node_id].cell, nodes_[cur_node_id].merkle_depth);
    cur_node_id = get_parent(cur_node_id);
  }
}

CellUsageTree::NodeId CellUsageTree::get_parent(NodeId node_id) const {
  return nodes_[node_id].parent;
}

CellUsageTree::NodeId CellUsageTree::get_child(NodeId node_id, unsigned ref_id) const {
  DCHECK(ref_id < CellTraits::max_refs);
  return nodes_[node_id].children[ref_id];
}

void CellUsageTree::on_load(NodeId node_id, const Cell::LoadedCell& loaded_cell) {
  if (ignore_loads_ || nodes_[node_id].is_loaded) {
    return;
  }
  nodes_[node_id].is_loaded = true;
  if (cell_load_callback_) {
    cell_load_callback_(node_id, loaded_cell);
  }
}

CellUsageTree::NodeId CellUsageTree::create_child(NodeId node_id, unsigned ref_id, Ref<Cell> cell) {
  DCHECK(ref_id < CellTraits::max_refs);
  NodeId res = nodes_[node_id].children[ref_id];
  if (res) {
    return res;
  }
  res = create_node(node_id, std::move(cell));
  nodes_[node_id].children[ref_id] = res;
  return res;
}

CellUsageTree::NodeId CellUsageTree::create_node(NodeId parent_id, Ref<Cell> cell) {
  NodeId res = static_cast<NodeId>(nodes_.size());
  nodes_.emplace_back();
  Node& node = nodes_.back();
  Node& parent = nodes_[parent_id];
  node.parent = parent_id;
  CHECK(cell.not_null());
  node.cell = std::move(cell);
  CellSlice parent_cs{NoVm{}, parent.cell};
  node.merkle_depth = parent_cs.child_merkle_depth(parent.merkle_depth);
  return res;
}

void CellUsageTree::set_cell_load_callback(std::function<void(const LoadedCell&)> f) {
  if (!f) {
    cell_load_callback_ = {};
  } else {
    cell_load_callback_ = std::bind(f, std::placeholders::_2);
  }
}

void CellUsageTree::Recorder::begin_epoch(CellUsageTree* tree) {
  CHECK(!tree_ && tree);
  tree_ = tree;
  previous_callback_ = std::move(tree_->cell_load_callback_);
  tree_->cell_load_callback_ = [this](NodeId node_id, const LoadedCell& cell) {
    journal_.push_back(node_id);
    if (previous_callback_) {
      previous_callback_(node_id, cell);
    }
  };
}

CellUsageTree::Recorder::Checkpoint CellUsageTree::Recorder::checkpoint() const {
  CHECK(tree_);
  return journal_.size();
}

void CellUsageTree::Recorder::commit_epoch() {
  CHECK(tree_);
  tree_->cell_load_callback_ = std::move(previous_callback_);
  tree_ = nullptr;
  journal_.clear();
}

void CellUsageTree::Recorder::rollback(Checkpoint checkpoint) {
  CHECK(tree_ && checkpoint <= journal_.size());
  for (size_t i = journal_.size(); i > checkpoint; --i) {
    tree_->nodes_[journal_[i - 1]].is_loaded = false;
    journal_.pop_back();
  }
}

}  // namespace vm
