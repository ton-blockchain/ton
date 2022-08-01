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
#include "vm/cells.h"
#include "ton/ton-types.h"
#include "auto/tl/ton_api.h"

namespace ton {

namespace validator {
using td::Ref;

struct OutMsgQueueProof : public td::CntObject {
  OutMsgQueueProof(Ref<vm::Cell> state_root, Ref<vm::Cell> block_state_proof)
      : state_root_(std::move(state_root)), block_state_proof_(std::move(block_state_proof)) {
  }

  Ref<vm::Cell> state_root_;
  Ref<vm::Cell> block_state_proof_;

  static td::Result<td::Ref<OutMsgQueueProof>> fetch(BlockIdExt block_id, ShardIdFull dst_shard,
                                                     const ton_api::tonNode_outMsgQueueProof &f);
  static td::Result<tl_object_ptr<ton_api::tonNode_outMsgQueueProof>> serialize(
      BlockIdExt block_id, ShardIdFull dst_shard, Ref<vm::Cell> state_root, Ref<vm::Cell> block_root);
};

}  // namespace validator
}  // namespace ton
