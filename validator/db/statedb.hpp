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
#include "td/db/KeyValueAsync.h"
#include "ton/ton-types.h"

#include "validator/interfaces/db.h"

namespace ton {

namespace validator {

class RootDb;

class StateDb : public td::actor::Actor {
 public:
  void update_init_masterchain_block(BlockIdExt block, td::Promise<td::Unit> promise);
  void get_init_masterchain_block(td::Promise<BlockIdExt> promise);

  void update_gc_masterchain_block(BlockIdExt block, td::Promise<td::Unit> promise);
  void get_gc_masterchain_block(td::Promise<BlockIdExt> promise);

  void update_shard_client_state(BlockIdExt masterchain_block_id, td::Promise<td::Unit> promise);
  void get_shard_client_state(td::Promise<BlockIdExt> promise);

  void update_destroyed_validator_sessions(std::vector<ValidatorSessionId> sessions, td::Promise<td::Unit> promise);
  void get_destroyed_validator_sessions(td::Promise<std::vector<ValidatorSessionId>> promise);

  void update_async_serializer_state(AsyncSerializerState state, td::Promise<td::Unit> promise);
  void get_async_serializer_state(td::Promise<AsyncSerializerState> promise);

  void update_hardforks(std::vector<BlockIdExt> blocks, td::Promise<td::Unit> promise);
  void get_hardforks(td::Promise<std::vector<BlockIdExt>> promise);

  void update_db_version(td::uint32 version, td::Promise<td::Unit> promise);
  void get_db_version(td::Promise<td::uint32> promise);

  StateDb(td::actor::ActorId<RootDb> root_db, std::string path);

  void start_up() override;

 private:
  using KeyType = td::Bits256;

  std::shared_ptr<td::KeyValue> kv_;

  td::actor::ActorId<RootDb> root_db_;
  std::string db_path_;
};

}  // namespace validator

}  // namespace ton
