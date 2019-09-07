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
#include "collate-query.hpp"
#include "adnl/utils.hpp"
#include "td/utils/Random.h"
#include "ton/ton-tl.hpp"
#include "shard.hpp"
#include "validator/fabric.h"

namespace ton {

namespace validator {

namespace dummy0 {

void CollateQuery::abort_query(td::Status reason) {
  if (promise_) {
    LOG(WARNING) << "aborting collate query: " << reason;
    promise_.set_error(std::move(reason));
  }
  stop();
}
void CollateQuery::finish_query() {
  if (promise_) {
    promise_.set_value(std::move(candidate_));
  }
  stop();
}

void CollateQuery::alarm() {
  abort_query(td::Status::Error(ErrorCode::timeout, "timeout"));
}

void CollateQuery::start_up() {
  //CHECK(shard_.workchain == masterchainId);
  //CHECK(shard_.shard == shardIdAll);
  LOG(WARNING) << "collate query: prev=" << prev_.size() << " ts=" << validator_set_->get_catchain_seqno();

  alarm_timestamp() = timeout_;
  ts_ = static_cast<UnixTime>(td::Clocks::system());
  if (ts_ < min_ts_) {
    ts_ = min_ts_;
  }

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<ShardState>> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &CollateQuery::abort_query, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &CollateQuery::got_prev_state, R.move_as_ok());
    }
  });

  if (prev_.size() == 1) {
    td::actor::send_closure(manager_, &ValidatorManager::wait_block_state_short, prev_[0], timeout_, std::move(P));
  } else {
    CHECK(prev_.size() == 2);
    td::actor::send_closure(manager_, &ValidatorManager::wait_block_state_merge, prev_[0], prev_[1], timeout_,
                            std::move(P));
  }
}

void CollateQuery::got_prev_state(td::Ref<ShardState> recv_state) {
  prev_state_ = td::Ref<ShardStateImpl>{std::move(recv_state)};
  CHECK(prev_state_.not_null());

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<MasterchainState>> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &CollateQuery::abort_query, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &CollateQuery::got_masterchain_state, R.move_as_ok());
    }
  });
  td::actor::send_closure(manager_, &ValidatorManager::get_top_masterchain_state, std::move(P));
}

void CollateQuery::got_masterchain_state(td::Ref<MasterchainState> state) {
  masterchain_state_ = std::move(state);

  if (masterchain_state_->get_block_id() < min_masterchain_block_id_) {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<ShardState>> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &CollateQuery::abort_query, R.move_as_error());
      } else {
        td::actor::send_closure(SelfId, &CollateQuery::got_masterchain_state,
                                td::Ref<MasterchainState>{R.move_as_ok()});
      }
    });
    td::actor::send_closure(manager_, &ValidatorManager::wait_block_state_short, min_masterchain_block_id_, timeout_,
                            std::move(P));
    return;
  }

  if (!shard_.is_masterchain()) {
    generate();
  } else {
    auto P = td::PromiseCreator::lambda(
        [SelfId = actor_id(this)](td::Result<std::vector<td::Ref<ShardTopBlockDescription>>> R) {
          if (R.is_error()) {
            td::actor::send_closure(SelfId, &CollateQuery::abort_query, R.move_as_error());
          } else {
            td::actor::send_closure(SelfId, &CollateQuery::got_shard_messages, R.move_as_ok());
          }
        });
    td::actor::send_closure(manager_, &ValidatorManager::get_shard_blocks, prev_[0], std::move(P));
  }
}

void CollateQuery::generate() {
  BlockSeqno seqno;
  if (prev_.size() == 1) {
    seqno = prev_[0].id.seqno + 1;
  } else {
    CHECK(prev_.size() == 2);
    seqno = std::max(prev_[0].id.seqno, prev_[1].id.seqno) + 1;
  }

  if (shard_.is_masterchain()) {
    if (seqno <= masterchain_state_->get_seqno()) {
      abort_query(td::Status::Error(ErrorCode::notready, "generating block, but newer already accepted"));
      return;
    }
  }

  auto v = masterchain_state_->get_validator_set(shard_);
  if (v->get_catchain_seqno() != validator_set_->get_catchain_seqno()) {
    abort_query(td::Status::Error(ErrorCode::protoviolation, "bad validator set"));
    return;
  }
  CHECK(v->get_validator_set_hash() == validator_set_->get_validator_set_hash());

  std::vector<tl_object_ptr<ton_api::tonNode_blockIdExt>> prev;
  for (auto& p : prev_) {
    prev.emplace_back(create_tl_block_id(p));
  }

  auto data = td::BufferSlice{10000};
  td::Random::secure_bytes(data.as_slice());

  auto block = create_tl_object<ton_api::test0_shardchain_block>(
      shard_.workchain, shard_.shard, seqno, std::move(prev), false, ts_, td::UInt256::zero(),
      validator_set_->get_catchain_seqno(), validator_set_->get_validator_set_hash(), std::move(data),
      create_tl_object<ton_api::test0_masterchainBlockExtra_empty>());

  if (shard_.is_masterchain()) {
    auto m_state = td::Ref<MasterchainStateImpl>{prev_state_};

    bool rotate = ts_ >= m_state->next_validator_rotate_at();

    auto x = create_tl_object<ton_api::test0_masterchainBlockExtra_extra>(td::Random::fast_uint32(), rotate,
                                                                          std::move(shards_));
    block->extra_ = std::move(x);
  }

  Bits256 x;
  x.set_zero();
  auto block_R =
      create_block(BlockIdExt{shard_.workchain, shard_.shard, seqno, x, x}, serialize_tl_object(block, true));
  if (block_R.is_error()) {
    abort_query(block_R.move_as_error());
    return;
  }
  auto s =
      prev_state_.write().apply_block(BlockIdExt{shard_.workchain, shard_.shard, seqno, x, x}, block_R.move_as_ok());
  if (s.is_error()) {
    abort_query(std::move(s));
    return;
  }
  block->state_ = Bits256_2_UInt256(prev_state_->root_hash());

  auto B = serialize_tl_object(block, true);
  auto file_hash = UInt256_2_Bits256(sha256_uint256(B.as_slice()));
  auto root_hash = file_hash;

  auto collated_data = td::BufferSlice{10000};
  td::Random::secure_bytes(collated_data.as_slice());
  auto collated_data_file_hash = UInt256_2_Bits256(sha256_uint256(collated_data.as_slice()));

  candidate_.collated_data = std::move(collated_data);
  candidate_.collated_file_hash = collated_data_file_hash;
  candidate_.data = std::move(B);
  candidate_.id = BlockIdExt{BlockId{shard_.workchain, shard_.shard, seqno}, root_hash, file_hash};

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &CollateQuery::abort_query, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &CollateQuery::finish_query);
    }
  });

  td::actor::send_closure(manager_, &ValidatorManager::set_block_candidate, candidate_.id, candidate_.clone(),
                          std::move(P));
}

void CollateQuery::got_shard_messages(std::vector<td::Ref<ShardTopBlockDescription>> shards) {
  for (auto& s : shards) {
    // TODO validate
    shards_.emplace_back(create_tl_object<ton_api::test0_masterchain_shardInfo>(
        create_tl_block_id(s->block_id()), false, s->before_split(), false, false));
  }
  generate();
}

CollateQuery::CollateQuery(ShardIdFull shard, UnixTime min_ts, BlockIdExt min_masterchain_block_id,
                           std::vector<BlockIdExt> prev, td::Ref<ValidatorSet> validator_set,
                           td::actor::ActorId<ValidatorManager> manager, td::Timestamp timeout,
                           td::Promise<BlockCandidate> promise)
    : shard_(shard)
    , min_ts_(min_ts)
    , min_masterchain_block_id_{min_masterchain_block_id}
    , prev_(std::move(prev))
    , validator_set_(std::move(validator_set))
    , manager_(manager)
    , timeout_(timeout)
    , promise_(std::move(promise)) {
}

}  // namespace dummy0

}  // namespace validator

}  // namespace ton
