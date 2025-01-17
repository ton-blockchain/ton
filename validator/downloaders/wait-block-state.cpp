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
#include "wait-block-state.hpp"
#include "validator/fabric.h"
#include "ton/ton-io.hpp"
#include "common/checksum.h"
#include "common/delay.h"
#include "validator/downloaders/download-state.hpp"

namespace ton {

namespace validator {

void WaitBlockState::alarm() {
  abort_query(td::Status::Error(ErrorCode::timeout, "timeout"));
}

void WaitBlockState::abort_query(td::Status reason) {
  if (promise_) {
    if (priority_ > 0 || (reason.code() != ErrorCode::timeout && reason.code() != ErrorCode::notready)) {
      LOG(WARNING) << "aborting wait block state query for " << handle_->id() << " priority=" << priority_ << ": "
                   << reason;
    } else {
      LOG(DEBUG) << "aborting wait block state query for " << handle_->id() << " priority=" << priority_ << ": "
                 << reason;
    }
    promise_.set_error(reason.move_as_error_prefix(PSTRING() << "failed to download state " << handle_->id() << ": "));
  }
  stop();
}

void WaitBlockState::finish_query() {
  CHECK(handle_->received_state());
  /*if (handle_->id().is_masterchain() && handle_->inited_proof()) {
    td::actor::send_closure(manager_, &ValidatorManager::new_block, handle_, prev_state_, [](td::Unit) {});
  }*/
  if (promise_) {
    promise_.set_result(prev_state_);
  }
  stop();
}

void WaitBlockState::start_up() {
  alarm_timestamp() = timeout_;

  CHECK(handle_);
  start();
}

void WaitBlockState::start() {
  if (reading_from_db_) {
    return;
  }
  bool inited_proof = handle_->id().is_masterchain() ? handle_->inited_proof() : handle_->inited_proof_link();
  if (handle_->received_state() && inited_proof) {
    reading_from_db_ = true;

    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<ShardState>> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &WaitBlockState::abort_query, R.move_as_error_prefix("db error: "));
      } else {
        td::actor::send_closure(SelfId, &WaitBlockState::got_state_from_db, R.move_as_ok());
      }
    });
    td::actor::send_closure(manager_, &ValidatorManager::get_shard_state_from_db, handle_, std::move(P));
  } else if (handle_->id().id.seqno == 0 && next_static_file_attempt_.is_in_past()) {
    next_static_file_attempt_ = td::Timestamp::in(60.0);
    // id.file_hash contrains correct file hash of zero state
    // => if file with this sha256 is found it is garanteed to be correct
    // => if it is not, this error is permanent
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), id = handle_->id()](td::Result<td::BufferSlice> R) {
      if (R.is_error()) {
        if (R.error().code() == ErrorCode::notready) {
          td::actor::send_closure(SelfId, &WaitBlockState::start);
        } else {
          td::actor::send_closure(SelfId, &WaitBlockState::abort_query, R.move_as_error_prefix("static db error: "));
        }
      } else {
        auto data = R.move_as_ok();
        td::actor::send_closure(SelfId, &WaitBlockState::got_state_from_net, std::move(data));
      }
    });
    td::actor::send_closure(manager_, &ValidatorManager::try_get_static_file, handle_->id().file_hash, std::move(P));
  } else if (handle_->id().id.seqno == 0) {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &WaitBlockState::failed_to_get_state_from_net,
                                R.move_as_error_prefix("net error: "));
      } else {
        td::actor::send_closure(SelfId, &WaitBlockState::got_state_from_net, R.move_as_ok());
      }
    });
    td::actor::send_closure(manager_, &ValidatorManager::send_get_zero_state_request, handle_->id(), priority_,
                            std::move(P));
  } else if (check_persistent_state_desc() && !handle_->received_state()) {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<ShardState>> R) {
      if (R.is_error()) {
        LOG(WARNING) << "failed to get persistent state: " << R.move_as_error();
        td::actor::send_closure(SelfId, &WaitBlockState::start);
      } else {
        td::actor::send_closure(SelfId, &WaitBlockState::written_state, R.move_as_ok());
      }
    });
    BlockIdExt masterchain_id = persistent_state_desc_->masterchain_id;
    td::actor::create_actor<DownloadShardState>("downloadstate", handle_->id(), masterchain_id, priority_, manager_,
                                                timeout_, std::move(P))
        .release();
  } else if (!handle_->inited_prev() || (!handle_->inited_proof() && !handle_->inited_proof_link())) {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), handle = handle_](td::Result<td::BufferSlice> R) {
      if (R.is_error()) {
        delay_action([SelfId]() { td::actor::send_closure(SelfId, &WaitBlockState::after_get_proof_link); },
                     td::Timestamp::in(0.1));
      } else {
        td::actor::send_closure(SelfId, &WaitBlockState::got_proof_link, R.move_as_ok());
      }
    });

    waiting_proof_link_ = true;
    td::actor::send_closure(manager_, &ValidatorManager::send_get_block_proof_link_request, handle_->id(), priority_,
                            std::move(P));
  } else if (prev_state_.is_null()) {
    CHECK(handle_->inited_proof() || handle_->inited_proof_link());
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<ShardState>> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &WaitBlockState::failed_to_get_prev_state,
                                R.move_as_error_prefix("prev state wait error: "));
      } else {
        td::actor::send_closure(SelfId, &WaitBlockState::got_prev_state, R.move_as_ok());
      }
    });

    td::actor::send_closure(manager_, &ValidatorManager::wait_prev_block_state, handle_, priority_, timeout_,
                            std::move(P));
  } else if (handle_->id().is_masterchain() && !handle_->inited_proof()) {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), handle = handle_](td::Result<td::BufferSlice> R) {
      if (R.is_error()) {
        delay_action([SelfId]() { td::actor::send_closure(SelfId, &WaitBlockState::after_get_proof); },
                     td::Timestamp::in(0.1));
      } else {
        td::actor::send_closure(SelfId, &WaitBlockState::got_proof, R.move_as_ok());
      }
    });

    waiting_proof_ = true;
    td::actor::send_closure(manager_, &ValidatorManager::send_get_block_proof_request, handle_->id(), priority_,
                            std::move(P));
  } else if (block_.is_null()) {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<BlockData>> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &WaitBlockState::failed_to_get_block_data,
                                R.move_as_error_prefix("block wait error: "));
      } else {
        td::actor::send_closure(SelfId, &WaitBlockState::got_block_data, R.move_as_ok());
      }
    });

    td::actor::send_closure(manager_, &ValidatorManager::wait_block_data, handle_, priority_, timeout_, std::move(P));
  } else {
    apply();
  }
}

void WaitBlockState::failed_to_get_prev_state(td::Status reason) {
  if (reason.code() == ErrorCode::notready) {
    start();
  } else {
    abort_query(std::move(reason));
  }
}

void WaitBlockState::got_prev_state(td::Ref<ShardState> state) {
  prev_state_ = std::move(state);

  start();
}

void WaitBlockState::got_proof_link(td::BufferSlice data) {
  if (!waiting_proof_link_) {
    return;
  }
  auto R = create_proof_link(handle_->id(), std::move(data));
  if (R.is_error()) {
    LOG(INFO) << "received bad proof link: " << R.move_as_error();
    start();
    return;
  }
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<BlockHandle> R) {
    if (R.is_ok()) {
      auto h = R.move_as_ok();
      CHECK(h->inited_prev());
      td::actor::send_closure(SelfId, &WaitBlockState::after_get_proof_link);
    } else {
      LOG(INFO) << "received bad proof link: " << R.move_as_error();
      delay_action([SelfId]() { td::actor::send_closure(SelfId, &WaitBlockState::after_get_proof_link); },
                   td::Timestamp::in(0.1));
    }
  });
  run_check_proof_link_query(handle_->id(), R.move_as_ok(), manager_, timeout_, std::move(P));
}

void WaitBlockState::got_proof(td::BufferSlice data) {
  if (!waiting_proof_) {
    return;
  }
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    if (R.is_ok()) {
      td::actor::send_closure(SelfId, &WaitBlockState::after_get_proof);
    } else {
      LOG(INFO) << "received bad proof link: " << R.move_as_error();
      td::actor::send_closure(SelfId, &WaitBlockState::after_get_proof);
    }
  });
  td::actor::send_closure(manager_, &ValidatorManager::validate_block_proof, handle_->id(), std::move(data),
                          std::move(P));
}

void WaitBlockState::failed_to_get_block_data(td::Status reason) {
  if (reason.code() == ErrorCode::notready) {
    start();
  } else {
    abort_query(std::move(reason));
  }
}

void WaitBlockState::got_block_data(td::Ref<BlockData> data) {
  block_ = std::move(data);

  start();
}

void WaitBlockState::apply() {
  TD_PERF_COUNTER(apply_block_to_state);
  td::PerfWarningTimer t{"applyblocktostate", 0.1};
  auto S = prev_state_.write().apply_block(handle_->id(), block_);
  if (S.is_error()) {
    abort_query(S.move_as_error_prefix("apply error: "));
    return;
  }

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<ShardState>> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &WaitBlockState::abort_query, R.move_as_error_prefix("db set error: "));
    } else {
      td::actor::send_closure(SelfId, &WaitBlockState::written_state, R.move_as_ok());
    }
  });

  td::actor::send_closure(manager_, &ValidatorManager::set_block_state, handle_, prev_state_, std::move(P));
}

void WaitBlockState::written_state(td::Ref<ShardState> upd_state) {
  prev_state_ = std::move(upd_state);
  finish_query();
}

void WaitBlockState::got_state_from_db(td::Ref<ShardState> state) {
  prev_state_ = state;
  if (!handle_->received_state()) {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<ShardState>> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &WaitBlockState::abort_query, R.move_as_error_prefix("db set error: "));
      } else {
        td::actor::send_closure(SelfId, &WaitBlockState::written_state, R.move_as_ok());
      }
    });

    td::actor::send_closure(manager_, &ValidatorManager::set_block_state, handle_, prev_state_, std::move(P));
  } else {
    finish_query();
  }
}

void WaitBlockState::got_state_from_static_file(td::Ref<ShardState> state, td::BufferSlice data) {
  auto P =
      td::PromiseCreator::lambda([SelfId = actor_id(this), state = std::move(state)](td::Result<td::Unit> R) mutable {
        R.ensure();
        td::actor::send_closure(SelfId, &WaitBlockState::got_state_from_db, std::move(state));
      });
  td::actor::send_closure(manager_, &ValidatorManager::store_zero_state_file, handle_->id(), std::move(data),
                          std::move(P));
}

void WaitBlockState::force_read_from_db() {
  if (!handle_ || reading_from_db_) {
    return;
  }

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<ShardState>> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &WaitBlockState::abort_query, R.move_as_error_prefix("db get error: "));
    } else {
      td::actor::send_closure(SelfId, &WaitBlockState::got_state_from_db, R.move_as_ok());
    }
  });
  td::actor::send_closure(manager_, &ValidatorManager::get_shard_state_from_db, handle_, std::move(P));
}

void WaitBlockState::got_state_from_net(td::BufferSlice data) {
  auto R = create_shard_state(handle_->id(), data.clone());
  if (R.is_error()) {
    LOG(WARNING) << "received bad state from net: " << R.move_as_error();
    start();
    return;
  }
  auto state = R.move_as_ok();

  if (handle_->id().id.seqno == 0) {
    handle_->set_state_root_hash(handle_->id().root_hash);
  }
  if (state->root_hash() != handle_->state()) {
    LOG(WARNING) << "received state have bad root hash";
    start();
    return;
  }

  if (handle_->id().id.seqno != 0) {
    auto S = state->validate_deep();
    if (S.is_error()) {
      LOG(WARNING) << "received bad state from net: " << S;
      start();
      return;
    }
  } else {
    if (sha256_bits256(data.as_slice()) != handle_->id().file_hash) {
      LOG(WARNING) << "received bad state from net: file hash mismatch";
      start();
      return;
    }
  }
  handle_->set_logical_time(state->get_logical_time());
  handle_->set_unix_time(state->get_unix_time());
  handle_->set_is_key_block(handle_->id().is_masterchain() && handle_->id().id.seqno == 0);
  handle_->set_split(state->before_split());

  prev_state_ = std::move(state);
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &WaitBlockState::abort_query, R.move_as_error_prefix("db set error: "));
    } else {
      td::actor::send_closure(SelfId, &WaitBlockState::written_state_file);
    }
  });

  td::actor::send_closure(manager_, &ValidatorManager::store_zero_state_file, handle_->id(), std::move(data),
                          std::move(P));
}

void WaitBlockState::written_state_file() {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<ShardState>> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &WaitBlockState::abort_query, R.move_as_error_prefix("db set error: "));
    } else {
      td::actor::send_closure(SelfId, &WaitBlockState::written_state, R.move_as_ok());
    }
  });

  td::actor::send_closure(manager_, &ValidatorManager::set_block_state, handle_, prev_state_, std::move(P));
}

void WaitBlockState::failed_to_get_zero_state() {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &WaitBlockState::failed_to_get_state_from_net,
                              R.move_as_error_prefix("net error: "));
    } else {
      td::actor::send_closure(SelfId, &WaitBlockState::got_state_from_net, R.move_as_ok());
    }
  });
  td::actor::send_closure(manager_, &ValidatorManager::send_get_zero_state_request, handle_->id(), priority_,
                          std::move(P));
}

void WaitBlockState::failed_to_get_state_from_net(td::Status reason) {
  if (reason.code() == ErrorCode::notready) {
    LOG(DEBUG) << "failed to download state for " << handle_->id() << " from net: " << reason;
  } else {
    LOG(WARNING) << "failed to download state for " << handle_->id() << " from net: " << reason;
  }

  start();
}

}  // namespace validator

}  // namespace ton
