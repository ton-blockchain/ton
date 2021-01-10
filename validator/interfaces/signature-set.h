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

namespace ton {

namespace validator {

class BlockSignatureSet : public td::CntObject {
 public:
  const auto &signatures() const {
    return signatures_;
  }
  auto &signatures() {
    return signatures_;
  }
  std::size_t size() const {
    return signatures_.size();
  }

  virtual td::BufferSlice serialize() const = 0;

  BlockSignatureSet(std::vector<BlockSignature> signatures) : signatures_(std::move(signatures)) {
  }

 protected:
  std::vector<BlockSignature> signatures_;
};

}  // namespace validator

}  // namespace ton
