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
#include "accept-block.hpp"
#include "adnl/utils.hpp"
#include "validator/interfaces/validator-manager.h"
#include "ton/ton-tl.hpp"
#include "ton/ton-io.hpp"

#include "validator/fabric.h"
#include "validator/invariants.hpp"

#include "top-shard-description.hpp"

namespace ton {

namespace validator {

namespace dummy0 {

void AcceptBlockQuery::abort_query(td::Status reason) {
  if (promise_) {
    LOG(WARNING) << "aborting accept block " << id_ << " query: " << reason;
    promise_.set_error(std::move(reason));
  }
  stop();
}

void AcceptBlockQuery::finish_query() {
  ValidatorInvariants::check_post_accept(handle_);
  if (promise_) {
    promise_.set_value(td::Unit());
  }
  stop();
}

void AcceptBlockQuery::alarm() {
  abort_query(td::Status::Error(ErrorCode::timeout, "timeout"));
}

void AcceptBlockQuery::start_up() {
  alarm_timestamp() = timeout_;

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<BlockHandle> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &AcceptBlockQuery::abort_query, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &AcceptBlockQuery::got_block_handle, R.move_as_ok());
    }
  });

  td::actor::send_closure(manager_, &ValidatorManager::get_block_handle, id_, true, std::move(P));
}

void AcceptBlockQuery::got_block_handle(BlockHandle handle) {
  handle_ = std::move(handle);
  if (handle_->processed() && handle_->received() && handle_->received_state() && handle_->inited_signatures() &&
      handle_->inited_split_after() && handle_->inited_merge_before() && handle_->inited_prev() &&
      (id_.is_masterchain() ? handle_->inited_proof() : handle_->inited_proof_link())) {
    send_block_description();
    return;
  }
  if (data_.not_null() && !handle_->received()) {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &AcceptBlockQuery::abort_query, R.move_as_error());
      } else {
        td::actor::send_closure(SelfId, &AcceptBlockQuery::written_block_data);
      }
    });

    td::actor::send_closure(manager_, &ValidatorManager::set_block_data, handle_, data_, std::move(P));
  } else {
    written_block_data();
  }
}

void AcceptBlockQuery::written_block_data() {
  if (handle_->inited_signatures()) {
    written_block_signatures();
    return;
  }
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &AcceptBlockQuery::abort_query, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &AcceptBlockQuery::written_block_signatures);
    }
  });

  td::actor::send_closure(manager_, &ValidatorManager::set_block_signatures, handle_, signatures_, std::move(P));
}

void AcceptBlockQuery::written_block_signatures() {
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
        td::actor::send_closure(SelfId, &AcceptBlockQuery::abort_query, R.move_as_error());
      } else {
        td::actor::send_closure(SelfId, &AcceptBlockQuery::written_block_info);
      }
    });

    handle_->flush(manager_, handle_, std::move(P));
  } else {
    written_block_info();
  }
}

void AcceptBlockQuery::written_block_info() {
  LOG(WARNING) << "written block info";
  if (data_.not_null()) {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<ShardState>> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &AcceptBlockQuery::abort_query, R.move_as_error());
      } else {
        td::actor::send_closure(SelfId, &AcceptBlockQuery::got_prev_state, R.move_as_ok());
      }
    });

    CHECK(prev_.size() <= 2);
    td::actor::send_closure(manager_, &ValidatorManager::wait_prev_block_state, handle_, timeout_, std::move(P));
  } else {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<BlockData>> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &AcceptBlockQuery::failed_to_get_block_candidate);
      } else {
        td::actor::send_closure(SelfId, &AcceptBlockQuery::got_block_data, R.move_as_ok());
      }
    });

    td::actor::send_closure(manager_, &ValidatorManager::get_block_candidate_data_from_db, id_, std::move(P));
  }
}

void AcceptBlockQuery::failed_to_get_block_candidate() {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<BlockData>> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &AcceptBlockQuery::abort_query, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &AcceptBlockQuery::got_block_data, R.move_as_ok());
    }
  });

  td::actor::send_closure(manager_, &ValidatorManager::wait_block_data, handle_, timeout_, std::move(P));
}

void AcceptBlockQuery::got_block_data(td::Ref<BlockData> data) {
  data_ = std::move(data);
  if (handle_->received()) {
    written_block_info();
  } else {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &AcceptBlockQuery::abort_query, R.move_as_error());
      } else {
        td::actor::send_closure(SelfId, &AcceptBlockQuery::written_block_data);
      }
    });

    td::actor::send_closure(manager_, &ValidatorManager::set_block_data, handle_, data_, std::move(P));
  }
}

void AcceptBlockQuery::got_prev_state(td::Ref<ShardState> state) {
  LOG(WARNING) << "got prev state";
  state_ = std::move(state);

  state_.write().apply_block(id_, data_).ensure();

  handle_->set_split(state_->before_split());

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &AcceptBlockQuery::abort_query, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &AcceptBlockQuery::written_state);
    }
  });

  td::actor::send_closure(manager_, &ValidatorManager::set_block_state, handle_, state_, std::move(P));
}

void AcceptBlockQuery::written_state() {
  LOG(WARNING) << "written state";

  // generate proof

  CHECK(prev_.size() == 1);
  std::vector<tl_object_ptr<ton_api::tonNode_blockIdExt>> prev;
  for (auto &p : prev_) {
    prev.emplace_back(create_tl_block_id(p));
  }

  auto proof_link = create_tl_object<ton_api::test0_proofLink>(
      create_tl_block_id(id_), std::move(prev), Bits256_2_UInt256(state_->root_hash()), handle_->split_after());
  proof_link_ = create_proof_link(serialize_tl_object(proof_link, true)).move_as_ok();

  if (id_.is_masterchain()) {
    auto proof = create_tl_object<ton_api::test0_proof>(
        std::move(proof_link), catchain_seqno_, validator_set_hash_,
        fetch_tl_object<ton_api::test0_blockSignatures>(signatures_->serialize(), true).move_as_ok());
    proof_ = create_proof(prev_[0], serialize_tl_object(proof, true)).move_as_ok();
  }

  if (handle_->id().is_masterchain()) {
    CHECK(prev_.size() == 1);
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &AcceptBlockQuery::abort_query, R.move_as_error());
      } else {
        td::actor::send_closure(SelfId, &AcceptBlockQuery::written_block_next);
      }
    });

    td::actor::send_closure(manager_, &ValidatorManager::set_next_block, prev_[0], id_, std::move(P));
  } else {
    written_block_next();
  }
}

void AcceptBlockQuery::written_block_next() {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &AcceptBlockQuery::abort_query, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &AcceptBlockQuery::written_block_proof);
    }
  });

  if (id_.is_masterchain()) {
    td::actor::send_closure(manager_, &ValidatorManager::set_block_proof, handle_, proof_, std::move(P));
  } else {
    td::actor::send_closure(manager_, &ValidatorManager::set_block_proof_link, handle_, proof_link_, std::move(P));
  }
}

void AcceptBlockQuery::written_block_proof() {
  if (handle_->need_flush()) {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &AcceptBlockQuery::abort_query, R.move_as_error());
      } else {
        td::actor::send_closure(SelfId, &AcceptBlockQuery::written_block_info_2);
      }
    });

    handle_->flush(manager_, handle_, std::move(P));
  } else {
    written_block_info_2();
  }
}

void AcceptBlockQuery::written_block_info_2() {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &AcceptBlockQuery::abort_query, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &AcceptBlockQuery::send_block_description);
    }
  });

  td::actor::send_closure(manager_, &ValidatorManager::new_block, handle_, state_, std::move(P));
}

void AcceptBlockQuery::send_block_description() {
  if (!handle_->id().is_masterchain()) {
    bool after_split = prev_.size() == 1 && handle_->id().id.shard != prev_[0].id.shard;
    if (after_split) {
      CHECK(shard_parent(handle_->id().shard_full()) == prev_[0].shard_full());
    }
    bool after_merge = prev_.size() == 2;

    auto desc = td::Ref<ShardTopBlockDescriptionImpl>{true,
                                                      handle_->id(),
                                                      after_split,
                                                      after_merge,
                                                      handle_->split_after(),
                                                      catchain_seqno_,
                                                      validator_set_hash_,
                                                      signatures_->serialize()};
    td::actor::send_closure(manager_, &ValidatorManager::send_top_shard_block_description, std::move(desc));
  }
  finish_query();
}

AcceptBlockQuery::AcceptBlockQuery(BlockIdExt id, td::Ref<BlockData> data, std::vector<BlockIdExt> prev,
                                   UnixTime catchain_seqno, td::uint32 validator_set_hash,
                                   td::Ref<BlockSignatureSet> signatures, bool send_broadcast,
                                   td::actor::ActorId<ValidatorManager> manager, td::Promise<td::Unit> promise)
    : id_(id)
    , data_(std::move(data))
    , prev_(std::move(prev))
    , catchain_seqno_(catchain_seqno)
    , validator_set_hash_(validator_set_hash)
    , signatures_(std::move(signatures))
    , send_broadcast_(send_broadcast)
    , manager_(manager)
    , promise_(std::move(promise)) {
  CHECK(prev_.size() > 0);
}

}  // namespace dummy0

}  // namespace validator

}  // namespace ton
