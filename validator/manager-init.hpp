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
#include "ton/ton-types.h"

#include "interfaces/validator-manager.h"
#include "interfaces/db.h"
#include "shard-client.hpp"

#include "manager-init.h"

namespace ton {

namespace validator {

class ValidatorManagerMasterchainReiniter : public td::actor::Actor {
 public:
  ValidatorManagerMasterchainReiniter(td::Ref<ValidatorManagerOptions> opts,
                                      td::actor::ActorId<ValidatorManager> manager, td::actor::ActorId<Db> db,
                                      td::Promise<ValidatorManagerInitResult> promise)
      : opts_(std::move(opts)), manager_(manager), db_(db), promise_(std::move(promise)) {
    block_id_ = opts_->init_block_id();
  }
  void start_up() override;
  void written_hardforks();
  void got_masterchain_handle(BlockHandle handle);
  void download_proof_link();
  void downloaded_proof_link(td::BufferSlice data);
  void downloaded_zero_state();

  void try_download_key_blocks(bool try_start);
  void got_next_key_blocks(std::vector<BlockIdExt> vec);
  void got_key_block_handle(td::uint32 idx, BlockHandle handle);

  void choose_masterchain_state();
  void download_masterchain_state();
  void downloaded_masterchain_state(td::Ref<ShardState> state);

  void downloaded_all_shards();
  void finish();

 private:
  td::Ref<ValidatorManagerOptions> opts_;

  BlockIdExt block_id_;
  BlockHandle handle_;
  td::Ref<MasterchainState> state_;
  BlockHandle last_key_block_handle_;

  std::vector<BlockHandle> key_blocks_;
  td::Timestamp download_new_key_blocks_until_;

  std::vector<ShardIdFull> shards_;

  td::actor::ActorId<ValidatorManager> manager_;
  td::actor::ActorId<Db> db_;

  td::Promise<ValidatorManagerInitResult> promise_;

  td::uint32 pending_ = 0;
  td::actor::ActorOwn<ShardClient> client_;
};

class ValidatorManagerMasterchainStarter : public td::actor::Actor {
 public:
  ValidatorManagerMasterchainStarter(td::Ref<ValidatorManagerOptions> opts,
                                     td::actor::ActorId<ValidatorManager> manager, td::actor::ActorId<Db> db,
                                     td::Promise<ValidatorManagerInitResult> promise)
      : opts_(std::move(opts)), manager_(manager), db_(db), promise_(std::move(promise)) {
  }

  void start_up() override;
  void got_init_block_id(BlockIdExt block_id);
  void failed_to_get_init_block_id();
  void got_init_block_handle(BlockHandle handle);
  void got_init_block_state(td::Ref<MasterchainState> state);
  void got_gc_block_id(BlockIdExt block_id);
  void got_gc_block_handle(BlockHandle handle);
  void got_gc_block_state(td::Ref<MasterchainState> state);
  void got_key_block_handle(BlockHandle handle);
  void got_shard_block_id(BlockIdExt block_id);
  void got_hardforks(std::vector<BlockIdExt> hardforks);
  void got_truncate_block_id(BlockIdExt block_id);
  void got_truncate_block_handle(BlockHandle handle);
  void got_truncate_state(td::Ref<MasterchainState> state);
  void truncated_db();
  void got_prev_key_block_handle(BlockHandle handle);
  void truncated();
  void written_next();
  void start_shard_client();
  void finish();

 private:
  td::Ref<ValidatorManagerOptions> opts_;

  BlockIdExt block_id_;
  BlockHandle handle_;
  td::Ref<MasterchainState> state_;
  BlockHandle gc_handle_;
  td::Ref<MasterchainState> gc_state_;
  BlockHandle last_key_block_handle_;

  td::actor::ActorId<ValidatorManager> manager_;
  td::actor::ActorId<Db> db_;

  td::Promise<ValidatorManagerInitResult> promise_;

  BlockIdExt client_block_id_;
  td::actor::ActorOwn<ShardClient> client_;
};

}  // namespace validator

}  // namespace ton
