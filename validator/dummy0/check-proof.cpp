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
#include "check-proof.hpp"
#include "adnl/utils.hpp"
#include "ton/ton-io.hpp"
#include "ton/ton-tl.hpp"
#include "validator/fabric.h"
#include "validator/invariants.hpp"

namespace ton {

namespace validator {

namespace dummy0 {

void CheckProofLink::alarm() {
  abort_query(td::Status::Error(ErrorCode::timeout, "timeout"));
}

void CheckProofLink::abort_query(td::Status reason) {
  if (promise_) {
    LOG(WARNING) << "aborting check proof link for " << id_ << " query: " << reason;
    promise_.set_error(std::move(reason));
  }
  stop();
}

void CheckProofLink::finish_query() {
  ValidatorInvariants::check_post_check_proof_link(handle_);
  if (promise_) {
    LOG(WARNING) << "checked proof link for " << handle_->id();
    promise_.set_result(handle_);
  }
  stop();
}

void CheckProofLink::start_up() {
  alarm_timestamp() = timeout_;

  auto F = fetch_tl_object<ton_api::test0_proofLink>(proof_->data(), true);
  if (F.is_error()) {
    abort_query(F.move_as_error());
    return;
  }

  unserialized_proof_ = F.move_as_ok();
  if (create_block_id(unserialized_proof_->id_) != id_) {
    abort_query(td::Status::Error(ErrorCode::protoviolation, "proof for wrong block"));
    return;
  }

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<BlockHandle> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &CheckProofLink::abort_query, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &CheckProofLink::got_block_handle, R.move_as_ok());
    }
  });

  td::actor::send_closure(manager_, &ValidatorManager::get_block_handle, id_, true, std::move(P));
}

void CheckProofLink::got_block_handle(BlockHandle handle) {
  // RUN CHECKS
  // SHOULD ONLY BE SOME MERKLE PROOFS
  // DUMMY0 DOES NOT DO IT

  handle_ = std::move(handle);

  std::vector<BlockIdExt> prev;
  for (auto &p : unserialized_proof_->prev_) {
    prev.push_back(create_block_id(p));
  }
  for (auto &p : prev) {
    handle_->set_prev(p);
  }
  handle_->set_merge(prev.size() == 2);
  handle_->set_split(unserialized_proof_->split_);

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &CheckProofLink::abort_query, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &CheckProofLink::finish_query);
    }
  });
  td::actor::send_closure(manager_, &ValidatorManager::set_block_proof_link, handle_, proof_, std::move(P));
}
void CheckProof::alarm() {
  abort_query(td::Status::Error(ErrorCode::timeout, "timeout"));
}

void CheckProof::abort_query(td::Status reason) {
  if (promise_) {
    LOG(WARNING) << "aborting check proof for " << id_ << " query: " << reason;
    promise_.set_error(std::move(reason));
  }
  stop();
}

void CheckProof::finish_query() {
  ValidatorInvariants::check_post_check_proof(handle_);
  if (promise_) {
    LOG(WARNING) << "checked proof for " << handle_->id();
    promise_.set_result(handle_);
  }
  stop();
}

void CheckProof::start_up() {
  alarm_timestamp() = timeout_;

  auto F = fetch_tl_object<ton_api::test0_proof>(proof_->data(), true);
  if (F.is_error()) {
    abort_query(F.move_as_error());
    return;
  }

  unserialized_proof_ = F.move_as_ok();

  auto proof_link_R = create_proof_link(serialize_tl_object(unserialized_proof_->link_, true));
  if (proof_link_R.is_error()) {
    abort_query(proof_link_R.move_as_error());
    return;
  }

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<BlockHandle> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &CheckProof::abort_query, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &CheckProof::got_block_handle, R.move_as_ok());
    }
  });

  run_check_proof_link_query(id_, proof_link_R.move_as_ok(), manager_, timeout_, std::move(P));
}

void CheckProof::got_block_handle(BlockHandle handle) {
  handle_ = std::move(handle);
  if (handle_ && handle_->inited_proof()) {
    finish_query();
    return;
  }

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<ShardState>> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &CheckProof::abort_query, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &CheckProof::got_masterchain_state, td::Ref<MasterchainState>{R.move_as_ok()});
    }
  });
  CHECK(!handle_->merge_before());
  td::actor::send_closure(manager_, &ValidatorManager::wait_block_state_short, handle_->one_prev(true), timeout_,
                          std::move(P));
}

void CheckProof::got_masterchain_state(td::Ref<MasterchainState> state) {
  state_ = std::move(state);

  auto s = state_->get_validator_set(id_.shard_full());
  if (s->get_catchain_seqno() != static_cast<CatchainSeqno>(unserialized_proof_->catchain_seqno_)) {
    abort_query(td::Status::Error(ErrorCode::protoviolation, "bad validator set ts"));
    return;
  }
  if (s->get_validator_set_hash() != static_cast<td::uint32>(unserialized_proof_->validator_set_hash_)) {
    abort_query(td::Status::Error(ErrorCode::protoviolation, "bad validator set hash"));
    return;
  }

  std::vector<BlockSignature> vec;
  for (auto &v : unserialized_proof_->signatures_->signatures_) {
    vec.emplace_back(BlockSignature{UInt256_2_Bits256(v->who_), std::move(v->signature_)});
  }

  auto sigs = create_signature_set(std::move(vec));

  auto S = s->check_signatures(id_.root_hash, id_.file_hash, sigs);
  if (S.is_error()) {
    abort_query(S.move_as_error());
    return;
  }

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &CheckProof::abort_query, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &CheckProof::set_next);
    }
  });
  td::actor::send_closure(manager_, &ValidatorManager::set_next_block, handle_->one_prev(true), handle_->id(),
                          std::move(P));
}

void CheckProof::set_next() {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &CheckProof::abort_query, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &CheckProof::finish_query);
    }
  });
  td::actor::send_closure(manager_, &ValidatorManager::set_block_proof, handle_, proof_, std::move(P));
}

}  // namespace dummy0

}  // namespace validator

}  // namespace ton
