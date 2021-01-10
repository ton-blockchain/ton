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

#include "ton/ton-types.h"
#include "validator/interfaces/signature-set.h"
#include "vm/cells.h"

namespace ton {

namespace validator {
using td::Ref;

class BlockSignatureSetQ : public BlockSignatureSet {
  enum { max_signatures = 1024 };

 public:
  BlockSignatureSetQ(std::vector<BlockSignature> signatures) : BlockSignatureSet{std::move(signatures)} {
  }
  BlockSignatureSetQ* make_copy() const override;
  td::BufferSlice serialize() const override;
  bool serialize_to(Ref<vm::Cell>& ref) const;
  static Ref<BlockSignatureSet> fetch(td::BufferSlice data);
  static Ref<BlockSignatureSet> fetch(td::Ref<vm::Cell> cell);
};

}  // namespace validator

}  // namespace ton
