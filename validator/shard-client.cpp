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
#include "shard-client.hpp"
#include "ton/ton-io.hpp"
#include "validator/fabric.h"
#include "td/actor/MultiPromise.h"
#include "validator/downloaders/download-state.hpp"

namespace ton {

namespace validator {

void ShardClient::start_up() {
  if (init_mode_) {
    start_up_init_mode();
    return;
  }

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<BlockIdExt> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &ShardClient::got_state_from_db, R.move_as_ok());
  });
  td::actor::send_closure(manager_, &ValidatorManager::get_shard_client_state, true, std::move(P));
}

void ShardClient::start() {
  if (!started_) {
    started_ = true;
    saved_to_db();
  }
}

void ShardClient::got_state_from_db(BlockIdExt state) {
  CHECK(!init_mode_);

  CHECK(state.is_valid());

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<BlockHandle> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &ShardClient::got_init_handle_from_db, R.move_as_ok());
  });
  td::actor::send_closure(manager_, &ValidatorManager::get_block_handle, state, true, std::move(P));
}

void ShardClient::got_init_handle_from_db(BlockHandle handle) {
  masterchain_block_handle_ = std::move(handle);

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<ShardState>> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &ShardClient::got_init_state_from_db, td::Ref<MasterchainState>{R.move_as_ok()});
  });
  td::actor::send_closure(manager_, &ValidatorManager::get_shard_state_from_db, masterchain_block_handle_,
                          std::move(P));
}

void ShardClient::got_init_state_from_db(td::Ref<MasterchainState> state) {
  saved_to_db();
}

void ShardClient::start_up_init_mode() {
  std::vector<BlockIdExt> shards;
  for (const auto& s : masterchain_state_->get_shards()) {
    if (opts_->need_monitor(s->shard(), masterchain_state_)) {
      shards.push_back(s->top_block_id());
    }
  }
  download_shard_states(masterchain_block_handle_->id(), std::move(shards), 0);
}

void ShardClient::download_shard_states(BlockIdExt masterchain_block_id, std::vector<BlockIdExt> shards, size_t idx) {
  if (idx >= shards.size()) {
    LOG(WARNING) << "downloaded all shard states";
    applied_all_shards();
    return;
  }
  BlockIdExt block_id = shards[idx];
  td::actor::create_actor<DownloadShardState>(
      "downloadstate", block_id, masterchain_block_handle_->id(), 2, manager_, td::Timestamp::in(3600 * 5),
      [=, SelfId = actor_id(this), shards = std::move(shards)](td::Result<td::Ref<ShardState>> R) {
        R.ensure();
        td::actor::send_closure(SelfId, &ShardClient::download_shard_states, masterchain_block_id, std::move(shards),
                                idx + 1);
      })
      .release();
}

void ShardClient::applied_all_shards() {
  LOG(DEBUG) << "shardclient: " << masterchain_block_handle_->id() << " finished";
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &ShardClient::saved_to_db);
  });
  td::actor::send_closure(manager_, &ValidatorManager::update_shard_client_state, masterchain_block_handle_->id(),
                          std::move(P));
}

void ShardClient::saved_to_db() {
  CHECK(masterchain_block_handle_);
  td::actor::send_closure(manager_, &ValidatorManager::update_shard_client_block_handle, masterchain_block_handle_,
                          std::move(masterchain_state_), [](td::Unit) {});
  masterchain_state_.clear();
  if (promise_) {
    promise_.set_value(td::Unit());
  }
  if (init_mode_) {
    init_mode_ = false;
  }

  if (!started_) {
    return;
  }
  if (masterchain_block_handle_->inited_next_left()) {
    new_masterchain_block_id(masterchain_block_handle_->one_next(true));
  } else {
    waiting_ = true;
  }
}

void ShardClient::new_masterchain_block_id(BlockIdExt block_id) {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<BlockHandle> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &ShardClient::got_masterchain_block_handle, R.move_as_ok());
  });
  td::actor::send_closure(manager_, &ValidatorManager::get_block_handle, block_id, true, std::move(P));
}

void ShardClient::got_masterchain_block_handle(BlockHandle handle) {
  masterchain_block_handle_ = std::move(handle);
  download_masterchain_state();
}

void ShardClient::download_masterchain_state() {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<ShardState>> R) {
    if (R.is_error()) {
      LOG(WARNING) << "failed to download masterchain state: " << R.move_as_error();
      td::actor::send_closure(SelfId, &ShardClient::download_masterchain_state);
    } else {
      td::actor::send_closure(SelfId, &ShardClient::got_masterchain_block_state,
                              td::Ref<MasterchainState>{R.move_as_ok()});
    }
  });
  td::actor::send_closure(manager_, &ValidatorManager::wait_block_state, masterchain_block_handle_,
                          shard_client_priority(), td::Timestamp::in(600), std::move(P));
}

void ShardClient::got_masterchain_block_state(td::Ref<MasterchainState> state) {
  masterchain_state_ = std::move(state);
  if (started_) {
    apply_all_shards();
  }
}

void ShardClient::apply_all_shards() {
  LOG(DEBUG) << "shardclient: " << masterchain_block_handle_->id() << " started";

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    if (R.is_error()) {
      LOG(WARNING) << "failed to receive shard states: " << R.move_as_error();
      td::actor::send_closure(SelfId, &ShardClient::apply_all_shards);
    } else {
      td::actor::send_closure(SelfId, &ShardClient::applied_all_shards);
    }
  });

  td::MultiPromise mp;
  auto ig = mp.init_guard();
  ig.add_promise(std::move(P));

  auto vec = masterchain_state_->get_shards();
  std::set<WorkchainId> workchains;
  for (auto &shard : vec) {
    workchains.insert(shard->shard().workchain);
    if (opts_->need_monitor(shard->shard(), masterchain_state_)) {
      auto Q = td::PromiseCreator::lambda([SelfId = actor_id(this), promise = ig.get_promise(),
                                           shard = shard->shard()](td::Result<td::Ref<ShardState>> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error_prefix(PSTRING() << "shard " << shard << ": "));
        } else {
          td::actor::send_closure(SelfId, &ShardClient::downloaded_shard_state, R.move_as_ok(), std::move(promise));
        }
      });
      td::actor::send_closure(manager_, &ValidatorManager::wait_block_state_short, shard->top_block_id(),
                              shard_client_priority(), td::Timestamp::in(1500), std::move(Q));
    }
  }
  for (const auto &[wc, desc] : masterchain_state_->get_workchain_list()) {
    if (!workchains.count(wc) && desc->active && opts_->need_monitor(ShardIdFull{wc, shardIdAll}, masterchain_state_)) {
      auto Q = td::PromiseCreator::lambda([SelfId = actor_id(this), promise = ig.get_promise(),
                                           workchain = wc](td::Result<td::Ref<ShardState>> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error_prefix(PSTRING() << "workchain " << workchain << ": "));
        } else {
          td::actor::send_closure(SelfId, &ShardClient::downloaded_shard_state, R.move_as_ok(), std::move(promise));
        }
      });
      td::actor::send_closure(manager_, &ValidatorManager::wait_block_state_short,
                              BlockIdExt{wc, shardIdAll, 0, desc->zerostate_root_hash, desc->zerostate_file_hash},
                              shard_client_priority(), td::Timestamp::in(1500), std::move(Q));
    }
  }
}

void ShardClient::downloaded_shard_state(td::Ref<ShardState> state, td::Promise<td::Unit> promise) {
  run_apply_block_query(state->get_block_id(), td::Ref<BlockData>{}, masterchain_block_handle_->id(), manager_,
                        td::Timestamp::in(600), std::move(promise));
}

void ShardClient::new_masterchain_block_notification(BlockHandle handle, td::Ref<MasterchainState> state) {
  if (!waiting_) {
    return;
  }
  if (handle->id().id.seqno <= masterchain_block_handle_->id().id.seqno) {
    return;
  }
  LOG_CHECK(masterchain_block_handle_->inited_next_left()) << handle->id() << " " << masterchain_block_handle_->id();
  LOG_CHECK(masterchain_block_handle_->one_next(true) == handle->id())
      << handle->id() << " " << masterchain_block_handle_->id();
  masterchain_block_handle_ = std::move(handle);
  masterchain_state_ = std::move(state);
  waiting_ = false;

  apply_all_shards();
}

void ShardClient::get_processed_masterchain_block(td::Promise<BlockSeqno> promise) {
  auto seqno = masterchain_block_handle_ ? masterchain_block_handle_->id().id.seqno : 0;
  if (seqno > 0 && !waiting_) {
    seqno--;
  }
  promise.set_result(seqno);
}

void ShardClient::get_processed_masterchain_block_id(td::Promise<BlockIdExt> promise) {
  if (masterchain_block_handle_) {
    promise.set_result(masterchain_block_handle_->id());
  } else {
    promise.set_error(td::Status::Error(ErrorCode::notready, "shard client not started"));
  }
}

void ShardClient::force_update_shard_client(BlockHandle handle, td::Promise<td::Unit> promise) {
  CHECK(!init_mode_);
  CHECK(!started_);

  if (masterchain_block_handle_->id().seqno() >= handle->id().seqno()) {
    promise.set_value(td::Unit());
    return;
  }

  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), handle, promise = std::move(promise)](td::Result<td::Ref<ShardState>> R) mutable {
        R.ensure();
        td::actor::send_closure(SelfId, &ShardClient::force_update_shard_client_ex, std::move(handle),
                                td::Ref<MasterchainState>{R.move_as_ok()}, std::move(promise));
      });
  td::actor::send_closure(manager_, &ValidatorManager::get_shard_state_from_db, std::move(handle), std::move(P));
}

void ShardClient::force_update_shard_client_ex(BlockHandle handle, td::Ref<MasterchainState> state,
                                               td::Promise<td::Unit> promise) {
  CHECK(!init_mode_);
  CHECK(!started_);

  if (masterchain_block_handle_->id().seqno() >= handle->id().seqno()) {
    promise.set_value(td::Unit());
    return;
  }
  masterchain_block_handle_ = std::move(handle);
  masterchain_state_ = std::move(state);
  promise_ = std::move(promise);
  applied_all_shards();
}

void ShardClient::update_options(td::Ref<ValidatorManagerOptions> opts) {
  opts_ = std::move(opts);
}

}  // namespace validator

}  // namespace ton
