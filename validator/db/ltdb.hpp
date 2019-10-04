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

#include "td/actor/actor.h"
#include "td/db/KeyValueAsync.h"
#include "validator/interfaces/db.h"

#include "ton/ton-types.h"

#include "auto/tl/ton_api.h"

namespace ton {

namespace validator {

class RootDb;

class LtDb : public td::actor::Actor {
 public:
  void add_new_block(BlockIdExt block_id, LogicalTime lt, UnixTime ts, td::Promise<td::Unit> promise);
  void get_block_common(AccountIdPrefixFull account_id,
                        std::function<td::int32(ton_api::db_lt_desc_value &)> compare_desc,
                        std::function<td::int32(ton_api::db_lt_el_value &)> compare, bool exact,
                        td::Promise<BlockIdExt> promise);
  void get_block_by_lt(AccountIdPrefixFull account_id, LogicalTime lt, td::Promise<BlockIdExt> promise);
  void get_block_by_unix_time(AccountIdPrefixFull account_id, UnixTime ts, td::Promise<BlockIdExt> promise);
  void get_block_by_seqno(AccountIdPrefixFull account_id, BlockSeqno seqno, td::Promise<BlockIdExt> promise);

  void truncate_workchain(ShardIdFull shard, td::Ref<MasterchainState> state);
  void truncate(td::Ref<MasterchainState> state, td::Promise<td::Unit> promise);

  void start_up() override;

  LtDb(td::actor::ActorId<RootDb> root_db, std::string db_path) : root_db_(root_db), db_path_(std::move(db_path)) {
  }

 private:
  td::BufferSlice get_desc_key(ShardIdFull shard);
  td::BufferSlice get_el_key(ShardIdFull shard, td::uint32 idx);
  td::BufferSlice get_from_db(ShardIdFull shard, td::uint32 idx);

  std::shared_ptr<td::KeyValue> kv_;

  td::actor::ActorId<RootDb> root_db_;
  std::string db_path_;
};

}  // namespace validator

}  // namespace ton
