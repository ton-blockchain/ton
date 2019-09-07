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
#include "top-shard-description.hpp"
#include "ton/ton-tl.hpp"
#include "tl-utils/tl-utils.hpp"
#include "common/errorcode.h"
#include "validator/fabric.h"

namespace ton {

namespace validator {

namespace dummy0 {

bool ShardTopBlockDescriptionImpl::may_be_valid(BlockHandle last_masterchain_block_handle,
                                                td::Ref<MasterchainState> last_masterchain_block_state) const {
  if (after_split_ && after_merge_) {
    return false;
  }
  if (!after_split_ && !after_merge_) {
    auto s = last_masterchain_block_state->get_shard_from_config(block_id_.shard_full());
    if (s.is_null()) {
      return false;
    }

    if (s->fsm_state() != McShardHash::FsmState::fsm_none) {
      return false;
    }

    if (s->top_block_id().id.seqno >= block_id_.id.seqno) {
      return false;
    }
  } else if (after_split_) {
    auto s = last_masterchain_block_state->get_shard_from_config(shard_parent(block_id_.shard_full()));
    if (s.is_null()) {
      return false;
    }

    if (s->fsm_state() != McShardHash::FsmState::fsm_split) {
      return false;
    }

    if (s->top_block_id().id.seqno + 1 != block_id_.id.seqno) {
      return false;
    }
  } else {
    auto s1 = last_masterchain_block_state->get_shard_from_config(shard_child(block_id_.shard_full(), true));
    if (s1.is_null()) {
      return false;
    }
    auto s2 = last_masterchain_block_state->get_shard_from_config(shard_child(block_id_.shard_full(), false));
    if (s2.is_null()) {
      return false;
    }

    if (s1->fsm_state() != McShardHash::FsmState::fsm_merge || s2->fsm_state() != McShardHash::FsmState::fsm_merge) {
      return false;
    }

    if (std::max(s1->top_block_id().id.seqno, s2->top_block_id().id.seqno) + 1 != block_id_.id.seqno) {
      return false;
    }
  }

  auto val_set = last_masterchain_block_state->get_validator_set(block_id_.shard_full());
  if (val_set->get_catchain_seqno() != catchain_seqno_) {
    return false;
  }

  return true;
}

td::BufferSlice ShardTopBlockDescriptionImpl::serialize() const {
  return serialize_tl_object(create_tl_object<ton_api::test0_topShardBlockDescription>(
                                 create_tl_block_id(block_id_), after_split_, after_merge_, before_split_,
                                 catchain_seqno_, validator_set_hash_, signatures_.clone()),
                             true);
}

td::Result<td::Ref<ShardTopBlockDescription>> ShardTopBlockDescriptionImpl::fetch(td::BufferSlice data) {
  TRY_RESULT(F, fetch_tl_object<ton_api::test0_topShardBlockDescription>(std::move(data), true));

  return td::Ref<ShardTopBlockDescriptionImpl>{true,
                                               create_block_id(F->block_id_),
                                               F->after_split_,
                                               F->after_merge_,
                                               F->before_split_,
                                               F->catchain_seqno_,
                                               F->validator_set_hash_,
                                               F->signatures_.clone()};
}

void ValidateShardTopBlockDescription::finish_query() {
  if (promise_) {
    promise_.set_value(ShardTopBlockDescriptionImpl::fetch(data_.clone()).move_as_ok());
  }
  stop();
}

void ValidateShardTopBlockDescription::abort_query(td::Status reason) {
  if (promise_) {
    promise_.set_error(std::move(reason));
  }
  stop();
}

void ValidateShardTopBlockDescription::alarm() {
  abort_query(td::Status::Error(ErrorCode::timeout, "timeout"));
}

void ValidateShardTopBlockDescription::start_up() {
  alarm_timestamp() = timeout_;

  auto F = fetch_tl_object<ton_api::test0_topShardBlockDescription>(data_.clone(), true);
  if (F.is_error()) {
    abort_query(F.move_as_error());
    return;
  }
  unserialized_ = F.move_as_ok();

  auto id = create_block_id(unserialized_->block_id_);

  auto val_set = state_->get_validator_set(id.shard_full());
  if (val_set->get_catchain_seqno() != static_cast<CatchainSeqno>(unserialized_->catchain_seqno_)) {
    abort_query(td::Status::Error(ErrorCode::protoviolation, "bad validator set ts"));
    return;
  }
  if (val_set->get_validator_set_hash() != static_cast<td::uint32>(unserialized_->validator_set_hash_)) {
    abort_query(td::Status::Error(ErrorCode::protoviolation, "bad validator set hash"));
    return;
  }

  auto sig_setR = create_signature_set(unserialized_->signatures_.clone());
  if (sig_setR.is_error()) {
    abort_query(sig_setR.move_as_error());
    return;
  }

  auto S = val_set->check_signatures(id.root_hash, id.file_hash, sig_setR.move_as_ok());
  if (S.is_error()) {
    abort_query(S.move_as_error());
    return;
  }

  finish_query();
}

}  // namespace dummy0

}  // namespace validator

}  // namespace ton
