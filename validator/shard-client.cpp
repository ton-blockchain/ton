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
#include "ton/ton-io.hpp"
#include "validator/downloaders/download-state.hpp"
#include "validator/fabric.h"

#include "shard-client.hpp"

namespace ton {

namespace validator {

void ShardClient::start_up() {
  initialize_waiter_ = initialize().start();
  initialize_waiter_.get().start().detach_ensure("shard client initialize");
}

td::actor::Task<> ShardClient::initialize() {
  Ref<MasterchainState> mc_state;
  if (init_mode_) {
    mc_state = init_mode_mc_state_;
    co_await initialize_init_mode();
  } else {
    BlockIdExt block_id = co_await td::actor::ask(manager_, &ValidatorManager::get_shard_client_state, true);
    CHECK(block_id.is_valid());
    masterchain_block_handle_ = co_await td::actor::ask(manager_, &ValidatorManager::get_block_handle, block_id, true);
    mc_state = Ref<MasterchainState>{
        co_await td::actor::ask(manager_, &ValidatorManager::get_shard_state_from_db, masterchain_block_handle_)};
  }
  td::actor::send_closure(manager_, &ValidatorManager::update_shard_client_block_handle, masterchain_block_handle_,
                          std::move(mc_state), [](td::Result<>) {});
  if (initialize_promise_) {
    initialize_promise_.set_value(td::Unit{});
  }
  init_mode_ = false;
  processed_masterchain_block_ = masterchain_block_handle_->id().seqno();
  co_return {};
}

td::actor::Task<> ShardClient::initialize_init_mode() {
  LOG(WARNING) << "Downloading shard states for masterchain block " << init_mode_mc_state_->get_block_id();
  for (const auto &s : init_mode_mc_state_->get_shards()) {
    if (opts_->need_monitor(s->shard(), init_mode_mc_state_)) {
      auto block_id = s->top_block_id();
      auto split_depth = init_mode_mc_state_->persistent_state_split_depth(s->shard().workchain);
      auto [task, promise] = td::actor::StartedTask<Ref<ShardState>>::make_bridge();
      td::actor::create_actor<DownloadShardState>("downloadstate", block_id, masterchain_block_handle_->id(),
                                                  split_depth, 2, manager_, td::Timestamp::in(3600 * 5),
                                                  std::move(promise))
          .release();
      co_await std::move(task);
    }
  }
  LOG(WARNING) << "Downloaded all shard states";
  co_await td::actor::ask(manager_, &ValidatorManager::update_shard_client_state, masterchain_block_handle_->id());
  init_mode_mc_state_.clear();
  co_return {};
}

void ShardClient::start() {
  started_ = true;
  run().start().detach_ensure("shard client");
}

td::actor::Task<> ShardClient::run() {
  co_await initialize_waiter_.get();
  CHECK(masterchain_block_handle_);
  run_preprocess().start().detach_ensure("shard client preprocess");

  while (true) {
    if (!masterchain_block_handle_->inited_next_left()) {
      co_await wait();
      continue;
    }
    masterchain_block_handle_ = co_await td::actor::ask(manager_, &ValidatorManager::get_block_handle,
                                                        masterchain_block_handle_->one_next(true), true);
    auto mc_state = co_await wait_mc_state(masterchain_block_handle_);
    while (true) {
      auto R = co_await apply_all_shards(mc_state).wrap();
      if (R.is_error()) {
        LOG(WARNING) << "failed to receive shard states: " << R.move_as_error();
        continue;
      }
      break;
    }
    co_await td::actor::ask(manager_, &ValidatorManager::update_shard_client_state, masterchain_block_handle_->id());
    processed_masterchain_block_ = masterchain_block_handle_->id().seqno();
    td::actor::send_closure(manager_, &ValidatorManager::update_shard_client_block_handle, masterchain_block_handle_,
                            std::move(mc_state), [](td::Result<>) {});
    notify();
  }
}

td::actor::Task<> ShardClient::run_preprocess() {
  BlockHandle handle = masterchain_block_handle_;
  while (true) {
    if (!handle->inited_next_left() || handle->id().seqno() >= processed_masterchain_block_ + MAX_PREPROCESS_DELTA) {
      co_await wait();
      continue;
    }
    handle = co_await td::actor::ask(manager_, &ValidatorManager::get_block_handle, handle->one_next(true), true);
    auto mc_state = co_await wait_mc_state(handle);
    wait_shard_states(mc_state).start().detach_silent();
  }
}

td::actor::Task<> ShardClient::apply_all_shards(Ref<MasterchainState> mc_state) {
  LOG(DEBUG) << "shardclient: " << masterchain_block_handle_->id() << " started";
  std::vector<td::actor::StartedTask<>> tasks;

  auto vec = mc_state->get_shards();
  std::set<WorkchainId> workchains;
  for (auto &shard : vec) {
    workchains.insert(shard->shard().workchain);
    if (opts_->need_monitor(shard->shard(), mc_state)) {
      tasks.push_back(apply_shard(shard->top_block_id()).start());
    }
  }
  for (const auto &[wc, desc] : mc_state->get_workchain_list()) {
    if (!workchains.contains(wc) && desc->active && opts_->need_monitor(ShardIdFull{wc, shardIdAll}, mc_state)) {
      tasks.push_back(
          apply_shard(BlockIdExt{wc, shardIdAll, 0, desc->zerostate_root_hash, desc->zerostate_file_hash}).start());
    }
  }
  co_await td::actor::all(std::move(tasks));
  LOG(DEBUG) << "shardclient: " << masterchain_block_handle_->id() << " finished";
  co_return {};
}

td::actor::Task<> ShardClient::apply_shard(BlockIdExt block_id) {
  auto state = co_await td::actor::ask(manager_, &ValidatorManager::wait_block_state_short, block_id,
                                       SHARD_CLIENT_PRIORITY, td::Timestamp::in(1500), true);
  auto [task, promise] = td::actor::StartedTask<>::make_bridge();
  run_apply_block_query(state->get_block_id(), Ref<BlockData>{}, masterchain_block_handle_->id(), manager_,
                        td::Timestamp::in(600), std::move(promise));
  co_await std::move(task);
  co_return {};
}

td::actor::Task<> ShardClient::wait_shard_states(Ref<MasterchainState> mc_state) {
  LOG(DEBUG) << "shardclient preprocess: " << masterchain_block_handle_->id() << " started";
  std::vector<td::actor::StartedTask<Ref<ShardState>>> tasks;

  auto vec = mc_state->get_shards();
  std::set<WorkchainId> workchains;
  for (auto &shard : vec) {
    workchains.insert(shard->shard().workchain);
    if (opts_->need_monitor(shard->shard(), mc_state)) {
      tasks.push_back(td::actor::ask(manager_, &ValidatorManager::wait_block_state_short, shard->top_block_id(),
                                     SHARD_CLIENT_PRIORITY, td::Timestamp::in(1500), true));
    }
  }
  auto R = co_await td::actor::all(std::move(tasks)).wrap();
  if (R.is_ok()) {
    LOG(DEBUG) << "shardclient preprocess: " << masterchain_block_handle_->id() << " finished";
  } else {
    LOG(WARNING) << "shardclient preprocess: " << masterchain_block_handle_->id() << " error: " << R.move_as_error();
  }
  co_return {};
}

td::actor::Task<Ref<MasterchainState>> ShardClient::wait_mc_state(BlockHandle handle) {
  while (true) {
    auto R = co_await td::actor::ask(manager_, &ValidatorManager::wait_block_state, handle, SHARD_CLIENT_PRIORITY,
                                     td::Timestamp::in(600), true)
                 .wrap();
    if (R.is_error()) {
      LOG(WARNING) << "Failed to get masterchain state: " << R.move_as_error();
      continue;
    }
    co_return Ref<MasterchainState>{R.move_as_ok()};
  }
}

void ShardClient::new_masterchain_block_notification() {
  notify();
}

void ShardClient::get_processed_masterchain_block(td::Promise<BlockSeqno> promise) {
  promise.set_result(processed_masterchain_block_);
}

td::actor::Task<> ShardClient::force_update_shard_client_ex(BlockHandle handle, Ref<MasterchainState> state) {
  CHECK(!init_mode_);
  CHECK(!started_);
  CHECK(masterchain_block_handle_);
  if (masterchain_block_handle_->id().seqno() >= handle->id().seqno()) {
    co_return {};
  }
  masterchain_block_handle_ = std::move(handle);

  (co_await td::actor::ask(manager_, &ValidatorManager::update_shard_client_state, masterchain_block_handle_->id())
       .wrap())
      .ensure();
  CHECK(!started_);
  processed_masterchain_block_ = masterchain_block_handle_->id().seqno();
  td::actor::send_closure(manager_, &ValidatorManager::update_shard_client_block_handle, masterchain_block_handle_,
                          std::move(state), [](td::Result<>) {});
  co_return {};
}

void ShardClient::update_options(Ref<ValidatorManagerOptions> opts) {
  opts_ = std::move(opts);
}

}  // namespace validator

}  // namespace ton
