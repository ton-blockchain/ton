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
#include "validate-query.hpp"
#include "adnl/utils.hpp"
#include "ton/ton-tl.hpp"
#include "ton/ton-io.hpp"

namespace ton {

namespace validator {

namespace dummy0 {

void ValidateQuery::alarm() {
  abort_query(td::Status::Error(ErrorCode::timeout, "timeout"));
}

void ValidateQuery::abort_query(td::Status reason) {
  if (promise_) {
    LOG(WARNING) << "aborting validate block candidate query: " << reason;
    promise_.set_error(std::move(reason));
  }
  stop();
}

void ValidateQuery::reject_query(std::string reason, td::BufferSlice proof) {
  if (promise_) {
    LOG(WARNING) << "rejecting validate block candidate query: " << reason;
    promise_.set_value(CandidateReject{std::move(reason), std::move(proof)});
  }
  stop();
}

void ValidateQuery::finish_query() {
  if (promise_) {
    promise_.set_result(block_ts_);
  }
  stop();
}

void ValidateQuery::start_up() {
  alarm_timestamp() = timeout_;

  auto F = fetch_tl_object<ton_api::test0_shardchain_block>(candidate_.data.clone(), true);
  if (F.is_error()) {
    abort_query(F.move_as_error());
    return;
  }
  unserialized_block_ = F.move_as_ok();
  block_ts_ = unserialized_block_->ts_;

  if (unserialized_block_->workchain_ != shard_.workchain) {
    abort_query(td::Status::Error(ErrorCode::protoviolation, "bad workchain"));
    return;
  }
  if (static_cast<ShardId>(unserialized_block_->shard_) != shard_.shard) {
    abort_query(td::Status::Error(ErrorCode::protoviolation, "bad shard"));
    return;
  }

  BlockSeqno max_seqno = 0;
  if (prev_.size() != unserialized_block_->prev_.size()) {
    abort_query(td::Status::Error(ErrorCode::protoviolation, "wrong prev block count"));
    return;
  }
  for (size_t i = 0; i < prev_.size(); i++) {
    if (prev_[i].id.seqno > max_seqno) {
      max_seqno = prev_[i].id.seqno;
    }
    auto p = create_block_id(unserialized_block_->prev_[i]);
    if (p != prev_[i]) {
      LOG(WARNING) << p << " " << prev_[i];
      abort_query(td::Status::Error(ErrorCode::protoviolation, "wrong prev block"));
      return;
    }
  }
  if (static_cast<BlockSeqno>(unserialized_block_->seqno_) != max_seqno + 1) {
    abort_query(td::Status::Error(ErrorCode::protoviolation, "wrong block seqno"));
    return;
  }
  if (static_cast<CatchainSeqno>(unserialized_block_->catchain_seqno_) != catchain_seqno_) {
    abort_query(td::Status::Error(ErrorCode::protoviolation, "wrong validator set ts"));
    return;
  }
  if (static_cast<td::uint32>(unserialized_block_->validator_set_hash_) != validator_set_hash_) {
    abort_query(td::Status::Error(ErrorCode::protoviolation, "wrong validator set hash"));
    return;
  }

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<ShardState>> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &ValidateQuery::abort_query, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &ValidateQuery::got_prev_state, R.move_as_ok());
    }
  });

  if (prev_.size() == 1) {
    td::actor::send_closure(manager_, &ValidatorManager::wait_block_state_short, prev_[0], timeout_, std::move(P));
  } else {
    td::actor::send_closure(manager_, &ValidatorManager::wait_block_state_merge, prev_[0], prev_[1], timeout_,
                            std::move(P));
  }
}

void ValidateQuery::got_prev_state(td::Ref<ShardState> R) {
  if (R->get_unix_time() >= static_cast<UnixTime>(unserialized_block_->ts_)) {
    abort_query(td::Status::Error(ErrorCode::protoviolation, "too small ts"));
    return;
  }

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &ValidateQuery::abort_query, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &ValidateQuery::written_candidate);
    }
  });

  td::actor::send_closure(manager_, &ValidatorManager::set_block_candidate, candidate_.id, candidate_.clone(),
                          std::move(P));
}

void ValidateQuery::written_candidate() {
  finish_query();
}

ValidateQuery::ValidateQuery(ShardIdFull shard, UnixTime min_ts, BlockIdExt min_masterchain_block_id,
                             std::vector<BlockIdExt> prev, BlockCandidate candidate, CatchainSeqno catchain_seqno,
                             td::uint32 validator_set_hash, td::actor::ActorId<ValidatorManager> manager,
                             td::Timestamp timeout, td::Promise<ValidateCandidateResult> promise)
    : shard_(shard)
    , min_ts_(min_ts)
    , min_masterchain_block_id_(min_masterchain_block_id)
    , prev_(std::move(prev))
    , candidate_(std::move(candidate))
    , catchain_seqno_(catchain_seqno)
    , validator_set_hash_(validator_set_hash)
    , manager_(manager)
    , timeout_(timeout)
    , promise_(std::move(promise)) {
}

}  // namespace dummy0

}  // namespace validator

}  // namespace ton
