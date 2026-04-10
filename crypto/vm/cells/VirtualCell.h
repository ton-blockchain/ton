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
#include "vm/cells/Cell.h"

namespace vm {
class VirtualCell : public Cell {
 private:
  struct PrivateTag {};

 public:
  static Ref<Cell> create(td::uint32 effective_level, Ref<Cell> cell) {
    if (cell->get_level() <= effective_level) {
      return cell;
    }
    return Ref<VirtualCell>{true, effective_level, std::move(cell), PrivateTag{}};
  }

  VirtualCell(td::uint32 effective_level, Ref<Cell> cell, PrivateTag)
      : effective_level_(effective_level), cell_(std::move(cell)) {
  }

  // load interface
  td::Status set_data_cell(Ref<DataCell> &&data_cell) const override {
    return cell_->set_data_cell(std::move(data_cell));
  }
  td::Result<LoadedCell> load_cell() const override {
    TRY_RESULT(loaded_cell, cell_->load_cell());
    loaded_cell.effective_level = std::min(loaded_cell.effective_level, effective_level_);
    return std::move(loaded_cell);
  }

  Ref<Cell> virtualize(td::uint32 new_effective_level) const override {
    if (effective_level_ <= new_effective_level) {
      return Ref<Cell>(this);
    }
    return create(new_effective_level, cell_);
  }

  bool is_virtualized() const override {
    return true;
  }

  CellUsageTree::NodePtr get_tree_node() const override {
    return cell_->get_tree_node();
  }

  bool is_loaded() const override {
    return cell_->is_loaded();
  }

  // hash and level
  LevelMask get_level_mask() const override {
    return cell_->get_level_mask().apply(effective_level_);
  }

 protected:
  const Hash do_get_hash(td::uint32 level) const override {
    return cell_->get_hash(std::min(effective_level_, level));
  }
  td::uint16 do_get_depth(td::uint32 level) const override {
    return cell_->get_depth(std::min(effective_level_, level));
  }

 private:
  td::uint32 effective_level_;
  Ref<Cell> cell_;
};
}  // namespace vm
