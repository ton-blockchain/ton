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
#include "adnl/utils.hpp"
#include "common/checksum.h"
#include "common/delay.h"
#include "downloaders/wait-block-state.hpp"
#include "td/actor/MultiPromise.h"
#include "td/utils/filesystem.h"
#include "ton/ton-io.hpp"
#include "validator/downloaders/download-state.hpp"

#include "fabric.h"
#include "manager-init.hpp"

namespace ton {

namespace validator {

void ValidatorManagerMasterchainReiniter::start_up() {
  status_ = ProcessStatus(manager_, "process.initial_sync");
  status_.set_status(PSTRING() << "starting, init block seqno " << block_id_.seqno());
  LOG(INFO) << "init_block_id=" << block_id_;
  CHECK(block_id_.is_masterchain());
  CHECK(block_id_.id.shard == shardIdAll);
  CHECK(block_id_.seqno() >= opts_->get_last_fork_masterchain_seqno());

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &ValidatorManagerMasterchainReiniter::written_hardforks);
  });
  td::actor::send_closure(db_, &Db::update_hardforks, opts_->get_hardforks(), std::move(P));
}

void ValidatorManagerMasterchainReiniter::written_hardforks() {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<BlockHandle> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &ValidatorManagerMasterchainReiniter::got_masterchain_handle, R.move_as_ok());
  });

  td::actor::send_closure(manager_, &ValidatorManager::get_block_handle, block_id_, true, std::move(P));
}

void ValidatorManagerMasterchainReiniter::got_masterchain_handle(BlockHandle handle) {
  handle_ = std::move(handle);
  key_blocks_.push_back(handle_);

  if (opts_->initial_sync_disabled() && handle_->id().seqno() == 0) {
    status_.set_status(PSTRING() << "downloading masterchain state " << handle_->id().seqno());
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<ShardState>> R) {
      R.ensure();
      td::actor::send_closure(SelfId, &ValidatorManagerMasterchainReiniter::download_masterchain_state);
    });
    td::actor::create_actor<DownloadShardState>("downloadstate", handle_->id(), BlockIdExt{}, 0, 2, manager_,
                                                td::Timestamp::in(3600), std::move(P))
        .release();
    return;
  }

  download_proof_link();
}

void ValidatorManagerMasterchainReiniter::download_proof_link(bool try_local) {
  if (handle_->id().id.seqno == 0) {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<ShardState>> R) {
      R.ensure();
      td::actor::send_closure(SelfId, &ValidatorManagerMasterchainReiniter::downloaded_zero_state);
    });
    td::actor::create_actor<DownloadShardState>("downloadstate", handle_->id(), BlockIdExt{}, 0, 2, manager_,
                                                td::Timestamp::in(3600), std::move(P))
        .release();
  } else {
    auto P = td::PromiseCreator::lambda([=, SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
      if (R.is_error()) {
        if (try_local) {
          LOG(DEBUG) << "failed to get proof link from local import: " << R.move_as_error();
        } else {
          LOG(WARNING) << "failed to download proof link: " << R.move_as_error();
        }
        delay_action(
            [SelfId]() {
              td::actor::send_closure(SelfId, &ValidatorManagerMasterchainReiniter::download_proof_link, false);
            },
            td::Timestamp::in(1.0));
      } else {
        td::actor::send_closure(SelfId, &ValidatorManagerMasterchainReiniter::downloaded_proof_link, R.move_as_ok());
      }
    });
    if (try_local) {
      td::actor::send_closure(manager_, &ValidatorManager::get_block_proof_link_from_import, handle_->id(),
                              handle_->id(), std::move(P));
    } else {
      td::actor::send_closure(manager_, &ValidatorManager::send_get_block_proof_link_request, handle_->id(), 2,
                              std::move(P));
    }
  }
}

void ValidatorManagerMasterchainReiniter::downloaded_proof_link(td::BufferSlice data) {
  auto r_proof = create_proof(handle_->id(), std::move(data));
  if (r_proof.is_error()) {
    LOG(WARNING) << "bad proof link: " << r_proof.move_as_error();
    download_proof_link();
    return;
  }
  auto proof = r_proof.move_as_ok();

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), db = db_, proof](td::Result<BlockHandle> R) {
    if (R.is_error()) {
      LOG(WARNING) << "downloaded proof link failed: " << R.move_as_error();
      td::actor::send_closure(SelfId, &ValidatorManagerMasterchainReiniter::download_proof_link, false);
    } else {
      auto P = td::PromiseCreator::lambda([SelfId, handle = R.move_as_ok()](td::Result<td::Unit> R) {
        R.ensure();
        td::actor::send_closure(SelfId, &ValidatorManagerMasterchainReiniter::try_download_key_blocks, false);
      });
      td::actor::send_closure(db, &Db::add_key_block_proof_link, proof, std::move(P));
    }
  });
  run_check_proof_query(handle_->id(), proof, manager_, td::Timestamp::in(60.0), std::move(P),
                        /* skip_check_signatures = */ true);
}

void ValidatorManagerMasterchainReiniter::downloaded_zero_state() {
  try_download_key_blocks(false);
}

void ValidatorManagerMasterchainReiniter::try_download_key_blocks(bool try_start) {
  if (opts_->initial_sync_disabled()) {
    download_masterchain_state();
    return;
  }
  if (!download_new_key_blocks_until_) {
    if (opts_->allow_blockchain_init()) {
      download_new_key_blocks_until_ = td::Timestamp::in(60.0);
    } else {
      download_new_key_blocks_until_ = td::Timestamp::in(600.0);
    }
  }
  if (key_blocks_.size() > 0 && try_start) {
    auto h = *key_blocks_.rbegin();
    CHECK(h->inited_unix_time());
    if (h->unix_time() + opts_->sync_blocks_before() > td::Clocks::system()) {
      choose_masterchain_state();
      return;
    }
    if (h->unix_time() + 2 * opts_->key_block_utime_step() > td::Clocks::system()) {
      choose_masterchain_state();
      return;
    }
    if (opts_->allow_blockchain_init() && download_new_key_blocks_until_.is_in_past()) {
      choose_masterchain_state();
      return;
    }
  }

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<std::vector<BlockIdExt>> R) {
    if (R.is_error()) {
      LOG(WARNING) << "failed to download key blocks: " << R.move_as_error();
      td::actor::send_closure(SelfId, &ValidatorManagerMasterchainReiniter::got_next_key_blocks,
                              std::vector<BlockIdExt>{});
    } else {
      td::actor::send_closure(SelfId, &ValidatorManagerMasterchainReiniter::got_next_key_blocks, R.move_as_ok());
    }
  });
  td::actor::send_closure(manager_, &ValidatorManager::send_get_next_key_blocks_request, (*key_blocks_.rbegin())->id(),
                          2, std::move(P));
}

void ValidatorManagerMasterchainReiniter::got_next_key_blocks(std::vector<BlockIdExt> vec) {
  if (!vec.size()) {
    delay_action(
        [SelfId = actor_id(this)]() {
          td::actor::send_closure(SelfId, &ValidatorManagerMasterchainReiniter::try_download_key_blocks, true);
        },
        td::Timestamp::in(1.0));
    return;
  }
  if (download_new_key_blocks_until_) {
    if (opts_->allow_blockchain_init()) {
      download_new_key_blocks_until_ = td::Timestamp::in(60.0);
    } else {
      download_new_key_blocks_until_ = td::Timestamp::in(600.0);
    }
  }
  auto s = static_cast<td::uint32>(key_blocks_.size());
  key_blocks_.resize(key_blocks_.size() + vec.size(), nullptr);

  pending_ = static_cast<td::uint32>(vec.size());
  CHECK(pending_ > 0);

  for (td::uint32 i = 0; i < pending_; i++) {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), idx = i + s](td::Result<BlockHandle> R) {
      R.ensure();
      td::actor::send_closure(SelfId, &ValidatorManagerMasterchainReiniter::got_key_block_handle, idx, R.move_as_ok());
    });
    td::actor::send_closure(manager_, &ValidatorManager::get_block_handle, vec[i], true, std::move(P));
  }
}

void ValidatorManagerMasterchainReiniter::got_key_block_handle(td::uint32 idx, BlockHandle handle) {
  CHECK(!key_blocks_[idx]);
  CHECK(handle->inited_proof());
  CHECK(handle->is_key_block());
  if (idx + 1 == key_blocks_.size()) {
    int ago = (int)td::Clocks::system() - (int)handle->unix_time();
    LOG(WARNING) << "last key block is " << handle->id().to_str() << ", " << ago << "s ago";
    status_.set_status(PSTRING() << "last key block is " << handle->id().seqno() << ", " << ago << " s ago");
  }
  key_blocks_[idx] = std::move(handle);
  CHECK(pending_ > 0);
  if (!--pending_) {
    try_download_key_blocks(false);
  }
}

void ValidatorManagerMasterchainReiniter::choose_masterchain_state() {
  BlockHandle handle = handle_;

  for (size_t i = 0; i < key_blocks_.size(); i++) {
    auto h = key_blocks_[key_blocks_.size() - 1 - i];
    BlockHandle p = nullptr;
    if (i < key_blocks_.size() - 1) {
      p = key_blocks_[key_blocks_.size() - 2 - i];
    }

    LOG(INFO) << "key block candidate: seqno=" << h->id().seqno()
              << " is_persistent=" << (!p || ValidatorManager::is_persistent_state(h->unix_time(), p->unix_time()))
              << " ttl=" << ValidatorManager::persistent_state_ttl(h->unix_time())
              << " syncbefore=" << opts_->sync_blocks_before();
    if (h->unix_time() + opts_->sync_blocks_before() > td::Clocks::system()) {
      LOG(INFO) << "ignoring: too new block";
      continue;
    }
    if (!p || ValidatorManager::is_persistent_state(h->unix_time(), p->unix_time())) {
      auto ttl = ValidatorManager::persistent_state_ttl(h->unix_time());
      double time_to_download = 3600 * 8;
      if (ttl > td::Clocks::system() + time_to_download) {
        handle = h;
        break;
      } else {
        LOG(INFO) << "ignoring: state is expiring shortly: expire_at=" << ttl;
      }
    } else {
      LOG(INFO) << "ignoring: state is not persistent";
    }
  }

  block_id_ = handle->id();
  handle_ = handle;
  LOG(WARNING) << "best handle is " << handle_->id();

  download_masterchain_state();
}

void ValidatorManagerMasterchainReiniter::download_masterchain_state() {
  status_.set_status(PSTRING() << "downloading masterchain state " << block_id_.seqno());
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<ShardState>> R) {
    if (R.is_error()) {
      LOG(WARNING) << "failed to download masterchain state: " << R.move_as_error();
      delay_action(
          [=]() { td::actor::send_closure(SelfId, &ValidatorManagerMasterchainReiniter::download_masterchain_state); },
          td::Timestamp::in(1.0));
    } else {
      td::actor::send_closure(SelfId, &ValidatorManagerMasterchainReiniter::downloaded_masterchain_state,
                              R.move_as_ok());
    }
  });
  td::actor::create_actor<DownloadShardState>("downloadstate", block_id_, block_id_, 0, 2, manager_,
                                              td::Timestamp::in(3600 * 3), std::move(P))
      .release();
}

void ValidatorManagerMasterchainReiniter::downloaded_masterchain_state(td::Ref<ShardState> state) {
  state_ = td::Ref<MasterchainState>{std::move(state)};
  CHECK(handle_->received_state());
  CHECK(handle_->is_applied());
  LOG(INFO) << "downloaded masterchain state";
  td::actor::send_closure(manager_, &ValidatorManager::init_last_masterchain_state, state_);
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &ValidatorManagerMasterchainReiniter::downloaded_all_shards);
  });
  client_ = td::actor::create_actor<ShardClient>("shardclient", opts_, handle_, state_, manager_, std::move(P));
  status_.set_status(PSTRING() << "downloading all shard states, mc seqno " << block_id_.seqno());
}

void ValidatorManagerMasterchainReiniter::downloaded_all_shards() {
  LOG(INFO) << "downloaded all shards";
  td::actor::send_closure(manager_, &ValidatorManager::update_gc_block_handle, handle_,
                          [SelfId = actor_id(this)](td::Result<td::Unit> R) {
                            R.ensure();
                            td::actor::send_closure(SelfId, &ValidatorManagerMasterchainReiniter::finish);
                          });
}

void ValidatorManagerMasterchainReiniter::finish() {
  CHECK(handle_->id().id.seqno == 0 || handle_->is_key_block());
  promise_.set_value(ValidatorManagerInitResult{handle_, state_, std::move(client_), handle_, state_, handle_});
  LOG(INFO) << "persistent state download finished";
  stop();
}

void ValidatorManagerMasterchainStarter::start_up() {
  run().start().detach();
}

td::actor::Task<> ValidatorManagerMasterchainStarter::run() {
  auto R = co_await run_inner().wrap();
  R.ensure();
  promise_.set_value(R.move_as_ok());
  stop();
  co_return {};
}

td::actor::Task<ValidatorManagerInitResult> ValidatorManagerMasterchainStarter::run_inner() {
  LOG(INFO) << "Starting validator manager";
  auto r_init = co_await td::actor::ask(db_, &Db::get_init_masterchain_block).wrap();
  if (r_init.is_error()) {
    LOG_CHECK(r_init.error().code() == ErrorCode::notready) << r_init.move_as_error();
    auto [task, promise] = td::actor::StartedTask<ValidatorManagerInitResult>::make_bridge();
    td::actor::create_actor<ValidatorManagerMasterchainReiniter>("reiniter", opts_, manager_, db_, std::move(promise))
        .release();
    co_return co_await std::move(task);
  }

  BlockIdExt init_block_id = r_init.move_as_ok();
  LOG(INFO) << "init_block_id = " << init_block_id.to_str();
  LOG(INFO) << "config init_block_id = " << opts_->init_block_id().to_str();
  handle_ = co_await td::actor::ask(manager_, &ValidatorManager::get_block_handle, init_block_id, true);

  while (true) {
    if (!handle_->received_state()) {
      LOG(ERROR) << "db inconsistent: last state " << handle_->id().to_str() << " not received";
      auto result = co_await td::actor::ask(manager_, &ValidatorManager::wait_block_state, handle_, 1,
                                            td::Timestamp::in(600.0), true)
                        .wrap();
      if (result.is_error()) {
        LOG(ERROR) << "wait state error: " << result.move_as_error();
        continue;
      }
    }
    if (!handle_->is_applied()) {
      LOG_CHECK(handle_->inited_prev()) << "block_id=" << handle_->id().to_str();
      LOG(WARNING) << "init_block not applied, trying previous block #" << handle_->id().seqno() - 1;
      handle_ = co_await td::actor::ask(manager_, &ValidatorManager::get_block_handle, handle_->one_prev(true), false);
      continue;
    }
    break;
  }
  LOG_CHECK(handle_->received_state()) << "block_id=" << handle_->id().to_str();

  state_ =
      td::Ref<MasterchainState>{co_await td::actor::ask(manager_, &ValidatorManager::get_shard_state_from_db, handle_)};
  LOG_CHECK(state_->get_block_id() == opts_->init_block_id() || state_->ancestor_is_valid(opts_->init_block_id()) ||
            state_->get_block_id().seqno() < opts_->get_last_fork_masterchain_seqno())
      << "block_id=" << state_->get_block_id().to_str() << " init_block_id=" << opts_->init_block_id().to_str()
      << " last_hardfork_seqno=" << opts_->get_last_fork_masterchain_seqno();

  auto r_gc_block_id = co_await td::actor::ask(db_, &Db::get_gc_masterchain_block).wrap();
  BlockIdExt gc_block_id;
  if (r_gc_block_id.is_error()) {
    LOG_CHECK(r_gc_block_id.error().code() == ErrorCode::notready) << r_gc_block_id.move_as_error();
    gc_block_id = opts_->init_block_id();
  } else {
    gc_block_id = r_gc_block_id.move_as_ok();
  }
  LOG(INFO) << "gc_block_id = " << gc_block_id.to_str();
  auto gc_handle = co_await td::actor::ask(manager_, &ValidatorManager::get_block_handle, gc_block_id, true);
  LOG_CHECK(gc_handle->id().id.seqno <= handle_->id().id.seqno)
      << "gc_block_id=" << gc_handle->id().to_str() << " block_id=" << handle_->id().to_str();
  LOG_CHECK(gc_handle->received_state()) << "gc_block_id=" << gc_handle->id().to_str();
  LOG_CHECK(!gc_handle->deleted_state_boc()) << "gc_block_id=" << gc_handle->id().to_str();
  auto gc_state = td::Ref<MasterchainState>{
      co_await td::actor::ask(manager_, &ValidatorManager::get_shard_state_from_db, gc_handle)};

  co_await td::actor::ask(manager_, &ValidatorManager::get_shard_client_state, true);
  auto stored_hardforks = co_await td::actor::ask(db_, &Db::get_hardforks);

  auto hardforks = opts_->get_hardforks();
  CHECK(hardforks.size() >= stored_hardforks.size());
  CHECK(hardforks.size() <= stored_hardforks.size() + 1);

  if (hardforks.size() == stored_hardforks.size() + 1) {
    std::vector<BlockIdExt> prev_hardforks = hardforks;
    prev_hardforks.pop_back();
    CHECK(prev_hardforks == stored_hardforks);
    co_await get_latest_applied_block();
    LOG(INFO) << "latest applied block = " << handle_->id().to_str();
    auto new_hardfork = hardforks.back();
    LOG(WARNING) << "New hardfork is " << new_hardfork.to_str();
    LOG_CHECK(handle_->id().seqno() + 1 >= new_hardfork.seqno())
        << "Last masterchain block seqno is " << handle_->id().seqno() << ", but the new hardfork has seqno "
        << new_hardfork.seqno();
    LOG_CHECK(new_hardfork.seqno() > gc_handle->id().seqno())
        << "cannot start: new hardfork " << new_hardfork.seqno() << "is on too old block, gc seqno is "
        << gc_handle->id().seqno();

    BlockSeqno truncate_seqno = new_hardfork.seqno() - 1;
    co_await truncate(truncate_seqno);
    handle_->set_next(new_hardfork);
    co_await handle_->flush(manager_, handle_);
    co_await td::actor::ask(db_, &Db::update_hardforks, hardforks);
  } else {
    CHECK(hardforks == stored_hardforks);
    if (opts_->need_db_truncate()) {
      auto truncate_seqno = opts_->get_truncate_seqno();
      LOG(WARNING) << "Requested truncate to " << truncate_seqno;
      co_await get_latest_applied_block();
      LOG(INFO) << "latest applied block = " << handle_->id().to_str();
      if (truncate_seqno <= handle_->id().seqno()) {
        co_await truncate(truncate_seqno);
      }
    }
  }

  LOG(INFO) << "Starting shard client";
  auto [task, promise] = td::actor::StartedTask<>::make_bridge();
  auto shard_client = td::actor::create_actor<ShardClient>("shardclient", opts_, manager_, std::move(promise));
  co_await std::move(task);
  LOG(INFO) << "Started shard client";
  BlockHandle last_key_block_handle;
  if (handle_->id().id.seqno == 0 || handle_->is_key_block()) {
    last_key_block_handle = handle_;
  } else {
    auto last_key_block_id = state_->last_key_block_id();
    CHECK(last_key_block_id.is_valid());
    last_key_block_handle =
        co_await td::actor::ask(manager_, &ValidatorManager::get_block_handle, last_key_block_id, true);
  }
  LOG(INFO) << "Last key block is " << last_key_block_handle->id().to_str();
  co_return ValidatorManagerInitResult{handle_,   state_,   std::move(shard_client),
                                       gc_handle, gc_state, last_key_block_handle};
}

td::actor::Task<> ValidatorManagerMasterchainStarter::get_latest_applied_block() {
  while (handle_->inited_next()) {
    BlockHandle next_handle =
        co_await td::actor::ask(manager_, &ValidatorManager::get_block_handle, handle_->one_next(true), true);
    if (next_handle->is_applied() && next_handle->received_state()) {
      handle_ = next_handle;
    } else {
      break;
    }
  }
  if (handle_->id() != state_->get_block_id()) {
    state_ = td::Ref<MasterchainState>{
        co_await td::actor::ask(manager_, &ValidatorManager::get_shard_state_from_db, handle_)};
  }
  co_return {};
}

td::actor::Task<> ValidatorManagerMasterchainStarter::truncate(BlockSeqno truncate_seqno) {
  LOG_CHECK(truncate_seqno <= handle_->id().seqno())
      << "block_id=" << handle_->id().to_str() << " truncate_seqno=" << truncate_seqno;
  if (truncate_seqno < handle_->id().seqno()) {
    LOG(WARNING) << "Truncating to seqno " << truncate_seqno;
    BlockIdExt block_id;
    CHECK(state_->get_old_mc_block_id(truncate_seqno, block_id));
    handle_ = co_await td::actor::ask(manager_, &ValidatorManager::get_block_handle, block_id, false);
    state_ = td::Ref<MasterchainState>{co_await td::actor::ask(db_, &Db::get_block_state, handle_)};
    co_await td::actor::ask(manager_, &ValidatorManager::truncate, block_id.seqno(), handle_);
  }
  LOG(WARNING) << "Clearing 'next' for mc seqno " << state_->get_block_id().to_str();
  auto s = state_->get_shards();
  for (auto &shard : s) {
    if (opts_->need_monitor(shard->shard(), state_)) {
      auto handle = co_await td::actor::ask(manager_, &ValidatorManager::get_block_handle, shard->top_block_id(), true);
      handle->unsafe_clear_next();
      co_await handle->flush(manager_, handle);
    }
  }
  handle_->unsafe_clear_next();
  co_await handle_->flush(manager_, handle_);
  co_return {};
}

void validator_manager_init(td::Ref<ValidatorManagerOptions> opts, td::actor::ActorId<ValidatorManager> manager,
                            td::actor::ActorId<Db> db, td::Promise<ValidatorManagerInitResult> promise) {
  CHECK(!opts.is_null());
  td::actor::create_actor<ValidatorManagerMasterchainStarter>("starter", std::move(opts), manager, db,
                                                              std::move(promise))
      .release();
}

}  // namespace validator

}  // namespace ton
