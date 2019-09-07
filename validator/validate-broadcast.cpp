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
#include "validate-broadcast.hpp"
#include "fabric.h"
#include "adnl/utils.hpp"
#include "ton/ton-io.hpp"
#include "apply-block.hpp"

namespace ton {

namespace validator {

void ValidateBroadcast::abort_query(td::Status reason) {
  if (promise_) {
    VLOG(VALIDATOR_WARNING) << "aborting validate broadcast query for " << broadcast_.block_id << ": " << reason;
    promise_.set_error(std::move(reason));
  }
  stop();
}

void ValidateBroadcast::finish_query() {
  if (promise_) {
    VLOG(VALIDATOR_DEBUG) << "validated broadcast for " << broadcast_.block_id;
    promise_.set_result(td::Unit());
  }
  stop();
}

void ValidateBroadcast::alarm() {
  abort_query(td::Status::Error(ErrorCode::timeout, "timeout"));
}

void ValidateBroadcast::start_up() {
  VLOG(VALIDATOR_DEBUG) << "received broadcast for " << broadcast_.block_id;
  alarm_timestamp() = timeout_;

  auto hash = sha256_bits256(broadcast_.data.as_slice());
  if (hash != broadcast_.block_id.file_hash) {
    abort_query(td::Status::Error(ErrorCode::protoviolation, "filehash mismatch"));
    return;
  }

  if (broadcast_.block_id.is_masterchain()) {
    if (last_masterchain_block_handle_->id().id.seqno >= broadcast_.block_id.id.seqno) {
      finish_query();
      return;
    }
  }

  sig_set_ = create_signature_set(std::move(broadcast_.signatures));
  if (sig_set_.is_null()) {
    abort_query(td::Status::Error(ErrorCode::protoviolation, "bad signature set"));
    return;
  }

  auto val_set = last_masterchain_state_->get_validator_set(broadcast_.block_id.shard_full());
  if (val_set.not_null() && val_set->get_catchain_seqno() == broadcast_.catchain_seqno &&
      val_set->get_validator_set_hash() == broadcast_.validator_set_hash) {
    auto S = val_set->check_signatures(broadcast_.block_id.root_hash, broadcast_.block_id.file_hash, sig_set_);
    if (S.is_ok()) {
      checked_signatures();
      return;
    } else {
      abort_query(S.move_as_error_prefix("failed signature check: "));
      return;
    }
  }

  val_set = last_masterchain_state_->get_next_validator_set(broadcast_.block_id.shard_full());
  if (val_set.not_null() && val_set->get_catchain_seqno() == broadcast_.catchain_seqno &&
      val_set->get_validator_set_hash() == broadcast_.validator_set_hash) {
    auto S = val_set->check_signatures(broadcast_.block_id.root_hash, broadcast_.block_id.file_hash, sig_set_);
    if (S.is_ok()) {
      checked_signatures();
      return;
    } else {
      abort_query(S.move_as_error_prefix("failed signature check: "));
      return;
    }
  }

  abort_query(td::Status::Error(ErrorCode::protoviolation, "bad signature set"));
}

void ValidateBroadcast::checked_signatures() {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<BlockHandle> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &ValidateBroadcast::abort_query, R.move_as_error_prefix("db error: "));
    } else {
      td::actor::send_closure(SelfId, &ValidateBroadcast::got_block_handle, R.move_as_ok());
    }
  });

  td::actor::send_closure(manager_, &ValidatorManager::get_block_handle, broadcast_.block_id, true, std::move(P));
}

void ValidateBroadcast::got_block_handle(BlockHandle handle) {
  handle_ = std::move(handle);

  auto dataR = create_block(broadcast_.block_id, broadcast_.data.clone());
  if (dataR.is_error()) {
    abort_query(dataR.move_as_error_prefix("bad block data: "));
    return;
  }
  data_ = dataR.move_as_ok();

  if (handle_->received()) {
    written_block_data();
    return;
  }

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &ValidateBroadcast::abort_query, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &ValidateBroadcast::written_block_data);
    }
  });

  td::actor::send_closure(manager_, &ValidatorManager::set_block_data, handle_, data_, std::move(P));
}

void ValidateBroadcast::written_block_data() {
  if (handle_->id().is_masterchain()) {
    if (handle_->inited_proof()) {
      checked_proof();
      return;
    }
    auto proofR = create_proof(broadcast_.block_id, broadcast_.proof.clone());
    if (proofR.is_error()) {
      abort_query(proofR.move_as_error_prefix("bad proof: "));
      return;
    }
    proof_ = proofR.move_as_ok();
    if (handle_->id().id.seqno == last_masterchain_block_handle_->id().id.seqno + 1) {
      auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<BlockHandle> R) {
        if (R.is_error()) {
          td::actor::send_closure(SelfId, &ValidateBroadcast::abort_query, R.move_as_error_prefix("db error: "));
        } else {
          td::actor::send_closure(SelfId, &ValidateBroadcast::checked_proof);
        }
      });
      run_check_proof_query(broadcast_.block_id, proof_, manager_, timeout_, std::move(P));
    } else {
      checked_proof();
    }
  } else {
    if (handle_->inited_proof_link()) {
      checked_proof();
      return;
    }
    auto proofR = create_proof_link(broadcast_.block_id, broadcast_.proof.clone());
    if (proofR.is_error()) {
      abort_query(proofR.move_as_error());
      return;
    }
    proof_link_ = proofR.move_as_ok();
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<BlockHandle> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &ValidateBroadcast::abort_query, R.move_as_error_prefix("db error: "));
      } else {
        td::actor::send_closure(SelfId, &ValidateBroadcast::checked_proof);
      }
    });
    run_check_proof_link_query(broadcast_.block_id, proof_link_, manager_, timeout_, std::move(P));
  }
}

void ValidateBroadcast::checked_proof() {
  if (handle_->inited_proof()) {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &ValidateBroadcast::abort_query, R.move_as_error());
      } else {
        td::actor::send_closure(SelfId, &ValidateBroadcast::finish_query);
      }
    });

    td::actor::create_actor<ApplyBlock>("applyblock", handle_->id(), data_, manager_, timeout_, std::move(P)).release();
  } else {
    finish_query();
  }
}

}  // namespace validator

}  // namespace ton
