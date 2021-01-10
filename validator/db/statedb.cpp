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
#include "statedb.hpp"
#include "ton/ton-tl.hpp"
#include "adnl/utils.hpp"
#include "td/db/RocksDb.h"
#include "ton/ton-shard.h"

namespace ton {

namespace validator {

void StateDb::update_init_masterchain_block(BlockIdExt block, td::Promise<td::Unit> promise) {
  auto key = create_hash_tl_object<ton_api::db_state_key_initBlockId>();

  kv_->begin_write_batch().ensure();
  kv_->set(key.as_slice(),
           create_serialize_tl_object<ton_api::db_state_initBlockId>(create_tl_block_id(block)).as_slice())
      .ensure();
  kv_->commit_write_batch().ensure();

  promise.set_value(td::Unit());
}

void StateDb::get_init_masterchain_block(td::Promise<BlockIdExt> promise) {
  auto key = create_hash_tl_object<ton_api::db_state_key_initBlockId>();

  std::string value;
  auto R = kv_->get(key.as_slice(), value);
  R.ensure();

  if (R.move_as_ok() == td::KeyValue::GetStatus::NotFound) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "not found"));
    return;
  }

  auto F = fetch_tl_object<ton_api::db_state_initBlockId>(td::BufferSlice{value}, true);
  F.ensure();
  auto obj = F.move_as_ok();
  promise.set_value(create_block_id(obj->block_));
}

void StateDb::update_gc_masterchain_block(BlockIdExt block, td::Promise<td::Unit> promise) {
  auto key = create_hash_tl_object<ton_api::db_state_key_gcBlockId>();

  kv_->begin_write_batch().ensure();
  kv_->set(key.as_slice(),
           create_serialize_tl_object<ton_api::db_state_gcBlockId>(create_tl_block_id(block)).as_slice())
      .ensure();
  kv_->commit_write_batch().ensure();

  promise.set_value(td::Unit());
}

void StateDb::get_gc_masterchain_block(td::Promise<BlockIdExt> promise) {
  auto key = create_hash_tl_object<ton_api::db_state_key_gcBlockId>();

  std::string value;
  auto R = kv_->get(key.as_slice(), value);
  R.ensure();

  if (R.move_as_ok() == td::KeyValue::GetStatus::NotFound) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "not found"));
    return;
  }

  auto F = fetch_tl_object<ton_api::db_state_gcBlockId>(td::BufferSlice{value}, true);
  F.ensure();
  auto obj = F.move_as_ok();
  promise.set_value(create_block_id(obj->block_));
}

void StateDb::update_shard_client_state(BlockIdExt masterchain_block_id, td::Promise<td::Unit> promise) {
  auto key = create_hash_tl_object<ton_api::db_state_key_shardClient>();

  kv_->begin_write_batch().ensure();
  kv_->set(key.as_slice(),
           create_serialize_tl_object<ton_api::db_state_shardClient>(create_tl_block_id(masterchain_block_id)))
      .ensure();
  kv_->commit_write_batch().ensure();

  promise.set_value(td::Unit());
}

void StateDb::get_shard_client_state(td::Promise<BlockIdExt> promise) {
  auto key = create_hash_tl_object<ton_api::db_state_key_shardClient>();

  std::string value;
  auto R = kv_->get(key.as_slice(), value);
  R.ensure();

  if (R.move_as_ok() == td::KeyValue::GetStatus::NotFound) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "not found"));
    return;
  }

  auto F = fetch_tl_object<ton_api::db_state_shardClient>(td::BufferSlice{value}, true);
  F.ensure();
  auto obj = F.move_as_ok();
  promise.set_value(create_block_id(obj->block_));
}

void StateDb::update_destroyed_validator_sessions(std::vector<ValidatorSessionId> sessions,
                                                  td::Promise<td::Unit> promise) {
  auto key = create_hash_tl_object<ton_api::db_state_key_destroyedSessions>();

  kv_->begin_write_batch().ensure();
  kv_->set(key.as_slice(), create_serialize_tl_object<ton_api::db_state_destroyedSessions>(std::move(sessions)))
      .ensure();
  kv_->commit_write_batch().ensure();

  promise.set_value(td::Unit());
}

void StateDb::get_destroyed_validator_sessions(td::Promise<std::vector<ValidatorSessionId>> promise) {
  auto key = create_hash_tl_object<ton_api::db_state_key_destroyedSessions>();

  std::string value;
  auto R = kv_->get(key.as_slice(), value);
  R.ensure();

  if (R.move_as_ok() == td::KeyValue::GetStatus::NotFound) {
    promise.set_value(std::vector<ValidatorSessionId>{});
    return;
  }

  auto F = fetch_tl_object<ton_api::db_state_destroyedSessions>(td::BufferSlice{value}, true);
  F.ensure();
  auto obj = F.move_as_ok();
  promise.set_value(std::move(obj->sessions_));
}

void StateDb::update_async_serializer_state(AsyncSerializerState state, td::Promise<td::Unit> promise) {
  auto key = create_hash_tl_object<ton_api::db_state_key_asyncSerializer>();

  auto value = create_serialize_tl_object<ton_api::db_state_asyncSerializer>(
      create_tl_block_id(state.last_block_id), create_tl_block_id(state.last_written_block_id),
      state.last_written_block_ts);

  kv_->begin_write_batch().ensure();
  kv_->set(key.as_slice(), value.as_slice()).ensure();
  kv_->commit_write_batch().ensure();

  promise.set_value(td::Unit());
}

void StateDb::get_async_serializer_state(td::Promise<AsyncSerializerState> promise) {
  auto key = create_hash_tl_object<ton_api::db_state_key_asyncSerializer>();

  std::string value;
  auto R = kv_->get(key.as_slice(), value);
  R.ensure();

  if (R.move_as_ok() == td::KeyValue::GetStatus::NotFound) {
    promise.set_value(AsyncSerializerState{BlockIdExt{}, BlockIdExt{}, 0});
    return;
  }

  auto F = fetch_tl_object<ton_api::db_state_asyncSerializer>(td::BufferSlice{value}, true);
  F.ensure();
  auto obj = F.move_as_ok();
  promise.set_value(AsyncSerializerState{create_block_id(obj->block_), create_block_id(obj->last_),
                                         static_cast<UnixTime>(obj->last_ts_)});
}

void StateDb::update_hardforks(std::vector<BlockIdExt> blocks, td::Promise<td::Unit> promise) {
  auto key = create_hash_tl_object<ton_api::db_state_key_hardforks>();

  std::vector<tl_object_ptr<ton_api::tonNode_blockIdExt>> vec;

  for (auto &e : blocks) {
    vec.push_back(create_tl_block_id(e));
  }

  kv_->begin_write_batch().ensure();
  kv_->set(key.as_slice(), create_serialize_tl_object<ton_api::db_state_hardforks>(std::move(vec))).ensure();
  kv_->commit_write_batch();

  promise.set_value(td::Unit());
}

void StateDb::get_hardforks(td::Promise<std::vector<BlockIdExt>> promise) {
  auto key = create_hash_tl_object<ton_api::db_state_key_hardforks>();

  std::string value;
  auto R = kv_->get(key.as_slice(), value);
  R.ensure();
  if (R.move_as_ok() == td::KeyValue::GetStatus::NotFound) {
    promise.set_value(std::vector<BlockIdExt>{});
    return;
  }
  auto F = fetch_tl_object<ton_api::db_state_hardforks>(value, true);
  F.ensure();
  auto f = F.move_as_ok();

  std::vector<BlockIdExt> vec;
  for (auto &e : f->blocks_) {
    vec.push_back(create_block_id(e));
  }

  promise.set_value(std::move(vec));
}

StateDb::StateDb(td::actor::ActorId<RootDb> root_db, std::string db_path) : root_db_(root_db), db_path_(db_path) {
}

void StateDb::start_up() {
  kv_ = std::make_shared<td::RocksDb>(td::RocksDb::open(db_path_).move_as_ok());

  std::string value;
  auto R = kv_->get(create_serialize_tl_object<ton_api::db_state_key_dbVersion>(), value);
  R.ensure();
  if (R.move_as_ok() == td::KeyValue::GetStatus::Ok) {
    auto F = fetch_tl_object<ton_api::db_state_dbVersion>(value, true);
    F.ensure();
    auto f = F.move_as_ok();
    CHECK(f->version_ == 2);
  } else {
    kv_->begin_write_batch().ensure();
    kv_->set(create_serialize_tl_object<ton_api::db_state_key_dbVersion>(),
             create_serialize_tl_object<ton_api::db_state_dbVersion>(2))
        .ensure();
    kv_->commit_write_batch().ensure();
  }
}

void StateDb::truncate(BlockSeqno masterchain_seqno, ConstBlockHandle handle, td::Promise<td::Unit> promise) {
  {
    auto key = create_hash_tl_object<ton_api::db_state_key_asyncSerializer>();

    std::string value;
    auto R = kv_->get(key.as_slice(), value);
    R.ensure();

    if (R.move_as_ok() == td::KeyValue::GetStatus::Ok) {
      auto F = fetch_tl_object<ton_api::db_state_asyncSerializer>(value, true);
      F.ensure();
      auto obj = F.move_as_ok();
      if (static_cast<BlockSeqno>(obj->last_->seqno_) > masterchain_seqno) {
        CHECK(handle);
        CHECK(handle->inited_unix_time());
        obj->last_ = create_tl_block_id(handle->id());
        obj->last_ts_ = handle->unix_time();
        kv_->begin_write_batch().ensure();
        kv_->set(key.as_slice(), serialize_tl_object(obj, true)).ensure();
        kv_->commit_write_batch().ensure();
      }
    }
  }
  {
    auto key = create_hash_tl_object<ton_api::db_state_key_shardClient>();

    std::string value;
    auto R = kv_->get(key.as_slice(), value);
    R.ensure();

    if (R.move_as_ok() == td::KeyValue::GetStatus::Ok) {
      auto F = fetch_tl_object<ton_api::db_state_shardClient>(td::BufferSlice{value}, true);
      F.ensure();
      auto obj = F.move_as_ok();
      if (static_cast<BlockSeqno>(obj->block_->seqno_) > masterchain_seqno) {
        CHECK(handle);
        obj->block_ = create_tl_block_id(handle->id());
        kv_->begin_write_batch().ensure();
        kv_->set(key.as_slice(), serialize_tl_object(obj, true)).ensure();
        kv_->commit_write_batch().ensure();
      }
    }
  }
  {
    auto key = create_hash_tl_object<ton_api::db_state_key_gcBlockId>();

    std::string value;
    auto R = kv_->get(key.as_slice(), value);
    R.ensure();

    if (R.move_as_ok() == td::KeyValue::GetStatus::Ok) {
      auto F = fetch_tl_object<ton_api::db_state_gcBlockId>(td::BufferSlice{value}, true);
      F.ensure();
      auto obj = F.move_as_ok();
      CHECK(static_cast<BlockSeqno>(obj->block_->seqno_) <= masterchain_seqno);
    }
  }
  {
    auto key = create_hash_tl_object<ton_api::db_state_key_initBlockId>();

    std::string value;
    auto R = kv_->get(key.as_slice(), value);
    R.ensure();

    if (R.move_as_ok() == td::KeyValue::GetStatus::Ok) {
      auto F = fetch_tl_object<ton_api::db_state_initBlockId>(td::BufferSlice{value}, true);
      F.ensure();
      auto obj = F.move_as_ok();
      if (static_cast<BlockSeqno>(obj->block_->seqno_) > masterchain_seqno) {
        CHECK(handle);
        obj->block_ = create_tl_block_id(handle->id());
        kv_->begin_write_batch().ensure();
        kv_->set(key.as_slice(), serialize_tl_object(obj, true)).ensure();
        kv_->commit_write_batch().ensure();
      }
    }
  }

  promise.set_value(td::Unit());
}

}  // namespace validator

}  // namespace ton
