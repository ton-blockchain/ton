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

#include "interfaces/validator-manager.h"
#include <set>

namespace ton {

namespace validator {

class ShardClient : public td::actor::Actor {
 private:
  td::Ref<ValidatorManagerOptions> opts_;

  BlockHandle masterchain_block_handle_;
  td::Ref<MasterchainState> masterchain_state_;

  std::vector<td::actor::ActorOwn<ShardClient>> children_;

  bool waiting_ = false;
  bool init_mode_ = false;
  bool started_ = false;

  td::actor::ActorId<ValidatorManager> manager_;

  td::Promise<td::Unit> promise_;

  std::set<ShardIdFull> created_overlays_;

 public:
  ShardClient(td::Ref<ValidatorManagerOptions> opts, BlockHandle masterchain_block_handle,
              td::Ref<MasterchainState> masterchain_state, td::actor::ActorId<ValidatorManager> manager,
              td::Promise<td::Unit> promise)
      : opts_(std::move(opts))
      , masterchain_block_handle_(masterchain_block_handle)
      , masterchain_state_(std::move(masterchain_state))
      , manager_(manager)
      , promise_(std::move(promise)) {
    init_mode_ = true;
  }
  ShardClient(td::Ref<ValidatorManagerOptions> opts, td::actor::ActorId<ValidatorManager> manager,
              td::Promise<td::Unit> promise)
      : opts_(std::move(opts)), manager_(manager), promise_(std::move(promise)) {
  }

  static constexpr td::uint32 shard_client_priority() {
    return 2;
  }

  void build_shard_overlays();

  void start_up() override;
  void start_up_init_mode();
  void start_up_init_mode_finished();
  void start();
  void got_state_from_db(BlockIdExt masterchain_block_id);
  void got_init_handle_from_db(BlockHandle handle);
  void got_init_state_from_db(td::Ref<MasterchainState> state);

  void im_download_shard_state(BlockIdExt block_id, td::Promise<td::Unit> promise);
  void im_downloaded_zero_state(BlockIdExt block_id, td::BufferSlice data, td::Promise<td::Unit> promise);
  void im_downloaded_proof_link(BlockIdExt block_id, td::BufferSlice data, td::Promise<td::Unit> promise);
  void im_checked_proof_link(BlockIdExt block_id, td::Promise<td::Unit> promise);
  void im_downloaded_shard_state(BlockIdExt block_id, td::Promise<td::Unit> promise);
  void im_got_shard_handle(BlockHandle handle, td::Promise<td::Unit> promise);

  void new_masterchain_block_id(BlockIdExt masterchain_block_id);
  void got_masterchain_block_handle(BlockHandle handle);
  void download_masterchain_state();
  void got_masterchain_block_state(td::Ref<MasterchainState> state);
  void apply_all_shards();
  void downloaded_shard_state(td::Ref<ShardState> state, td::Promise<td::Unit> promise);
  void applied_all_shards();
  void saved_to_db();

  void new_masterchain_block_notification(BlockHandle handle, td::Ref<MasterchainState> state);

  void get_processed_masterchain_block(td::Promise<BlockSeqno> promise);
  void get_processed_masterchain_block_id(td::Promise<BlockIdExt> promise);

  void force_update_shard_client(BlockHandle handle, td::Promise<td::Unit> promise);
  void force_update_shard_client_ex(BlockHandle handle, td::Ref<MasterchainState> state, td::Promise<td::Unit> promise);
};

}  // namespace validator

}  // namespace ton
