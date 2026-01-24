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
*/
#pragma once
#include "block/signature-set.h"
#include "ton/ton-types.h"

namespace ton::validator {

struct ReceivedBlock {
  BlockIdExt id;
  td::BufferSlice data;

  ReceivedBlock clone() const {
    return ReceivedBlock{id, data.clone()};
  }
};

struct BlockBroadcast {
  BlockIdExt block_id;
  td::Ref<block::BlockSignatureSet> sig_set;
  td::BufferSlice data;
  td::BufferSlice proof;

  BlockBroadcast clone() const {
    return {block_id, sig_set, data.clone(), proof.clone()};
  }
};

}  // namespace ton::validator
