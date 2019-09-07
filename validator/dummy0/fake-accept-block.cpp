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
#include "fake-accept-block.hpp"
#include "adnl/utils.hpp"
#include "interfaces/validator-manager.h"
#include "ton/ton-tl.hpp"

#include "fabric.h"

namespace ton {

namespace validator {

void FakeAcceptBlockQuery::abort_query(td::Status reason) {
  if (promise_) {
    LOG(WARNING) << "aborting accept block query: " << reason;
    promise_.set_error(std::move(reason));
  }
  stop();
}

void FakeAcceptBlockQuery::finish_query() {
  if (promise_) {
    promise_.set_value(td::Unit());
  }
  stop();
}

void FakeAcceptBlockQuery::alarm() {
  abort_query(td::Status::Error(ErrorCode::timeout, "timeout"));
}

void FakeAcceptBlockQuery::start_up() {
  alarm_timestamp() = timeout_;

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<BlockHandle> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &FakeAcceptBlockQuery::abort_query, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &FakeAcceptBlockQuery::got_block_handle, R.move_as_ok());
    }
  });

  td::actor::send_closure(manager_, &ValidatorManager::get_block_handle, id_, true, std::move(P));
}

void FakeAcceptBlockQuery::got_block_handle(BlockHandle handle) {
  handle_ = std::move(handle);
  CHECK(!handle_->received());
  CHECK(data_.not_null());
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &FakeAcceptBlockQuery::abort_query, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &FakeAcceptBlockQuery::written_block_data);
    }
  });

  td::actor::send_closure(manager_, &ValidatorManager::set_block_data, handle_, data_, std::move(P));
}

void FakeAcceptBlockQuery::written_block_data() {
  written_block_signatures();
}

void FakeAcceptBlockQuery::written_block_signatures() {
  if (prev_.size() == 2) {
    handle_->set_merge(true);
  } else {
    handle_->set_merge(false);
  }

  for (auto &p : prev_) {
    handle_->set_prev(p);
  }

  if (handle_->need_flush()) {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &FakeAcceptBlockQuery::abort_query, R.move_as_error());
      } else {
        td::actor::send_closure(SelfId, &FakeAcceptBlockQuery::written_block_info);
      }
    });

    handle_->flush(manager_, handle_, std::move(P));
  } else {
    written_block_info();
  }
}

void FakeAcceptBlockQuery::written_block_info() {
  LOG(WARNING) << "written block info";
  CHECK(handle_->received());
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<ShardState>> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &FakeAcceptBlockQuery::abort_query, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &FakeAcceptBlockQuery::got_prev_state, R.move_as_ok());
    }
  });

  CHECK(prev_.size() <= 2);
  td::actor::send_closure(manager_, &ValidatorManager::wait_prev_block_state, handle_, timeout_, std::move(P));
}

void FakeAcceptBlockQuery::got_prev_state(td::Ref<ShardState> state) {
  LOG(WARNING) << "got prev state";
  state_ = std::move(state);

  state_.write().apply_block(id_, data_).ensure();

  handle_->set_split(state_->before_split());

  handle_->set_state_root_hash(state_->root_hash());

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &FakeAcceptBlockQuery::abort_query, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &FakeAcceptBlockQuery::written_state);
    }
  });

  td::actor::send_closure(manager_, &ValidatorManager::set_block_state, handle_, state_, std::move(P));
}

void FakeAcceptBlockQuery::written_state() {
  LOG(WARNING) << "written state";
  if (!id_.id.is_masterchain()) {
    finish_query();
    return;
  }

  // generate proof

  CHECK(prev_.size() == 1);
  proof_ = create_proof(prev_[0], td::BufferSlice()).move_as_ok();

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &FakeAcceptBlockQuery::abort_query, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &FakeAcceptBlockQuery::written_block_proof);
    }
  });
  //handle_->set_masterchain_block(prev_[0]);

  td::actor::send_closure(manager_, &ValidatorManager::set_block_proof, handle_, proof_, std::move(P));
}

void FakeAcceptBlockQuery::written_block_proof() {
  CHECK(prev_.size() <= 1);

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &FakeAcceptBlockQuery::abort_query, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &FakeAcceptBlockQuery::written_block_next);
    }
  });

  td::actor::send_closure(manager_, &ValidatorManager::set_next_block, prev_[0], id_, std::move(P));
}

void FakeAcceptBlockQuery::written_block_next() {
  if (handle_->need_flush()) {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &FakeAcceptBlockQuery::abort_query, R.move_as_error());
      } else {
        td::actor::send_closure(SelfId, &FakeAcceptBlockQuery::written_block_info_2);
      }
    });

    handle_->flush(manager_, handle_, std::move(P));
  } else {
    written_block_info_2();
  }
}

void FakeAcceptBlockQuery::written_block_info_2() {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &FakeAcceptBlockQuery::abort_query, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &FakeAcceptBlockQuery::finish_query);
    }
  });

  td::actor::send_closure(manager_, &ValidatorManager::new_block, handle_, state_, std::move(P));
}

FakeAcceptBlockQuery::FakeAcceptBlockQuery(BlockIdExt id, td::Ref<BlockData> data, std::vector<BlockIdExt> prev,
                                           UnixTime validator_set_ts, td::uint32 validator_set_hash,
                                           td::Ref<BlockSignatureSet> signatures,
                                           td::actor::ActorId<ValidatorManager> manager, td::Promise<td::Unit> promise)
    : id_(id)
    , data_(std::move(data))
    , prev_(std::move(prev))
    , validator_set_ts_(validator_set_ts)
    , validator_set_hash_(validator_set_hash)
    , signatures_(std::move(signatures))
    , manager_(manager)
    , promise_(std::move(promise)) {
  CHECK(prev_.size() > 0);
}

}  // namespace validator

}  // namespace ton
