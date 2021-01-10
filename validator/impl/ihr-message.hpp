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

#include "validator/interfaces/ihr-message.h"
#include "auto/tl/ton_api.h"
#include "adnl/utils.hpp"

namespace ton {

namespace validator {

class IhrMessageQ : public IhrMessage {
  td::Ref<vm::Cell> root_;
  AccountIdPrefixFull addr_prefix_;
  td::BufferSlice data_;
  Hash hash_;
  BlockIdExt blkid_;

 public:
  static constexpr unsigned max_ihr_msg_size = 65535;
  AccountIdPrefixFull shard() const override {
    return addr_prefix_;
  }
  td::BufferSlice serialize() const override {
    return data_.clone();
  }
  td::Ref<vm::Cell> root_cell() const override {
    return root_;
  }
  Hash hash() const override {
    return hash_;
  }
  IhrMessageQ(td::BufferSlice data, td::Ref<vm::Cell> root, BlockIdExt block, AccountIdPrefixFull shard);
  static td::Result<td::Ref<IhrMessageQ>> create_ihr_message(td::BufferSlice data);
};

}  // namespace validator

}  // namespace ton
