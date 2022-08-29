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

#include "td/actor/actor.h"
#include "crypto/vm/db/DynamicBagOfCellsDb.h"
#include "crypto/vm/db/CellStorage.h"
#include "td/db/KeyValue.h"
#include "ton/ton-types.h"
#include "interfaces/block-handle.h"
#include "auto/tl/ton_api.h"

namespace ton {

namespace validator {

class RootDb;

class CellDb;
class CellDbAsyncExecutor;

class CellDbBase : public td::actor::Actor {
 public:
  virtual void start_up();
 protected:
  std::shared_ptr<vm::DynamicBagOfCellsDb::AsyncExecutor> async_executor;
 private:
  void execute_sync(std::function<void()> f);
  friend CellDbAsyncExecutor;
};

class CellDbIn : public CellDbBase {
 public:
  using KeyHash = td::Bits256;

  void load_cell(RootHash hash, td::Promise<td::Ref<vm::DataCell>> promise);
  void store_cell(BlockIdExt block_id, td::Ref<vm::Cell> cell, td::Promise<td::Ref<vm::DataCell>> promise);
  void get_cell_db_reader(td::Promise<std::shared_ptr<vm::CellDbReader>> promise);

  CellDbIn(td::actor::ActorId<RootDb> root_db, td::actor::ActorId<CellDb> parent, std::string path);

  void start_up() override;
  void alarm() override;

 private:
  struct DbEntry {
    BlockIdExt block_id;
    KeyHash prev;
    KeyHash next;
    RootHash root_hash;

    DbEntry(tl_object_ptr<ton_api::db_celldb_value> entry);
    DbEntry() {
    }
    DbEntry(BlockIdExt block_id, KeyHash prev, KeyHash next, RootHash root_hash)
        : block_id(block_id), prev(prev), next(next), root_hash(root_hash) {
    }
    td::BufferSlice release();
    bool is_empty() const {
      return !block_id.is_valid();
    }
  };
  td::Result<DbEntry> get_block(KeyHash key);
  void set_block(KeyHash key, DbEntry e);

  static std::string get_key(KeyHash key);
  static KeyHash get_key_hash(BlockIdExt block_id);
  static BlockIdExt get_empty_key();
  KeyHash get_empty_key_hash();

  void gc();
  void gc_cont(BlockHandle handle);
  void gc_cont2(BlockHandle handle);
  void skip_gc();

  td::actor::ActorId<RootDb> root_db_;
  td::actor::ActorId<CellDb> parent_;

  std::string path_;

  std::unique_ptr<vm::DynamicBagOfCellsDb> boc_;
  std::shared_ptr<vm::KeyValue> cell_db_;

  KeyHash last_gc_;
};

class CellDb : public CellDbBase {
 public:
  void load_cell(RootHash hash, td::Promise<td::Ref<vm::DataCell>> promise);
  void store_cell(BlockIdExt block_id, td::Ref<vm::Cell> cell, td::Promise<td::Ref<vm::DataCell>> promise);
  void update_snapshot(std::unique_ptr<td::KeyValueReader> snapshot) {
    started_ = true;
    boc_->set_loader(std::make_unique<vm::CellLoader>(std::move(snapshot))).ensure();
  }
  void get_cell_db_reader(td::Promise<std::shared_ptr<vm::CellDbReader>> promise);

  CellDb(td::actor::ActorId<RootDb> root_db, std::string path) : root_db_(root_db), path_(path) {
  }

  void start_up() override;

 private:
  td::actor::ActorId<RootDb> root_db_;
  std::string path_;

  td::actor::ActorOwn<CellDbIn> cell_db_;

  std::unique_ptr<vm::DynamicBagOfCellsDb> boc_;
  bool started_ = false;
};

}  // namespace validator

}  // namespace ton
