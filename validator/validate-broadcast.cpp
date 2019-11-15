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

  if (broadcast_.block_id.is_masterchain()) {
    auto R = create_proof(broadcast_.block_id, broadcast_.proof.clone());
    if (R.is_error()) {
      abort_query(R.move_as_error_prefix("bad proof: "));
      return;
    }
    proof_ = R.move_as_ok();
    auto hR = proof_->get_basic_header_info();
    if (hR.is_error()) {
      abort_query(hR.move_as_error_prefix("bad proof: "));
      return;
    }
    header_info_ = hR.move_as_ok();
  } else {
    auto R = create_proof_link(broadcast_.block_id, broadcast_.proof.clone());
    if (R.is_error()) {
      abort_query(R.move_as_error_prefix("bad proof link: "));
      return;
    }
    proof_link_ = R.move_as_ok();
    auto hR = proof_link_->get_basic_header_info();
    if (hR.is_error()) {
      abort_query(hR.move_as_error_prefix("bad proof link: "));
      return;
    }
    header_info_ = hR.move_as_ok();
  }

  BlockSeqno key_block_seqno = header_info_.prev_key_mc_seqno;
  exact_key_block_handle_ = key_block_seqno <= last_known_masterchain_block_handle_->id().seqno();
  if (key_block_seqno < last_known_masterchain_block_handle_->id().seqno()) {
    if (key_block_seqno < last_masterchain_state_->get_seqno()) {
      BlockIdExt block_id;
      if (!last_masterchain_state_->get_old_mc_block_id(key_block_seqno, block_id)) {
        abort_query(td::Status::Error(ErrorCode::error, "too old reference key block"));
        return;
      }
      got_key_block_id(block_id);
    } else if (key_block_seqno == last_masterchain_state_->get_seqno()) {
      got_key_block_handle(last_masterchain_block_handle_);
    } else {
      auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<BlockHandle> R) {
        if (R.is_error()) {
          td::actor::send_closure(SelfId, &ValidateBroadcast::abort_query,
                                  R.move_as_error_prefix("cannot find reference key block id: "));
        } else {
          td::actor::send_closure(SelfId, &ValidateBroadcast::got_key_block_handle, R.move_as_ok());
        }
      });
      td::actor::send_closure(manager_, &ValidatorManager::get_block_by_seqno_from_db,
                              AccountIdPrefixFull{masterchainId, 0}, key_block_seqno, std::move(P));
    }
  } else {
    got_key_block_handle(last_known_masterchain_block_handle_);
  }
}

void ValidateBroadcast::got_key_block_id(BlockIdExt block_id) {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<BlockHandle> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &ValidateBroadcast::abort_query,
                              R.move_as_error_prefix("cannot find reference key block handle: "));
    } else {
      td::actor::send_closure(SelfId, &ValidateBroadcast::got_key_block_handle, R.move_as_ok());
    }
  });
  td::actor::send_closure(manager_, &ValidatorManager::get_block_handle, block_id, false, std::move(P));
}

void ValidateBroadcast::got_key_block_handle(BlockHandle handle) {
  if (handle->id().seqno() == 0) {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<ShardState>> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &ValidateBroadcast::abort_query,
                                R.move_as_error_prefix("failed to get zero state: "));
      } else {
        td::actor::send_closure(SelfId, &ValidateBroadcast::got_zero_state, td::Ref<MasterchainState>{R.move_as_ok()});
      }
    });
    td::actor::send_closure(manager_, &ValidatorManager::get_shard_state_from_db, handle, std::move(P));
  } else {
    if (!handle->inited_proof() && !handle->inited_proof_link()) {
      abort_query(td::Status::Error(ErrorCode::notready, "reference key block proof not received"));
      return;
    }
    if (!handle->is_key_block()) {
      abort_query(td::Status::Error(ErrorCode::protoviolation, "reference key block is not key"));
      return;
    }

    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<ProofLink>> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &ValidateBroadcast::abort_query,
                                R.move_as_error_prefix("cannot get reference key block proof: "));
      } else {
        td::actor::send_closure(SelfId, &ValidateBroadcast::got_key_block_proof_link, R.move_as_ok());
      }
    });
    td::actor::send_closure(manager_, &ValidatorManager::get_block_proof_link_from_db, handle, std::move(P));
  }
}

void ValidateBroadcast::got_key_block_proof_link(td::Ref<ProofLink> key_proof_link) {
  key_proof_link_ = key_proof_link;
  auto confR = key_proof_link->get_key_block_config();
  if (confR.is_error()) {
    abort_query(confR.move_as_error_prefix("failed to extract config from key proof: "));
    return;
  }
  check_signatures_common(confR.move_as_ok());
}

void ValidateBroadcast::got_zero_state(td::Ref<MasterchainState> state) {
  zero_state_ = state;
  auto confR = state->get_key_block_config();
  if (confR.is_error()) {
    abort_query(confR.move_as_error_prefix("failed to extract config from zero state: "));
    return;
  }
  check_signatures_common(confR.move_as_ok());
}

void ValidateBroadcast::check_signatures_common(td::Ref<ConfigHolder> conf) {
  auto val_set = conf->get_validator_set(broadcast_.block_id.shard_full(), header_info_.utime, header_info_.cc_seqno);
  if (val_set.is_null()) {
    abort_query(td::Status::Error(ErrorCode::notready, "failed to compute validator set"));
    return;
  }

  if (val_set->get_validator_set_hash() != header_info_.validator_set_hash) {
    if (!exact_key_block_handle_) {
      abort_query(td::Status::Error(ErrorCode::notready, "too new block, don't know recent enough key block"));
      return;
    } else {
      abort_query(td::Status::Error(ErrorCode::notready, "bad validator set hash"));
      return;
    }
  }
  auto S = val_set->check_signatures(broadcast_.block_id.root_hash, broadcast_.block_id.file_hash, sig_set_);
  if (S.is_ok()) {
    checked_signatures();
  } else {
    abort_query(S.move_as_error_prefix("failed signature check: "));
  }
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
    if (exact_key_block_handle_) {
      auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<BlockHandle> R) {
        if (R.is_error()) {
          td::actor::send_closure(SelfId, &ValidateBroadcast::abort_query, R.move_as_error_prefix("db error: "));
        } else {
          td::actor::send_closure(SelfId, &ValidateBroadcast::checked_proof);
        }
      });
      if (!key_proof_link_.is_null()) {
        run_check_proof_query(broadcast_.block_id, proof_, manager_, timeout_, std::move(P), key_proof_link_);
      } else {
        CHECK(zero_state_.not_null());
        run_check_proof_query(broadcast_.block_id, proof_, manager_, timeout_, std::move(P), zero_state_);
      }
    } else {
      checked_proof();
    }
  } else {
    if (handle_->inited_proof_link()) {
      checked_proof();
      return;
    }
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
  if (handle_->inited_proof() && handle_->is_key_block()) {
    td::actor::send_closure(manager_, &ValidatorManager::update_last_known_key_block, handle_, false);
  }
  if (handle_->inited_proof() && handle_->id().seqno() - last_masterchain_block_handle_->id().seqno() <= 16) {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &ValidateBroadcast::abort_query, R.move_as_error());
      } else {
        td::actor::send_closure(SelfId, &ValidateBroadcast::finish_query);
      }
    });

    td::actor::create_actor<ApplyBlock>("applyblock", handle_->id(), data_, handle_->id(), manager_, timeout_,
                                        std::move(P))
        .release();
  } else {
    finish_query();
  }
}

}  // namespace validator

}  // namespace ton
