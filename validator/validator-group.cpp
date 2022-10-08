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
#include "validator-group.hpp"
#include "fabric.h"
#include "ton/ton-io.hpp"
#include "td/utils/overloaded.h"
#include "common/delay.h"

namespace ton {

namespace validator {

void ValidatorGroup::generate_block_candidate(td::uint32 round_id, td::Promise<BlockCandidate> promise) {
  if (round_id > last_known_round_id_) {
    last_known_round_id_ = round_id;
  }
  if (!started_) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "cannot collate block: group not started"));
    return;
  }
  run_collate_query(shard_, min_ts_, min_masterchain_block_id_, prev_block_ids_,
                    Ed25519_PublicKey{local_id_full_.ed25519_value().raw()}, validator_set_, manager_,
                    td::Timestamp::in(10.0), std::move(promise));
}

void ValidatorGroup::validate_block_candidate(td::uint32 round_id, BlockCandidate block,
                                              td::Promise<UnixTime> promise) {
  if (round_id > last_known_round_id_) {
    last_known_round_id_ = round_id;
  }
  if (round_id < last_known_round_id_) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "too old"));
    return;
  }
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), round_id, block = block.clone(),
                                       promise = std::move(promise)](td::Result<ValidateCandidateResult> R) mutable {
    if (R.is_error()) {
      auto S = R.move_as_error();
      if (S.code() != ErrorCode::timeout && S.code() != ErrorCode::notready) {
        LOG(ERROR) << "failed to validate candidate: " << S;
      }
      delay_action(
          [SelfId, round_id, block = std::move(block), promise = std::move(promise)]() mutable {
            td::actor::send_closure(SelfId, &ValidatorGroup::validate_block_candidate, round_id, std::move(block),
                                    std::move(promise));
          },
          td::Timestamp::in(0.1));
    } else {
      auto v = R.move_as_ok();
      v.visit(td::overloaded([&](UnixTime ts) { promise.set_result(ts); },
                             [&](CandidateReject reject) {
                               promise.set_error(td::Status::Error(ErrorCode::protoviolation,
                                                                   PSTRING() << "bad candidate: " << reject.reason));
                             }));
    }
  });
  if (!started_) {
    P.set_error(td::Status::Error(ErrorCode::notready, "validator group not started"));
    return;
  }
  auto next_block_id = create_next_block_id(block.id.root_hash, block.id.file_hash);
  VLOG(VALIDATOR_DEBUG) << "validating block candidate " << next_block_id;
  block.id = next_block_id;
  run_validate_query(shard_, min_ts_, min_masterchain_block_id_, prev_block_ids_, std::move(block), validator_set_,
                     manager_, td::Timestamp::in(10.0), std::move(P));
}

void ValidatorGroup::accept_block_candidate(td::uint32 round_id, PublicKeyHash src, td::BufferSlice block_data,
                                            RootHash root_hash, FileHash file_hash,
                                            std::vector<BlockSignature> signatures,
                                            std::vector<BlockSignature> approve_signatures,
                                            validatorsession::ValidatorSessionStats stats,
                                            td::Promise<td::Unit> promise) {
  if (round_id >= last_known_round_id_) {
    last_known_round_id_ = round_id + 1;
  }
  auto sig_set = create_signature_set(std::move(signatures));
  validator_set_->check_signatures(root_hash, file_hash, sig_set).ensure();
  auto approve_sig_set = create_signature_set(std::move(approve_signatures));
  validator_set_->check_approve_signatures(root_hash, file_hash, approve_sig_set).ensure();

  if (!started_) {
    postponed_accept_.push_back(PostponedAccept{root_hash, file_hash, std::move(block_data), std::move(sig_set),
                                                std::move(approve_sig_set), std::move(stats), std::move(promise)});
    return;
  }
  auto next_block_id = create_next_block_id(root_hash, file_hash);
  td::actor::send_closure(manager_, &ValidatorManager::log_validator_session_stats, next_block_id, std::move(stats));
  auto block =
      block_data.size() > 0 ? create_block(next_block_id, std::move(block_data)).move_as_ok() : td::Ref<BlockData>{};

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), block_id = next_block_id, block, prev = prev_block_ids_,
                                       sig_set, approve_sig_set,
                                       promise = std::move(promise)](td::Result<td::Unit> R) mutable {
    if (R.is_error()) {
      if (R.error().code() == ErrorCode::cancelled) {
        promise.set_value(td::Unit());
        return;
      }
      LOG_CHECK(R.error().code() == ErrorCode::timeout || R.error().code() == ErrorCode::notready) << R.move_as_error();
      td::actor::send_closure(SelfId, &ValidatorGroup::retry_accept_block_query, block_id, std::move(block),
                              std::move(prev), std::move(sig_set), std::move(approve_sig_set), std::move(promise));
    } else {
      promise.set_value(R.move_as_ok());
    }
  });

  run_accept_block_query(next_block_id, std::move(block), prev_block_ids_, validator_set_, std::move(sig_set),
                         std::move(approve_sig_set), src == local_id_, manager_, std::move(P));
  prev_block_ids_ = std::vector<BlockIdExt>{next_block_id};
}

void ValidatorGroup::retry_accept_block_query(BlockIdExt block_id, td::Ref<BlockData> block,
                                              std::vector<BlockIdExt> prev, td::Ref<BlockSignatureSet> sig_set,
                                              td::Ref<BlockSignatureSet> approve_sig_set,
                                              td::Promise<td::Unit> promise) {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), block_id, block, prev, sig_set, approve_sig_set,
                                       promise = std::move(promise)](td::Result<td::Unit> R) mutable {
    if (R.is_error()) {
      LOG_CHECK(R.error().code() == ErrorCode::timeout) << R.move_as_error();
      td::actor::send_closure(SelfId, &ValidatorGroup::retry_accept_block_query, block_id, std::move(block),
                              std::move(prev), std::move(sig_set), std::move(approve_sig_set), std::move(promise));
    } else {
      promise.set_value(R.move_as_ok());
    }
  });

  run_accept_block_query(block_id, std::move(block), prev, validator_set_, std::move(sig_set),
                         std::move(approve_sig_set), false, manager_, std::move(P));
}

void ValidatorGroup::skip_round(td::uint32 round_id) {
  if (round_id >= last_known_round_id_) {
    last_known_round_id_ = round_id + 1;
  }
}

void ValidatorGroup::get_approved_candidate(PublicKey source, RootHash root_hash, FileHash file_hash,
                                            FileHash collated_data_file_hash, td::Promise<BlockCandidate> promise) {
  BlockIdExt id = create_next_block_id(root_hash, file_hash);

  td::actor::send_closure(manager_, &ValidatorManager::get_block_candidate_from_db, source, id, collated_data_file_hash,
                          std::move(promise));
}

BlockIdExt ValidatorGroup::create_next_block_id(RootHash root_hash, FileHash file_hash) const {
  BlockSeqno seqno = 0;
  for (auto &p : prev_block_ids_) {
    if (seqno < p.id.seqno) {
      seqno = p.id.seqno;
    }
  }
  return BlockIdExt{shard_.workchain, shard_.shard, seqno + 1, root_hash, file_hash};
}

std::unique_ptr<validatorsession::ValidatorSession::Callback> ValidatorGroup::make_validator_session_callback() {
  class Callback : public validatorsession::ValidatorSession::Callback {
   public:
    Callback(td::actor::ActorId<ValidatorGroup> id) : id_(id) {
    }
    void on_candidate(td::uint32 round, PublicKey source, validatorsession::ValidatorSessionRootHash root_hash,
                      td::BufferSlice data, td::BufferSlice collated_data,
                      td::Promise<validatorsession::ValidatorSession::CandidateDecision> promise) override {
      auto P = td::PromiseCreator::lambda([id = id_, promise = std::move(promise)](td::Result<td::uint32> R) mutable {
        if (R.is_ok()) {
          promise.set_value(validatorsession::ValidatorSession::CandidateDecision{R.move_as_ok()});
        } else {
          auto S = R.move_as_error();
          promise.set_value(
              validatorsession::ValidatorSession::CandidateDecision{S.message().c_str(), td::BufferSlice()});
        }
      });

      BlockCandidate candidate{Ed25519_PublicKey{source.ed25519_value().raw()},
                               BlockIdExt{0, 0, 0, root_hash, sha256_bits256(data.as_slice())},
                               sha256_bits256(collated_data.as_slice()), data.clone(), collated_data.clone()};

      td::actor::send_closure(id_, &ValidatorGroup::validate_block_candidate, round, std::move(candidate),
                              std::move(P));
    }
    void on_generate_slot(td::uint32 round, td::Promise<BlockCandidate> promise) override {
      td::actor::send_closure(id_, &ValidatorGroup::generate_block_candidate, round, std::move(promise));
    }
    void on_block_committed(td::uint32 round, PublicKey source, validatorsession::ValidatorSessionRootHash root_hash,
                            validatorsession::ValidatorSessionFileHash file_hash, td::BufferSlice data,
                            std::vector<std::pair<PublicKeyHash, td::BufferSlice>> signatures,
                            std::vector<std::pair<PublicKeyHash, td::BufferSlice>> approve_signatures,
                            validatorsession::ValidatorSessionStats stats) override {
      std::vector<BlockSignature> sigs;
      for (auto &sig : signatures) {
        sigs.emplace_back(BlockSignature{sig.first.bits256_value(), std::move(sig.second)});
      }
      std::vector<BlockSignature> approve_sigs;
      for (auto &sig : approve_signatures) {
        approve_sigs.emplace_back(BlockSignature{sig.first.bits256_value(), std::move(sig.second)});
      }
      auto P = td::PromiseCreator::lambda([](td::Result<td::Unit>) {});
      td::actor::send_closure(id_, &ValidatorGroup::accept_block_candidate, round, source.compute_short_id(),
                              std::move(data), root_hash, file_hash, std::move(sigs), std::move(approve_sigs),
                              std::move(stats), std::move(P));
    }
    void on_block_skipped(td::uint32 round) override {
      td::actor::send_closure(id_, &ValidatorGroup::skip_round, round);
    }
    void get_approved_candidate(PublicKey source, validatorsession::ValidatorSessionRootHash root_hash,
                                validatorsession::ValidatorSessionFileHash file_hash,
                                validatorsession::ValidatorSessionCollatedDataFileHash collated_data_file_hash,
                                td::Promise<BlockCandidate> promise) override {
      td::actor::send_closure(id_, &ValidatorGroup::get_approved_candidate, source, root_hash, file_hash,
                              collated_data_file_hash, std::move(promise));
    }

   private:
    td::actor::ActorId<ValidatorGroup> id_;
  };

  return std::make_unique<Callback>(actor_id(this));
}

void ValidatorGroup::create_session() {
  CHECK(!init_);
  init_ = true;
  std::vector<validatorsession::ValidatorSessionNode> vec;
  auto v = validator_set_->export_vector();
  bool found = false;
  for (auto &el : v) {
    validatorsession::ValidatorSessionNode n;
    n.pub_key = ValidatorFullId{el.key};
    n.weight = el.weight;
    if (n.pub_key.compute_short_id() == local_id_) {
      CHECK(!found);
      found = true;
      local_id_full_ = n.pub_key;
    }
    if (el.addr.is_zero()) {
      n.adnl_id = adnl::AdnlNodeIdShort{n.pub_key.compute_short_id()};
    } else {
      n.adnl_id = adnl::AdnlNodeIdShort{el.addr};
    }
    vec.emplace_back(std::move(n));
  }
  CHECK(found);

  if (!config_.new_catchain_ids) {
    session_ = validatorsession::ValidatorSession::create(session_id_, config_, local_id_, std::move(vec),
                                                          make_validator_session_callback(), keyring_, adnl_, rldp_,
                                                          overlays_, db_root_, "-", allow_unsafe_self_blocks_resync_);
  } else {
    session_ = validatorsession::ValidatorSession::create(
        session_id_, config_, local_id_, std::move(vec), make_validator_session_callback(), keyring_, adnl_, rldp_,
        overlays_, db_root_ + "/catchains/",
        PSTRING() << "." << shard_.workchain << "." << shard_.shard << "." << validator_set_->get_catchain_seqno()
                  << ".",
        allow_unsafe_self_blocks_resync_);
  }
  if (started_) {
    td::actor::send_closure(session_, &validatorsession::ValidatorSession::start);
  }
}

void ValidatorGroup::start(std::vector<BlockIdExt> prev, BlockIdExt min_masterchain_block_id, UnixTime min_ts) {
  prev_block_ids_ = prev;
  min_masterchain_block_id_ = min_masterchain_block_id;
  min_ts_ = min_ts;
  started_ = true;

  if (init_) {
    td::actor::send_closure(session_, &validatorsession::ValidatorSession::start);
  }

  for (auto &p : postponed_accept_) {
    auto next_block_id = create_next_block_id(p.root_hash, p.file_hash);
    td::actor::send_closure(manager_, &ValidatorManager::log_validator_session_stats, next_block_id,
                            std::move(p.stats));

    auto block =
        p.block.size() > 0 ? create_block(next_block_id, std::move(p.block)).move_as_ok() : td::Ref<BlockData>{};
    retry_accept_block_query(next_block_id, std::move(block), prev_block_ids_, std::move(p.sigs),
                             std::move(p.approve_sigs), std::move(p.promise));
    prev_block_ids_ = std::vector<BlockIdExt>{next_block_id};
  }
  postponed_accept_.clear();
}

void ValidatorGroup::destroy() {
  if (!session_.empty()) {
    auto ses = session_.release();
    delay_action([ses]() mutable { td::actor::send_closure(ses, &validatorsession::ValidatorSession::destroy); },
                 td::Timestamp::in(10.0));
  }
  stop();
}

}  // namespace validator

}  // namespace ton
