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

#include "crypto/common/refcnt.hpp"
#include "ton/ton-types.h"
#include "crypto/vm/cells.h"

namespace ton {

namespace validator {

class ExtMessage : public td::CntObject {
 public:
  using Hash = Bits256;

  virtual ~ExtMessage() = default;
  virtual AccountIdPrefixFull shard() const = 0;
  virtual td::BufferSlice serialize() const = 0;
  virtual td::Ref<vm::Cell> root_cell() const = 0;
  virtual Hash hash() const = 0;
  virtual ton::WorkchainId wc() const = 0;
  virtual ton::StdSmcAddress addr() const = 0;
};

}  // namespace validator

}  // namespace ton
