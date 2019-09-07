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

#include "interfaces/shard-block.h"

namespace ton {

namespace validator {

namespace dummy0 {

class ShardTopBlockDescriptionImpl : public ShardTopBlockDescription {
 public:
  ShardIdFull shard() const override {
    return block_id_.shard_full();
  }
  BlockIdExt block_id() const override {
    return block_id_;
  }

  bool may_be_valid(BlockHandle last_masterchain_block_handle,
                    td::Ref<MasterchainState> last_masterchain_block_state) const override;

  td::BufferSlice serialize() const override;

  bool before_split() const override {
    return before_split_;
  }
  bool after_split() const override {
    return after_split_;
  }
  bool after_merge() const override {
    return after_merge_;
  }

  ShardTopBlockDescriptionImpl(BlockIdExt block_id, bool after_split, bool after_merge, bool before_split,
                               CatchainSeqno catchain_seqno, td::uint32 validator_set_hash, td::BufferSlice signatures)
      : block_id_(block_id)
      , after_split_(after_split)
      , after_merge_(after_merge)
      , before_split_(before_split)
      , catchain_seqno_(catchain_seqno)
      , validator_set_hash_(validator_set_hash)
      , signatures_(std::move(signatures)) {
  }

  ShardTopBlockDescriptionImpl *make_copy() const override {
    return new ShardTopBlockDescriptionImpl{block_id_,       after_split_,        after_merge_,       before_split_,
                                            catchain_seqno_, validator_set_hash_, signatures_.clone()};
  }

  static td::Result<td::Ref<ShardTopBlockDescription>> fetch(td::BufferSlice data);

 private:
  BlockIdExt block_id_;
  bool after_split_;
  bool after_merge_;
  bool before_split_;

  CatchainSeqno catchain_seqno_;
  td::uint32 validator_set_hash_;
  td::BufferSlice signatures_;
};

class ValidateShardTopBlockDescription : public td::actor::Actor {
 public:
  ValidateShardTopBlockDescription(td::BufferSlice data, BlockHandle masterchain_handle,
                                   td::Ref<MasterchainState> masterchain_state,
                                   td::actor::ActorId<ValidatorManager> manager, td::Timestamp timeout,
                                   td::Promise<td::Ref<ShardTopBlockDescription>> promise)
      : data_(std::move(data))
      , handle_(std::move(masterchain_handle))
      , state_(std::move(masterchain_state))
      , manager_(manager)
      , timeout_(timeout)
      , promise_(std::move(promise)) {
  }

  void finish_query();
  void abort_query(td::Status reason);
  void alarm() override;

  void start_up() override;

 private:
  td::BufferSlice data_;
  tl_object_ptr<ton_api::test0_topShardBlockDescription> unserialized_;

  BlockHandle handle_;
  td::Ref<MasterchainState> state_;

  td::actor::ActorId<ValidatorManager> manager_;
  td::Timestamp timeout_;
  td::Promise<td::Ref<ShardTopBlockDescription>> promise_;
};

}  // namespace dummy0

}  // namespace validator

}  // namespace ton
