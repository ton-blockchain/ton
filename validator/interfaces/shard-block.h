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
#include "shard.h"
#include "interfaces/block-handle.h"

namespace ton {

namespace validator {

class ShardTopBlockDescription : public td::CntObject {
 public:
  virtual ~ShardTopBlockDescription() = default;
  virtual ShardIdFull shard() const = 0;
  virtual BlockIdExt block_id() const = 0;
  virtual bool before_split() const = 0;
  virtual bool after_split() const = 0;
  virtual bool after_merge() const = 0;
  virtual CatchainSeqno catchain_seqno() const = 0;
  virtual UnixTime generated_at() const = 0;

  // if method returns false this shard block description is discarded
  // if it returns true it will be supplied to collator
  //
  // it may be invalid if:
  //   a. a block with a greater or equal seqno is in the masterchain
  //   b. this validator set is not valid anymore
  //   c. this shard is not valid anymore
  virtual bool may_be_valid(BlockHandle last_masterchain_block_handle,
                            td::Ref<MasterchainState> last_masterchain_block_state) const = 0;

  virtual td::BufferSlice serialize() const = 0;
};

}  // namespace validator

}  // namespace ton
