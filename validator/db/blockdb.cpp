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
#include "blockdb.hpp"
#include "rootdb.hpp"
#include "validator/fabric.h"
#include "ton/ton-tl.hpp"
#include "td/utils/port/path.h"
#include "files-async.hpp"
#include "td/db/RocksDb.h"

namespace ton {

namespace validator {

void BlockDb::store_block_handle(BlockHandle handle, td::Promise<td::Unit> promise) {
  if (!handle->id().is_valid()) {
    promise.set_error(td::Status::Error(ErrorCode::protoviolation, "invalid block id"));
    return;
  }
  auto lru_key = get_block_lru_key(handle->id());
  auto value_key = get_block_value_key(handle->id());
  while (handle->need_flush()) {
    auto version = handle->version();
    auto R = get_block_value(value_key);
    if (R.is_ok()) {
      kv_->begin_transaction().ensure();
      set_block_value(value_key, handle->serialize());
      kv_->commit_transaction().ensure();
    } else {
      CHECK(get_block_lru(lru_key).is_error());
      auto empty = get_block_lru_empty_key_hash();
      auto ER = get_block_lru(empty);
      ER.ensure();
      auto E = ER.move_as_ok();

      auto PR = get_block_lru(E.prev);
      PR.ensure();
      auto P = PR.move_as_ok();
      CHECK(P.next == empty);

      DbEntry D{handle->id(), E.prev, empty};

      E.prev = lru_key;
      P.next = lru_key;

      if (P.is_empty()) {
        E.next = lru_key;
        P.prev = lru_key;
      }

      kv_->begin_transaction().ensure();
      set_block_value(value_key, handle->serialize());
      set_block_lru(empty, std::move(E));
      set_block_lru(D.prev, std::move(P));
      set_block_lru(lru_key, std::move(D));
      kv_->commit_transaction().ensure();
    }
    handle->flushed_upto(version);
  }
  promise.set_value(td::Unit());
}

void BlockDb::get_block_handle(BlockIdExt id, td::Promise<BlockHandle> promise) {
  if (!id.is_valid()) {
    promise.set_error(td::Status::Error(ErrorCode::protoviolation, "invalid block id"));
    return;
  }
  CHECK(id.is_valid());
  auto key_hash = get_block_value_key(id);
  auto B = get_block_value(key_hash);
  if (B.is_error()) {
    promise.set_error(B.move_as_error());
    return;
  }
  promise.set_result(create_block_handle(B.move_as_ok()));
}

void BlockDb::start_up() {
  kv_ = std::make_shared<td::RocksDb>(td::RocksDb::open(db_path_).move_as_ok());
  alarm_timestamp() = td::Timestamp::in(0.1);

  auto empty = get_block_lru_empty_key_hash();
  if (get_block_lru(empty).is_error()) {
    DbEntry e{BlockIdExt{}, empty, empty};
    kv_->begin_transaction().ensure();
    set_block_lru(empty, std::move(e));
    kv_->commit_transaction().ensure();
  }
  last_gc_ = empty;
}

BlockDb::BlockDb(td::actor::ActorId<RootDb> root_db, std::string db_path) : root_db_(root_db), db_path_(db_path) {
}

BlockDb::KeyHash BlockDb::get_block_lru_key(BlockIdExt id) {
  if (!id.is_valid()) {
    return KeyHash::zero();
  } else {
    auto obj = create_tl_object<ton_api::db_blockdb_key_lru>(create_tl_block_id(id));
    return get_tl_object_sha_bits256(obj);
  }
}

BlockDb::KeyHash BlockDb::get_block_value_key(BlockIdExt id) {
  CHECK(id.is_valid());
  auto obj = create_tl_object<ton_api::db_blockdb_key_value>(create_tl_block_id(id));
  return get_tl_object_sha_bits256(obj);
}

td::Result<BlockDb::DbEntry> BlockDb::get_block_lru(KeyHash key_hash) {
  std::string value;
  auto R = kv_->get(key_hash.as_slice(), value);
  R.ensure();
  if (R.move_as_ok() == td::KeyValue::GetStatus::NotFound) {
    return td::Status::Error(ErrorCode::notready, "not in db");
  }
  auto v = fetch_tl_object<ton_api::db_blockdb_lru>(td::BufferSlice{value}, true);
  v.ensure();
  return DbEntry{v.move_as_ok()};
}

td::Result<td::BufferSlice> BlockDb::get_block_value(KeyHash key_hash) {
  std::string value;
  auto R = kv_->get(key_hash.as_slice(), value);
  R.ensure();
  if (R.move_as_ok() == td::KeyValue::GetStatus::NotFound) {
    return td::Status::Error(ErrorCode::notready, "not in db");
  }
  return td::BufferSlice{value};
}

void BlockDb::set_block_lru(KeyHash key_hash, DbEntry e) {
  kv_->set(key_hash.as_slice(), serialize_tl_object(e.release(), true)).ensure();
}

void BlockDb::set_block_value(KeyHash key_hash, td::BufferSlice value) {
  kv_->set(key_hash.as_slice(), std::move(value)).ensure();
}

void BlockDb::alarm() {
  auto R = get_block_lru(last_gc_);
  R.ensure();

  auto N = R.move_as_ok();
  if (N.is_empty()) {
    last_gc_ = N.next;
    alarm_timestamp() = td::Timestamp::in(0.01);
    return;
  }

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<bool> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &BlockDb::skip_gc);
    } else {
      auto value = R.move_as_ok();
      if (!value) {
        td::actor::send_closure(SelfId, &BlockDb::skip_gc);
      } else {
        td::actor::send_closure(SelfId, &BlockDb::gc);
      }
    }
  });
  td::actor::send_closure(root_db_, &RootDb::allow_block_gc, N.block_id, std::move(P));
}

void BlockDb::gc() {
  auto FR = get_block_lru(last_gc_);
  FR.ensure();
  auto F = FR.move_as_ok();

  auto PR = get_block_lru(F.prev);
  PR.ensure();
  auto P = PR.move_as_ok();
  auto NR = get_block_lru(F.next);
  NR.ensure();
  auto N = NR.move_as_ok();

  P.next = F.next;
  N.prev = F.prev;
  if (P.is_empty() && N.is_empty()) {
    P.prev = P.next;
    N.next = N.prev;
  }

  auto value_key = get_block_value_key(F.block_id);

  kv_->begin_transaction().ensure();
  kv_->erase(last_gc_.as_slice()).ensure();
  kv_->erase(value_key.as_slice()).ensure();
  set_block_lru(F.prev, std::move(P));
  set_block_lru(F.next, std::move(N));
  kv_->commit_transaction().ensure();
  alarm_timestamp() = td::Timestamp::now();

  DCHECK(get_block_lru(last_gc_).is_error());
  last_gc_ = F.next;
}

void BlockDb::skip_gc() {
  auto R = get_block_lru(last_gc_);
  R.ensure();

  auto N = R.move_as_ok();
  last_gc_ = N.next;
  alarm_timestamp() = td::Timestamp::in(0.01);
}

BlockDb::DbEntry::DbEntry(tl_object_ptr<ton_api::db_blockdb_lru> entry)
    : block_id(create_block_id(entry->id_)), prev(entry->prev_), next(entry->next_) {
}

tl_object_ptr<ton_api::db_blockdb_lru> BlockDb::DbEntry::release() {
  return create_tl_object<ton_api::db_blockdb_lru>(create_tl_block_id(block_id), prev, next);
}

void BlockDb::truncate(td::Ref<MasterchainState> state, td::Promise<td::Unit> promise) {
  std::map<ShardIdFull, BlockSeqno> max_seqno;
  max_seqno.emplace(ShardIdFull{masterchainId}, state->get_seqno() + 1);

  auto shards = state->get_shards();
  auto it = KeyHash::zero();
  kv_->begin_transaction().ensure();
  while (true) {
    auto R = get_block_lru(it);
    R.ensure();
    auto v = R.move_as_ok();
    it = v.next;
    R = get_block_lru(it);
    R.ensure();
    v = R.move_as_ok();
    if (v.is_empty()) {
      break;
    }

    auto s = v.block_id.shard_full();
    if (!max_seqno.count(s)) {
      bool found = false;
      for (auto &shard : shards) {
        if (shard_intersects(shard->shard(), s)) {
          found = true;
          max_seqno.emplace(s, shard->top_block_id().seqno() + 1);
          break;
        }
      }
      if (!found) {
        max_seqno.emplace(s, 0);
      }
    }

    bool to_delete = v.block_id.seqno() >= max_seqno[s];
    if (to_delete) {
      auto key_hash = get_block_value_key(v.block_id);
      auto B = get_block_value(key_hash);
      B.ensure();
      auto handleR = create_block_handle(B.move_as_ok());
      handleR.ensure();
      auto handle = handleR.move_as_ok();

      handle->unsafe_clear_applied();
      handle->unsafe_clear_next();

      if (handle->need_flush()) {
        set_block_value(key_hash, handle->serialize());
      }
    } else if (v.block_id.seqno() + 1 == max_seqno[s]) {
      auto key_hash = get_block_value_key(v.block_id);
      auto B = get_block_value(key_hash);
      B.ensure();
      auto handleR = create_block_handle(B.move_as_ok());
      handleR.ensure();
      auto handle = handleR.move_as_ok();

      handle->unsafe_clear_next();

      if (handle->need_flush()) {
        set_block_value(key_hash, handle->serialize());
      }
    }
  }
  kv_->commit_transaction().ensure();
}

}  // namespace validator

}  // namespace ton
