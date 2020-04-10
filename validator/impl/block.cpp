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
#include "block.hpp"

namespace ton {

namespace validator {

using td::Ref;

BlockQ::BlockQ(const BlockQ& other) : data_(other.data_.clone()), id_(other.id_), root_(other.root_), boc_(other.boc_) {
}

BlockQ::BlockQ(BlockIdExt id, td::BufferSlice data) : data_(std::move(data)), id_(id) {
}

BlockQ::~BlockQ() {
}

td::Status BlockQ::init() {
  if (root_.not_null()) {
    return td::Status::OK();
  }
  if (data_.is_null()) {
    return td::Status::Error(-668, "cannot initialize a block from an empty BufferSlice");
  }
  vm::StaticBagOfCellsDbLazy::Options options;
  options.check_crc32c = true;
  auto res = vm::StaticBagOfCellsDbLazy::create(vm::BufferSliceBlobView::create(data_.clone()), options);
  if (res.is_error()) {
    return res.move_as_error();
  }
  boc_ = res.move_as_ok();
  auto rc = boc_->get_root_count();
  if (rc.is_error()) {
    return rc.move_as_error();
  }
  if (rc.move_as_ok() != 1) {
    return td::Status::Error(-668, "shardchain block BoC is invalid");
  }
  auto res3 = boc_->get_root_cell(0);
  if (res3.is_error()) {
    return res3.move_as_error();
  }
  root_ = res3.move_as_ok();
  if (root_.is_null()) {
    return td::Status::Error(-668, "cannot extract root cell out of a shardchain block BoC");
  }
  return td::Status::OK();
}

td::Result<td::Ref<BlockQ>> BlockQ::create(BlockIdExt id, td::BufferSlice data) {
  td::Ref<BlockQ> res{true, id, std::move(data)};
  auto err = res.unique_write().init();
  if (err.is_error()) {
    return err.move_as_error();
  } else {
    return std::move(res);
  }
}

}  // namespace validator
}  // namespace ton
