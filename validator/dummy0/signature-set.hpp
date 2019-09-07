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

#include "ton/ton-types.h"
#include "validator/interfaces/signature-set.h"

namespace ton {

namespace validator {

namespace dummy0 {

class BlockSignatureSetImpl : public BlockSignatureSet {
 public:
  BlockSignatureSetImpl(std::vector<BlockSignature> signatures) : BlockSignatureSet{std::move(signatures)} {
  }

  BlockSignatureSetImpl *make_copy() const override {
    std::vector<BlockSignature> vec;
    auto &sigs = signatures();
    for (auto &s : sigs) {
      vec.emplace_back(BlockSignature{s.node, s.signature.clone()});
    }

    return new BlockSignatureSetImpl{std::move(vec)};
  }

  td::BufferSlice serialize() const override;
  static td::Ref<BlockSignatureSet> fetch(td::BufferSlice data);
};

}  // namespace dummy0

}  // namespace validator

}  // namespace ton
