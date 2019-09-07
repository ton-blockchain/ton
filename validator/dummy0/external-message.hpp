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

#include "validator/interfaces/external-message.h"
#include "auto/tl/ton_api.h"
#include "adnl/utils.hpp"

namespace ton {

namespace validator {

namespace dummy0 {

class ExtMessageImpl : public ExtMessage {
 public:
  AccountIdPrefixFull shard() const override {
    return shard_;
  }
  td::BufferSlice serialize() const override {
    return data_.clone();
  }
  td::Ref<vm::Cell> root_cell() const override {
    UNREACHABLE();
  }
  Hash hash() const override {
    return hash_;
  }

  ExtMessageImpl *make_copy() const override {
    return new ExtMessageImpl{shard_, data_.clone(), hash_};
  }

  ExtMessageImpl(AccountIdPrefixFull shard, td::BufferSlice data, Hash hash)
      : shard_(shard), data_(std::move(data)), hash_(hash) {
  }

  ExtMessageImpl(tl_object_ptr<ton_api::test0_extMessage> data) {
    data_ = serialize_tl_object(data, true);
    hash_ = UInt256_2_Bits256(get_tl_object_sha256(data));
    shard_ = AccountIdPrefixFull{data->workchain_, static_cast<AccountIdPrefix>(data->shard_)};
  }

 private:
  AccountIdPrefixFull shard_;
  td::BufferSlice data_;
  Hash hash_;
};

}  // namespace dummy0

}  // namespace validator

}  // namespace ton
