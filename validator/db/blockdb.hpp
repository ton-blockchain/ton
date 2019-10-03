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
#include "ton/ton-types.h"
#include "validator/interfaces/block-handle.h"
#include "validator/interfaces/db.h"
#include "td/db/KeyValueAsync.h"

namespace ton {

namespace validator {

class RootDb;

class BlockDb : public td::actor::Actor {
 public:
  void store_block_handle(BlockHandle handle, td::Promise<td::Unit> promise);
  void get_block_handle(BlockIdExt id, td::Promise<BlockHandle> promise);

  void start_up() override;
  void alarm() override;

  void gc();
  void skip_gc();

  void truncate(td::Ref<MasterchainState> state, td::Promise<td::Unit> promise);

  BlockDb(td::actor::ActorId<RootDb> root_db, std::string db_path);

 private:
  using KeyHash = td::Bits256;
  struct DbEntry {
    BlockIdExt block_id;
    KeyHash prev;
    KeyHash next;

    DbEntry(tl_object_ptr<ton_api::db_blockdb_lru> entry);
    DbEntry() {
    }
    DbEntry(BlockIdExt block_id, KeyHash prev, KeyHash next) : block_id(block_id), prev(prev), next(next) {
    }
    tl_object_ptr<ton_api::db_blockdb_lru> release();
    bool is_empty() const {
      return !block_id.is_valid();
    }
  };
  static KeyHash get_block_lru_key(BlockIdExt block_id);
  static KeyHash get_block_value_key(BlockIdExt block_id);
  static KeyHash get_block_lru_empty_key_hash() {
    return KeyHash::zero();
  }

  td::Result<DbEntry> get_block_lru(KeyHash key);
  td::Result<td::BufferSlice> get_block_value(KeyHash key);

  void set_block_lru(KeyHash key_hash, DbEntry e);
  void set_block_value(KeyHash key_hash, td::BufferSlice data);

  std::shared_ptr<td::KeyValue> kv_;

  td::actor::ActorId<RootDb> root_db_;

  std::string db_path_;

  KeyHash last_gc_ = KeyHash::zero();
};

}  // namespace validator

}  // namespace ton
