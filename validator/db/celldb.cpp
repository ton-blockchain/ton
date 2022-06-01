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
#include "celldb.hpp"
#include "rootdb.hpp"

#include "td/db/RocksDb.h"

#include "ton/ton-tl.hpp"
#include "ton/ton-io.hpp"

namespace ton {

namespace validator {

class CellDbAsyncExecutor : public vm::DynamicBagOfCellsDb::AsyncExecutor {
 public:
  explicit CellDbAsyncExecutor(td::actor::ActorId<CellDbBase> cell_db) : cell_db_(std::move(cell_db)) {
  }

  void execute_async(std::function<void()> f) {
    class Runner : public td::actor::Actor {
     public:
      explicit Runner(std::function<void()> f) : f_(std::move(f)) {}
      void start_up() {
        f_();
        stop();
      }
     private:
      std::function<void()> f_;
    };
    td::actor::create_actor<Runner>("executeasync", std::move(f)).release();
  }

  void execute_sync(std::function<void()> f) {
    td::actor::send_closure(cell_db_, &CellDbBase::execute_sync, std::move(f));
  }
 private:
  td::actor::ActorId<CellDbBase> cell_db_;
};

void CellDbBase::start_up() {
  async_executor = std::make_shared<CellDbAsyncExecutor>(actor_id(this));
}

void CellDbBase::execute_sync(std::function<void()> f) {
  f();
}

CellDbIn::CellDbIn(td::actor::ActorId<RootDb> root_db, td::actor::ActorId<CellDb> parent, std::string path)
    : root_db_(root_db), parent_(parent), path_(std::move(path)) {
}

void CellDbIn::start_up() {
  CellDbBase::start_up();
  cell_db_ = std::make_shared<td::RocksDb>(td::RocksDb::open(path_).move_as_ok());

  boc_ = vm::DynamicBagOfCellsDb::create();
  boc_->set_loader(std::make_unique<vm::CellLoader>(cell_db_->snapshot())).ensure();
  td::actor::send_closure(parent_, &CellDb::update_snapshot, cell_db_->snapshot());

  alarm_timestamp() = td::Timestamp::in(10.0);

  auto empty = get_empty_key_hash();
  if (get_block(empty).is_error()) {
    DbEntry e{get_empty_key(), empty, empty, RootHash::zero()};
    cell_db_->begin_write_batch().ensure();
    set_block(empty, std::move(e));
    cell_db_->commit_write_batch().ensure();
  }
  last_gc_ = empty;
}

void CellDbIn::load_cell(RootHash hash, td::Promise<td::Ref<vm::DataCell>> promise) {
  boc_->load_cell_async(hash.as_slice(), async_executor, std::move(promise));
}

void CellDbIn::store_cell(BlockIdExt block_id, td::Ref<vm::Cell> cell, td::Promise<td::Ref<vm::DataCell>> promise) {
  td::PerfWarningTimer{"storecell", 0.1};
  auto key_hash = get_key_hash(block_id);
  auto R = get_block(key_hash);
  // duplicate
  if (R.is_ok()) {
    promise.set_result(boc_->load_cell(cell->get_hash().as_slice()));
    return;
  }

  auto empty = get_empty_key_hash();
  auto ER = get_block(empty);
  ER.ensure();
  auto E = ER.move_as_ok();

  auto PR = get_block(E.prev);
  PR.ensure();
  auto P = PR.move_as_ok();
  CHECK(P.next == empty);

  DbEntry D{block_id, E.prev, empty, cell->get_hash().bits()};

  E.prev = key_hash;
  P.next = key_hash;

  if (P.is_empty()) {
    E.next = key_hash;
    P.prev = key_hash;
  }

  boc_->inc(cell);
  boc_->prepare_commit().ensure();
  vm::CellStorer stor{*cell_db_.get()};
  cell_db_->begin_write_batch().ensure();
  boc_->commit(stor).ensure();
  set_block(empty, std::move(E));
  set_block(D.prev, std::move(P));
  set_block(key_hash, std::move(D));
  cell_db_->commit_write_batch().ensure();

  boc_->set_loader(std::make_unique<vm::CellLoader>(cell_db_->snapshot())).ensure();
  td::actor::send_closure(parent_, &CellDb::update_snapshot, cell_db_->snapshot());

  promise.set_result(boc_->load_cell(cell->get_hash().as_slice()));
}

void CellDbIn::get_cell_db_reader(td::Promise<std::shared_ptr<vm::CellDbReader>> promise) {
  promise.set_result(boc_->get_cell_db_reader());
}

void CellDbIn::alarm() {
  auto R = get_block(last_gc_);
  R.ensure();

  auto N = R.move_as_ok();
  if (N.is_empty()) {
    last_gc_ = N.next;
    alarm_timestamp() = td::Timestamp::in(0.1);
    return;
  }

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<bool> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &CellDbIn::skip_gc);
    } else {
      auto value = R.move_as_ok();
      if (!value) {
        td::actor::send_closure(SelfId, &CellDbIn::skip_gc);
      } else {
        td::actor::send_closure(SelfId, &CellDbIn::gc);
      }
    }
  });
  td::actor::send_closure(root_db_, &RootDb::allow_state_gc, N.block_id, std::move(P));
}

void CellDbIn::gc() {
  auto R = get_block(last_gc_);
  R.ensure();

  auto N = R.move_as_ok();

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<BlockHandle> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &CellDbIn::gc_cont, R.move_as_ok());
  });
  td::actor::send_closure(root_db_, &RootDb::get_block_handle_external, N.block_id, false, std::move(P));
}

void CellDbIn::gc_cont(BlockHandle handle) {
  if (!handle->inited_state_boc()) {
    LOG(WARNING) << "inited_state_boc=false, but state in db. blockid=" << handle->id();
  }
  handle->set_deleted_state_boc();

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), handle](td::Result<td::Unit> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &CellDbIn::gc_cont2, handle);
  });

  td::actor::send_closure(root_db_, &RootDb::store_block_handle, handle, std::move(P));
}

void CellDbIn::gc_cont2(BlockHandle handle) {
  td::PerfWarningTimer{"gccell", 0.1};

  auto FR = get_block(last_gc_);
  FR.ensure();
  auto F = FR.move_as_ok();

  auto PR = get_block(F.prev);
  PR.ensure();
  auto P = PR.move_as_ok();
  auto NR = get_block(F.next);
  NR.ensure();
  auto N = NR.move_as_ok();

  P.next = F.next;
  N.prev = F.prev;
  if (P.is_empty() && N.is_empty()) {
    P.prev = P.next;
    N.next = N.prev;
  }

  auto cell = boc_->load_cell(F.root_hash.as_slice()).move_as_ok();

  boc_->dec(cell);
  boc_->prepare_commit().ensure();
  vm::CellStorer stor{*cell_db_.get()};
  cell_db_->begin_write_batch().ensure();
  boc_->commit(stor).ensure();
  cell_db_->erase(get_key(last_gc_)).ensure();
  set_block(F.prev, std::move(P));
  set_block(F.next, std::move(N));
  cell_db_->commit_write_batch().ensure();
  alarm_timestamp() = td::Timestamp::now();

  boc_->set_loader(std::make_unique<vm::CellLoader>(cell_db_->snapshot())).ensure();
  td::actor::send_closure(parent_, &CellDb::update_snapshot, cell_db_->snapshot());

  DCHECK(get_block(last_gc_).is_error());
  last_gc_ = F.next;
}

void CellDbIn::skip_gc() {
  auto FR = get_block(last_gc_);
  FR.ensure();
  auto F = FR.move_as_ok();
  last_gc_ = F.next;
  alarm_timestamp() = td::Timestamp::in(0.01);
}

std::string CellDbIn::get_key(KeyHash key_hash) {
  if (!key_hash.is_zero()) {
    return PSTRING() << "desc" << key_hash;
  } else {
    return "desczero";
  }
}

CellDbIn::KeyHash CellDbIn::get_key_hash(BlockIdExt block_id) {
  if (block_id.is_valid()) {
    return get_tl_object_sha_bits256(create_tl_block_id(block_id));
  } else {
    return KeyHash::zero();
  }
}

BlockIdExt CellDbIn::get_empty_key() {
  return BlockIdExt{workchainInvalid, 0, 0, RootHash::zero(), FileHash::zero()};
}

CellDbIn::KeyHash CellDbIn::get_empty_key_hash() {
  return KeyHash::zero();
}

td::Result<CellDbIn::DbEntry> CellDbIn::get_block(KeyHash key_hash) {
  const auto key = get_key(key_hash);
  std::string value;
  auto R = cell_db_->get(td::as_slice(key), value);
  R.ensure();
  auto S = R.move_as_ok();
  if (S == td::KeyValue::GetStatus::NotFound) {
    return td::Status::Error(ErrorCode::notready, "not in db");
  }
  auto obj = fetch_tl_object<ton_api::db_celldb_value>(td::BufferSlice{value}, true);
  obj.ensure();
  return DbEntry{obj.move_as_ok()};
}

void CellDbIn::set_block(KeyHash key_hash, DbEntry e) {
  const auto key = get_key(key_hash);
  cell_db_->set(td::as_slice(key), e.release()).ensure();
}

void CellDb::load_cell(RootHash hash, td::Promise<td::Ref<vm::DataCell>> promise) {
  if (!started_) {
    td::actor::send_closure(cell_db_, &CellDbIn::load_cell, hash, std::move(promise));
  } else {
    auto P = td::PromiseCreator::lambda(
        [cell_db_in = cell_db_.get(), hash, promise = std::move(promise)](td::Result<td::Ref<vm::DataCell>> R) mutable {
          if (R.is_error()) {
            td::actor::send_closure(cell_db_in, &CellDbIn::load_cell, hash, std::move(promise));
          } else {
            promise.set_result(R.move_as_ok());
          }
    });
    boc_->load_cell_async(hash.as_slice(), async_executor, std::move(P));
  }
}

void CellDb::store_cell(BlockIdExt block_id, td::Ref<vm::Cell> cell, td::Promise<td::Ref<vm::DataCell>> promise) {
  td::actor::send_closure(cell_db_, &CellDbIn::store_cell, block_id, std::move(cell), std::move(promise));
}

void CellDb::get_cell_db_reader(td::Promise<std::shared_ptr<vm::CellDbReader>> promise) {
  td::actor::send_closure(cell_db_, &CellDbIn::get_cell_db_reader, std::move(promise));
}

void CellDb::start_up() {
  CellDbBase::start_up();
  boc_ = vm::DynamicBagOfCellsDb::create();
  cell_db_ = td::actor::create_actor<CellDbIn>("celldbin", root_db_, actor_id(this), path_);
}

CellDbIn::DbEntry::DbEntry(tl_object_ptr<ton_api::db_celldb_value> entry)
    : block_id(create_block_id(entry->block_id_))
    , prev(entry->prev_)
    , next(entry->next_)
    , root_hash(entry->root_hash_) {
}

td::BufferSlice CellDbIn::DbEntry::release() {
  return create_serialize_tl_object<ton_api::db_celldb_value>(create_tl_block_id(block_id), prev, next, root_hash);
}

}  // namespace validator

}  // namespace ton
