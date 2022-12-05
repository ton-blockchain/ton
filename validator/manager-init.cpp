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
#include "manager-init.hpp"
#include "td/utils/filesystem.h"
#include "fabric.h"
#include "downloaders/wait-block-state.hpp"
#include "ton/ton-io.hpp"
#include "common/checksum.h"
#include "adnl/utils.hpp"
#include "validator/downloaders/download-state.hpp"
#include "common/delay.h"
#include "td/actor/MultiPromise.h"

namespace ton {

namespace validator {

void ValidatorManagerMasterchainReiniter::start_up() {
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

  if (opts_->initial_sync_disabled()) {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<ShardState>> R) {
      R.ensure();
      td::actor::send_closure(SelfId, &ValidatorManagerMasterchainReiniter::download_masterchain_state);
    });
    td::actor::create_actor<DownloadShardState>("downloadstate", handle_->id(), BlockIdExt{}, 2, manager_,
                                                td::Timestamp::in(3600), std::move(P))
        .release();
    return;
  }

  download_proof_link();
}

void ValidatorManagerMasterchainReiniter::download_proof_link() {
  if (handle_->id().id.seqno == 0) {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<ShardState>> R) {
      R.ensure();
      td::actor::send_closure(SelfId, &ValidatorManagerMasterchainReiniter::downloaded_zero_state);
    });
    td::actor::create_actor<DownloadShardState>("downloadstate", handle_->id(), BlockIdExt{}, 2, manager_,
                                                td::Timestamp::in(3600), std::move(P))
        .release();
  } else {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
      if (R.is_error()) {
        LOG(WARNING) << "failed to download proof link: " << R.move_as_error();
        delay_action(
            [SelfId]() { td::actor::send_closure(SelfId, &ValidatorManagerMasterchainReiniter::download_proof_link); },
            td::Timestamp::in(1.0));
      } else {
        td::actor::send_closure(SelfId, &ValidatorManagerMasterchainReiniter::downloaded_proof_link, R.move_as_ok());
      }
    });
    td::actor::send_closure(manager_, &ValidatorManager::send_get_block_proof_link_request, handle_->id(), 2,
                            std::move(P));
  }
}

void ValidatorManagerMasterchainReiniter::downloaded_proof_link(td::BufferSlice proof) {
  auto pp = create_proof_link(handle_->id(), std::move(proof));
  if (pp.is_error()) {
    LOG(WARNING) << "bad proof link: " << pp.move_as_error();
    download_proof_link();
    return;
  }

  auto proof_link = pp.move_as_ok();

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), db = db_, proof_link](td::Result<BlockHandle> R) {
    if (R.is_error()) {
      LOG(WARNING) << "downloaded proof link failed: " << R.move_as_error();
      td::actor::send_closure(SelfId, &ValidatorManagerMasterchainReiniter::download_proof_link);
    } else {
      auto P = td::PromiseCreator::lambda([SelfId, handle = R.move_as_ok()](td::Result<td::Unit> R) {
        R.ensure();
        td::actor::send_closure(SelfId, &ValidatorManagerMasterchainReiniter::try_download_key_blocks, false);
      });
      td::actor::send_closure(db, &Db::add_key_block_proof_link, proof_link, std::move(P));
    }
  });

  run_check_proof_link_query(handle_->id(), proof_link, manager_, td::Timestamp::in(60.0), std::move(P));
}

void ValidatorManagerMasterchainReiniter::downloaded_zero_state() {
  try_download_key_blocks(false);
}

void ValidatorManagerMasterchainReiniter::try_download_key_blocks(bool try_start) {
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
  LOG(WARNING) << "last key block is " << vec[vec.size() - 1];
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
      double time_to_download = 3600 * 3;
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
  td::actor::create_actor<DownloadShardState>("downloadstate", block_id_, block_id_, 2, manager_,
                                              td::Timestamp::in(3600 * 3), std::move(P))
      .release();
}

void ValidatorManagerMasterchainReiniter::downloaded_masterchain_state(td::Ref<ShardState> state) {
  state_ = td::Ref<MasterchainState>{std::move(state)};
  CHECK(handle_->received_state());
  CHECK(handle_->is_applied());
  LOG(INFO) << "downloaded masterchain state";
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &ValidatorManagerMasterchainReiniter::downloaded_all_shards);
  });
  client_ = td::actor::create_actor<ShardClient>("shardclient", opts_, handle_, state_, manager_, std::move(P));
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
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<BlockIdExt> R) {
    if (R.is_error()) {
      CHECK(R.error().code() == ErrorCode::notready);
      td::actor::send_closure(SelfId, &ValidatorManagerMasterchainStarter::failed_to_get_init_block_id);
    } else {
      td::actor::send_closure(SelfId, &ValidatorManagerMasterchainStarter::got_init_block_id, R.move_as_ok());
    }
  });
  td::actor::send_closure(db_, &Db::get_init_masterchain_block, std::move(P));
}

void ValidatorManagerMasterchainStarter::failed_to_get_init_block_id() {
  td::actor::create_actor<ValidatorManagerMasterchainReiniter>("reiniter", opts_, manager_, db_, std::move(promise_))
      .release();
  stop();
}

void ValidatorManagerMasterchainStarter::got_init_block_id(BlockIdExt block_id) {
  block_id_ = block_id;

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<BlockHandle> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &ValidatorManagerMasterchainStarter::got_init_block_handle, R.move_as_ok());
  });

  td::actor::send_closure(manager_, &ValidatorManager::get_block_handle, block_id_, true, std::move(P));
}

void ValidatorManagerMasterchainStarter::got_init_block_handle(BlockHandle handle) {
  handle_ = std::move(handle);
  if (!handle_->received_state()) {
    LOG(ERROR) << "db inconsistent: last state ( " << handle_->id() << " ) not received";
    td::actor::send_closure(manager_, &ValidatorManager::wait_block_state, handle_, 1, td::Timestamp::in(600.0),
                            [SelfId = actor_id(this), handle = handle_](td::Result<td::Ref<ShardState>> R) {
                              td::actor::send_closure(
                                  SelfId, &ValidatorManagerMasterchainStarter::got_init_block_handle, handle);
                            });
    return;
  }
  if (!handle_->is_applied()) {
    CHECK(handle_->inited_prev());
    td::actor::send_closure(manager_, &ValidatorManager::get_block_handle, handle_->one_prev(true), false,
                            [SelfId = actor_id(this)](td::Result<BlockHandle> R) {
                              R.ensure();
                              td::actor::send_closure(
                                  SelfId, &ValidatorManagerMasterchainStarter::got_init_block_handle, R.move_as_ok());
                            });
    return;
  }
  LOG_CHECK(handle_->received_state()) << "block_id=" << handle_->id();

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<ShardState>> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &ValidatorManagerMasterchainStarter::got_init_block_state,
                            td::Ref<MasterchainState>{R.move_as_ok()});
  });

  td::actor::send_closure(manager_, &ValidatorManager::get_shard_state_from_db, handle_, std::move(P));
}

void ValidatorManagerMasterchainStarter::got_init_block_state(td::Ref<MasterchainState> state) {
  state_ = std::move(state);
  CHECK(state_->get_block_id() == opts_->init_block_id() || state_->ancestor_is_valid(opts_->init_block_id()) ||
        state_->get_block_id().seqno() < opts_->get_last_fork_masterchain_seqno());
  //finish();

  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), block_id = opts_->init_block_id()](td::Result<BlockIdExt> R) {
        if (R.is_error()) {
          LOG_CHECK(R.error().code() == ErrorCode::notready) << R.move_as_error();
          td::actor::send_closure(SelfId, &ValidatorManagerMasterchainStarter::got_gc_block_id, block_id);
        } else {
          td::actor::send_closure(SelfId, &ValidatorManagerMasterchainStarter::got_gc_block_id, R.move_as_ok());
        }
      });

  td::actor::send_closure(db_, &Db::get_gc_masterchain_block, std::move(P));
}

void ValidatorManagerMasterchainStarter::got_gc_block_id(BlockIdExt block_id) {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<BlockHandle> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &ValidatorManagerMasterchainStarter::got_gc_block_handle, R.move_as_ok());
  });

  td::actor::send_closure(manager_, &ValidatorManager::get_block_handle, block_id, true, std::move(P));
}

void ValidatorManagerMasterchainStarter::got_gc_block_handle(BlockHandle handle) {
  gc_handle_ = std::move(handle);

  CHECK(gc_handle_->id().id.seqno <= handle_->id().id.seqno);
  LOG_CHECK(gc_handle_->received_state()) << "block_id=" << gc_handle_->id();
  LOG_CHECK(!gc_handle_->deleted_state_boc()) << "block_id=" << gc_handle_->id();

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<ShardState>> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &ValidatorManagerMasterchainStarter::got_gc_block_state,
                            td::Ref<MasterchainState>{R.move_as_ok()});
  });

  td::actor::send_closure(manager_, &ValidatorManager::get_shard_state_from_db, gc_handle_, std::move(P));
}

void ValidatorManagerMasterchainStarter::got_gc_block_state(td::Ref<MasterchainState> state) {
  gc_state_ = std::move(state);

  if (handle_->id().id.seqno == 0 || handle_->is_key_block()) {
    last_key_block_handle_ = handle_;

    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<BlockIdExt> R) {
      R.ensure();
      td::actor::send_closure(SelfId, &ValidatorManagerMasterchainStarter::got_shard_block_id, R.move_as_ok());
    });

    td::actor::send_closure(manager_, &ValidatorManager::get_shard_client_state, true, std::move(P));
    return;
  }

  auto block_id = state_->last_key_block_id();
  CHECK(block_id.is_valid());

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<BlockHandle> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &ValidatorManagerMasterchainStarter::got_key_block_handle, R.move_as_ok());
  });
  td::actor::send_closure(manager_, &ValidatorManager::get_block_handle, block_id, true, std::move(P));
}

void ValidatorManagerMasterchainStarter::got_key_block_handle(BlockHandle handle) {
  CHECK(handle);
  //CHECK(handle->id().id.seqno == 0 || handle->is_key_block());
  last_key_block_handle_ = std::move(handle);

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<BlockIdExt> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &ValidatorManagerMasterchainStarter::got_shard_block_id, R.move_as_ok());
  });

  td::actor::send_closure(manager_, &ValidatorManager::get_shard_client_state, true, std::move(P));
}

void ValidatorManagerMasterchainStarter::got_shard_block_id(BlockIdExt block_id) {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<std::vector<BlockIdExt>> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &ValidatorManagerMasterchainStarter::got_hardforks, R.move_as_ok());
  });
  td::actor::send_closure(db_, &Db::get_hardforks, std::move(P));
}

void ValidatorManagerMasterchainStarter::got_hardforks(std::vector<BlockIdExt> vec) {
  auto h = opts_->get_hardforks();
  if (h.size() < vec.size()) {
    LOG(FATAL) << "cannot start: number of hardforks decreased";
    return;
  }
  if (h.size() == vec.size()) {
    if (h.size() > 0) {
      if (*h.rbegin() != *vec.rbegin()) {
        LOG(FATAL) << "cannot start: hardforks list changed";
        return;
      }
    }
    if (opts_->need_db_truncate()) {
      auto seq = opts_->get_truncate_seqno();
      if (seq <= handle_->id().seqno()) {
        got_truncate_block_seqno(seq);
        return;
      }
    }
    start_shard_client();
    return;
  }
  if (h.size() > vec.size() + 1) {
    LOG(FATAL) << "cannot start: number of hardforks increase is too big";
    return;
  }
  has_new_hardforks_ = true;

  auto b = *h.rbegin();
  if (handle_->id().seqno() + 1 < b.seqno()) {
    truncated();
    return;
  }
  if (b.seqno() <= gc_handle_->id().seqno()) {
    LOG(FATAL) << "cannot start: new hardfork is on too old block (already gc'd)";
    return;
  }

  got_truncate_block_seqno(b.seqno() - 1);
}

void ValidatorManagerMasterchainStarter::got_truncate_block_seqno(BlockSeqno seqno) {
  BlockIdExt id;
  if (handle_->id().seqno() == seqno) {
    got_truncate_block_handle(handle_);
    return;
  }
  if (state_->get_old_mc_block_id(seqno, id)) {
    got_truncate_block_id(id);
    return;
  }

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), db = db_](td::Result<ConstBlockHandle> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &ValidatorManagerMasterchainStarter::got_truncate_block_id, R.move_as_ok()->id());
  });
  td::actor::send_closure(db_, &Db::get_block_by_seqno, AccountIdPrefixFull{masterchainId, 0}, seqno, std::move(P));
}

void ValidatorManagerMasterchainStarter::got_truncate_block_id(BlockIdExt block_id) {
  block_id_ = block_id;

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<BlockHandle> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &ValidatorManagerMasterchainStarter::got_truncate_block_handle, R.move_as_ok());
  });
  td::actor::send_closure(manager_, &ValidatorManager::get_block_handle, block_id_, false, std::move(P));
}

void ValidatorManagerMasterchainStarter::got_truncate_block_handle(BlockHandle handle) {
  handle_ = std::move(handle);
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<ShardState>> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &ValidatorManagerMasterchainStarter::got_truncate_state,
                            td::Ref<MasterchainState>{R.move_as_ok()});
  });
  td::actor::send_closure(db_, &Db::get_block_state, handle_, std::move(P));
}

void ValidatorManagerMasterchainStarter::got_truncate_state(td::Ref<MasterchainState> state) {
  state_ = std::move(state);

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<BlockHandle> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &ValidatorManagerMasterchainStarter::got_prev_key_block_handle, R.move_as_ok());
  });
  td::actor::send_closure(manager_, &ValidatorManager::get_block_handle, state_->last_key_block_id(), false,
                          std::move(P));
}

void ValidatorManagerMasterchainStarter::got_prev_key_block_handle(BlockHandle handle) {
  last_key_block_handle_ = std::move(handle);
  //LOG_CHECK(last_key_block_handle_->inited_is_key_block() && last_key_block_handle_->is_key_block())
  //    << last_key_block_handle_->id();
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &ValidatorManagerMasterchainStarter::truncated);
  });
  td::actor::send_closure(manager_, &ValidatorManager::truncate, block_id_.seqno(), handle_, std::move(P));
}

void ValidatorManagerMasterchainStarter::truncate_shard_next(BlockIdExt block_id, td::Promise<td::Unit> promise) {
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), manager = manager_, promise = std::move(promise)](td::Result<BlockHandle> R) mutable {
        R.ensure();
        auto handle = R.move_as_ok();
        handle->unsafe_clear_next();
        handle->flush(manager, handle, std::move(promise));
      });
  td::actor::send_closure(manager_, &ValidatorManager::get_block_handle, block_id, true, std::move(P));
}

void ValidatorManagerMasterchainStarter::truncated() {
  td::MultiPromise mp;
  auto ig = mp.init_guard();

  ig.add_promise([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &ValidatorManagerMasterchainStarter::truncated_next);
  });

  truncate_shard_next(handle_->id(), ig.get_promise());
  auto s = state_->get_shards();
  for (auto &shard : s) {
    if (opts_->need_monitor(shard->shard())) {
      truncate_shard_next(shard->top_block_id(), ig.get_promise());
    }
  }
}

void ValidatorManagerMasterchainStarter::truncated_next() {
  if (has_new_hardforks_) {
    handle_->set_next(*opts_->get_hardforks().rbegin());
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
      R.ensure();
      td::actor::send_closure(SelfId, &ValidatorManagerMasterchainStarter::written_next);
    });
    handle_->flush(manager_, handle_, std::move(P));
  } else {
    start_shard_client();
  }
}

void ValidatorManagerMasterchainStarter::written_next() {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &ValidatorManagerMasterchainStarter::start_shard_client);
  });
  td::actor::send_closure(db_, &Db::update_hardforks, opts_->get_hardforks(), std::move(P));
}

void ValidatorManagerMasterchainStarter::start_shard_client() {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &ValidatorManagerMasterchainStarter::finish);
  });
  client_ = td::actor::create_actor<ShardClient>("shardclient", opts_, manager_, std::move(P));
}

void ValidatorManagerMasterchainStarter::finish() {
  promise_.set_value(
      ValidatorManagerInitResult{handle_, state_, std::move(client_), gc_handle_, gc_state_, last_key_block_handle_});
  stop();
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
