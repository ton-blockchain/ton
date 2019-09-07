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

    Copyright 2017-2019 Telegram Systems LLP
*/
#pragma once

#include "validator/interfaces/block.h"

#include "adnl/utils.hpp"

#include "ton/ton-types.h"

namespace ton {

namespace validator {

namespace dummy0 {

class Block : public BlockData {
 private:
  td::BufferSlice data_;
  BlockIdExt id_;

 public:
  td::BufferSlice data() const override {
    return data_.clone();
  }
  FileHash file_hash() const override {
    return id_.file_hash;
  }
  BlockIdExt block_id() const override {
    return id_;
  }
  td::Ref<vm::Cell> root_cell() const override {
    return {};
  }
  Block *make_copy() const override {
    return new Block(id_, data_.clone());
  }
  Block(BlockIdExt id, td::BufferSlice data) : data_(std::move(data)), id_(id) {
  }
};

}  // namespace dummy0

}  // namespace validator

}  // namespace ton
