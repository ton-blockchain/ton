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
#include "full-node-master.hpp"
#include "ton/ton-io.hpp"
#include "td/utils/overloaded.h"
#include "common/delay.h"
#include "ton/lite-tl.hpp"

namespace ton {

namespace validator {

static bool need_send_candidate_broadcast(const validatorsession::BlockSourceInfo &source_info, bool is_masterchain) {
  return source_info.first_block_round == source_info.round && source_info.source_priority == 0 && !is_masterchain;
}

void ValidatorGroup::generate_block_candidate(
    validatorsession::BlockSourceInfo source_info,
    td::Promise<validatorsession::ValidatorSession::GeneratedCandidate> promise) {
  td::uint32 round_id = source_info.round;
  if (round_id > last_known_round_id_) {
    last_known_round_id_ = round_id;
  }
  if (!started_) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "cannot collate block: group not started"));
    return;
  }
  if (cached_collated_block_) {
    if (cached_collated_block_->result) {
      promise.set_value({cached_collated_block_->result.value().clone(), true});
    } else {
      cached_collated_block_->promises.push_back(promise.wrap([](BlockCandidate &&res) {
        return validatorsession::ValidatorSession::GeneratedCandidate{std::move(res), true};
      }));
    }
    return;
  }
  cached_collated_block_ = std::make_shared<CachedCollatedBlock>();
  cached_collated_block_->promises.push_back(promise.wrap([](BlockCandidate &&res) {
    return validatorsession::ValidatorSession::GeneratedCandidate{std::move(res), false};
  }));
  run_collate_query(
      shard_, min_masterchain_block_id_, prev_block_ids_, Ed25519_PublicKey{local_id_full_.ed25519_value().raw()},
      validator_set_, opts_->get_collator_options(), manager_, td::Timestamp::in(10.0),
      [SelfId = actor_id(this), cache = cached_collated_block_, source_info](td::Result<BlockCandidate> R) mutable {
        td::actor::send_closure(SelfId, &ValidatorGroup::generated_block_candidate, std::move(source_info),
                                std::move(cache), std::move(R));
      }, cancellation_token_source_.get_cancellation_token(), /* mode = */ 0);
}

void ValidatorGroup::generated_block_candidate(validatorsession::BlockSourceInfo source_info,
                                               std::shared_ptr<CachedCollatedBlock> cache,
                                               td::Result<BlockCandidate> R) {
  if (R.is_error()) {
    for (auto &p : cache->promises) {
      p.set_error(R.error().clone());
    }
    if (cache == cached_collated_block_) {
      cached_collated_block_ = nullptr;
    }
  } else {
    auto candidate = R.move_as_ok();
    add_available_block_candidate(candidate.pubkey.as_bits256(), candidate.id, candidate.collated_file_hash);
    if (need_send_candidate_broadcast(source_info, shard_.is_masterchain())) {
      send_block_candidate_broadcast(candidate.id, candidate.data.clone());
    }
    cache->result = std::move(candidate);
    for (auto &p : cache->promises) {
      p.set_value(cache->result.value().clone());
    }
  }
  cache->promises.clear();
}

void ValidatorGroup::validate_block_candidate(validatorsession::BlockSourceInfo source_info, BlockCandidate block,
                                              td::Promise<std::pair<UnixTime, bool>> promise) {
  td::uint32 round_id = source_info.round;
  if (round_id > last_known_round_id_) {
    last_known_round_id_ = round_id;
  }
  if (round_id < last_known_round_id_) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "too old"));
    return;
  }

  auto next_block_id = create_next_block_id(block.id.root_hash, block.id.file_hash);
  block.id = next_block_id;

  CacheKey cache_key = block_to_cache_key(block);
  auto it = approved_candidates_cache_.find(cache_key);
  if (it != approved_candidates_cache_.end()) {
    promise.set_value({it->second, true});
    return;
  }

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), source_info, block = block.clone(), manager = manager_,
                                       validator_set = validator_set_,
                                       promise = std::move(promise)](td::Result<ValidateCandidateResult> R) mutable {
    if (R.is_error()) {
      auto S = R.move_as_error();
      if (S.code() != ErrorCode::timeout && S.code() != ErrorCode::notready) {
        LOG(ERROR) << "failed to validate candidate: " << S;
      }
      delay_action(
          [SelfId, source_info, block = std::move(block), promise = std::move(promise)]() mutable {
            td::actor::send_closure(SelfId, &ValidatorGroup::validate_block_candidate, std::move(source_info),
                                    std::move(block), std::move(promise));
          },
          td::Timestamp::in(0.1));
    } else {
      auto v = R.move_as_ok();
      v.visit(td::overloaded(
          [&](UnixTime ts) {
            td::actor::send_closure(SelfId, &ValidatorGroup::update_approve_cache, block_to_cache_key(block), ts);
            td::actor::send_closure(SelfId, &ValidatorGroup::add_available_block_candidate, block.pubkey.as_bits256(),
                                    block.id, block.collated_file_hash);
            if (need_send_candidate_broadcast(source_info, block.id.is_masterchain())) {
              td::actor::send_closure(SelfId, &ValidatorGroup::send_block_candidate_broadcast, block.id,
                                      block.data.clone());
            }
            promise.set_value({ts, false});
          },
          [&](CandidateReject reject) {
            promise.set_error(
                td::Status::Error(ErrorCode::protoviolation, PSTRING() << "bad candidate: " << reject.reason));
          }));
    }
  });
  if (!started_) {
    P.set_error(td::Status::Error(ErrorCode::notready, "validator group not started"));
    return;
  }
  VLOG(VALIDATOR_DEBUG) << "validating block candidate " << next_block_id;
  block.id = next_block_id;
  run_validate_query(shard_, min_masterchain_block_id_, prev_block_ids_, std::move(block), validator_set_, manager_,
                     td::Timestamp::in(15.0), std::move(P));
}

void ValidatorGroup::update_approve_cache(CacheKey key, UnixTime value) {
  approved_candidates_cache_[key] = value;
}

void ValidatorGroup::accept_block_candidate(validatorsession::BlockSourceInfo source_info, td::BufferSlice block_data,
                                            RootHash root_hash, FileHash file_hash,
                                            std::vector<BlockSignature> signatures,
                                            std::vector<BlockSignature> approve_signatures,
                                            validatorsession::ValidatorSessionStats stats,
                                            td::Promise<td::Unit> promise) {
  stats.cc_seqno = validator_set_->get_catchain_seqno();
  td::uint32 round_id = source_info.round;
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
  LOG(WARNING) << "Accepted block " << next_block_id;
  td::actor::send_closure(manager_, &ValidatorManager::log_validator_session_stats, next_block_id, std::move(stats));
  auto block =
      block_data.size() > 0 ? create_block(next_block_id, std::move(block_data)).move_as_ok() : td::Ref<BlockData>{};

  // Creator of the block sends broadcast to public overlays
  // Creator of the block sends broadcast to private block overlay unless candidate broadcast was sent
  // Any node sends broadcast to custom overlays unless candidate broadcast was sent
  int send_broadcast_mode = 0;
  bool sent_candidate = sent_candidate_broadcasts_.contains(next_block_id);
  if (source_info.source.compute_short_id() == local_id_) {
    send_broadcast_mode |= fullnode::FullNode::broadcast_mode_public;
    if (!sent_candidate) {
      send_broadcast_mode |= fullnode::FullNode::broadcast_mode_private_block;
    }
  }
  if (!sent_candidate) {
    send_broadcast_mode |= fullnode::FullNode::broadcast_mode_custom;
  }

  auto P = td::PromiseCreator::lambda([=, SelfId = actor_id(this), block_id = next_block_id, prev = prev_block_ids_,
                                       promise = std::move(promise)](td::Result<td::Unit> R) mutable {
    if (R.is_error()) {
      if (R.error().code() == ErrorCode::cancelled) {
        promise.set_value(td::Unit());
        return;
      }
      LOG_CHECK(R.error().code() == ErrorCode::timeout || R.error().code() == ErrorCode::notready) << R.move_as_error();
      td::actor::send_closure(SelfId, &ValidatorGroup::retry_accept_block_query, block_id, std::move(block),
                              std::move(prev), std::move(sig_set), std::move(approve_sig_set), send_broadcast_mode,
                              std::move(promise));
    } else {
      promise.set_value(R.move_as_ok());
    }
  });

  run_accept_block_query(next_block_id, std::move(block), prev_block_ids_, validator_set_, std::move(sig_set),
                         std::move(approve_sig_set), send_broadcast_mode, manager_,
                         std::move(P));
  prev_block_ids_ = std::vector<BlockIdExt>{next_block_id};
  cached_collated_block_ = nullptr;
  approved_candidates_cache_.clear();
  cancellation_token_source_.cancel();
}

void ValidatorGroup::retry_accept_block_query(BlockIdExt block_id, td::Ref<BlockData> block,
                                              std::vector<BlockIdExt> prev, td::Ref<BlockSignatureSet> sig_set,
                                              td::Ref<BlockSignatureSet> approve_sig_set, int send_broadcast_mode,
                                              td::Promise<td::Unit> promise) {
  auto P = td::PromiseCreator::lambda(
      [=, SelfId = actor_id(this), promise = std::move(promise)](td::Result<td::Unit> R) mutable {
        if (R.is_error()) {
          LOG_CHECK(R.error().code() == ErrorCode::timeout) << R.move_as_error();
          td::actor::send_closure(SelfId, &ValidatorGroup::retry_accept_block_query, block_id, std::move(block),
                                  std::move(prev), std::move(sig_set), std::move(approve_sig_set), send_broadcast_mode,
                                  std::move(promise));
        } else {
          promise.set_value(R.move_as_ok());
        }
      });

  run_accept_block_query(block_id, std::move(block), prev, validator_set_, std::move(sig_set),
                         std::move(approve_sig_set), send_broadcast_mode, manager_, std::move(P));
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

BlockId ValidatorGroup::create_next_block_id_simple() const {
  BlockSeqno seqno = 0;
  for (auto &p : prev_block_ids_) {
    if (seqno < p.id.seqno) {
      seqno = p.id.seqno;
    }
  }
  return BlockId{shard_.workchain, shard_.shard, seqno + 1};
}

std::unique_ptr<validatorsession::ValidatorSession::Callback> ValidatorGroup::make_validator_session_callback() {
  class Callback : public validatorsession::ValidatorSession::Callback {
   public:
    Callback(td::actor::ActorId<ValidatorGroup> id) : id_(id) {
    }
    void on_candidate(validatorsession::BlockSourceInfo source_info,
                      validatorsession::ValidatorSessionRootHash root_hash, td::BufferSlice data,
                      td::BufferSlice collated_data,
                      td::Promise<validatorsession::ValidatorSession::CandidateDecision> promise) override {
      auto P =
          td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<std::pair<td::uint32, bool>> R) mutable {
            if (R.is_ok()) {
              validatorsession::ValidatorSession::CandidateDecision decision(R.ok().first);
              decision.set_is_cached(R.ok().second);
              promise.set_value(std::move(decision));
            } else {
              auto S = R.move_as_error();
              promise.set_value(
                  validatorsession::ValidatorSession::CandidateDecision{S.message().c_str(), td::BufferSlice()});
            }
          });

      BlockCandidate candidate{Ed25519_PublicKey{source_info.source.ed25519_value().raw()},
                               BlockIdExt{0, 0, 0, root_hash, sha256_bits256(data.as_slice())},
                               sha256_bits256(collated_data.as_slice()), data.clone(), collated_data.clone()};

      td::actor::send_closure(id_, &ValidatorGroup::validate_block_candidate, std::move(source_info),
                              std::move(candidate), std::move(P));
    }
    void on_generate_slot(validatorsession::BlockSourceInfo source_info,
                          td::Promise<validatorsession::ValidatorSession::GeneratedCandidate> promise) override {
      td::actor::send_closure(id_, &ValidatorGroup::generate_block_candidate, std::move(source_info),
                              std::move(promise));
    }
    void on_block_committed(validatorsession::BlockSourceInfo source_info, validatorsession::ValidatorSessionRootHash root_hash,
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
      td::actor::send_closure(id_, &ValidatorGroup::accept_block_candidate, std::move(source_info), std::move(data),
                              root_hash, file_hash, std::move(sigs), std::move(approve_sigs), std::move(stats),
                              std::move(P));
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

  config_.catchain_opts.broadcast_speed_multiplier = opts_->get_catchain_broadcast_speed_multiplier();
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
  double catchain_delay = opts_->get_catchain_max_block_delay() ? opts_->get_catchain_max_block_delay().value() : 0.4;
  double catchain_delay_slow =
      std::max(catchain_delay,
               opts_->get_catchain_max_block_delay_slow() ? opts_->get_catchain_max_block_delay_slow().value() : 1.0);
  td::actor::send_closure(session_, &validatorsession::ValidatorSession::set_catchain_max_block_delay, catchain_delay,
                          catchain_delay_slow);
  if (started_) {
    td::actor::send_closure(session_, &validatorsession::ValidatorSession::start);
  }
}

void ValidatorGroup::start(std::vector<BlockIdExt> prev, BlockIdExt min_masterchain_block_id) {
  prev_block_ids_ = prev;
  min_masterchain_block_id_ = min_masterchain_block_id;
  cached_collated_block_ = nullptr;
  approved_candidates_cache_.clear();
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
                             std::move(p.approve_sigs), 0, std::move(p.promise));
    prev_block_ids_ = std::vector<BlockIdExt>{next_block_id};
  }
  postponed_accept_.clear();

  validatorsession::NewValidatorGroupStats stats;
  stats.session_id = session_id_;
  stats.shard = shard_;
  stats.cc_seqno = validator_set_->get_catchain_seqno();
  stats.last_key_block_seqno = last_key_block_seqno_;
  stats.timestamp = td::Clocks::system();
  td::uint32 idx = 0;
  for (const auto& node : validator_set_->export_vector()) {
    PublicKeyHash id = ValidatorFullId{node.key}.compute_short_id();
    if (id == local_id_) {
      stats.self_idx = idx;
    }
    stats.nodes.push_back(validatorsession::NewValidatorGroupStats::Node{id, node.weight});
    ++idx;
  }
  td::actor::send_closure(manager_, &ValidatorManager::log_new_validator_group_stats, std::move(stats));
}

void ValidatorGroup::destroy() {
  if (!session_.empty()) {
    td::actor::send_closure(session_, &validatorsession::ValidatorSession::get_current_stats,
                            [manager = manager_, cc_seqno = validator_set_->get_catchain_seqno(),
                             block_id = create_next_block_id(RootHash::zero(), FileHash::zero())](
                                td::Result<validatorsession::ValidatorSessionStats> R) {
                              if (R.is_error()) {
                                LOG(WARNING) << "Failed to get validator session stats: " << R.move_as_error();
                                return;
                              }
                              auto stats = R.move_as_ok();
                              if (stats.rounds.empty()) {
                                return;
                              }
                              stats.cc_seqno = cc_seqno;
                              td::actor::send_closure(manager, &ValidatorManager::log_validator_session_stats, block_id,
                                                      std::move(stats));
                            });
    td::actor::send_closure(session_, &validatorsession::ValidatorSession::get_end_stats,
                            [manager = manager_](td::Result<validatorsession::EndValidatorGroupStats> R) {
                              if (R.is_error()) {
                                LOG(DEBUG) << "Failed to get validator session end stats: " << R.move_as_error();
                                return;
                              }
                              auto stats = R.move_as_ok();
                              td::actor::send_closure(manager, &ValidatorManager::log_end_validator_group_stats,
                                                      std::move(stats));
                            });
    auto ses = session_.release();
    delay_action([ses]() mutable { td::actor::send_closure(ses, &validatorsession::ValidatorSession::destroy); },
                 td::Timestamp::in(10.0));
  }
  cancellation_token_source_.cancel();
  stop();
}

void ValidatorGroup::get_validator_group_info_for_litequery(
    td::Promise<tl_object_ptr<lite_api::liteServer_nonfinal_validatorGroupInfo>> promise) {
  if (session_.empty()) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "not started"));
    return;
  }
  td::actor::send_closure(
      session_, &validatorsession::ValidatorSession::get_validator_group_info_for_litequery, last_known_round_id_,
      [SelfId = actor_id(this), promise = std::move(promise), round = last_known_round_id_](
          td::Result<std::vector<tl_object_ptr<lite_api::liteServer_nonfinal_candidateInfo>>> R) mutable {
        TRY_RESULT_PROMISE(promise, result, std::move(R));
        td::actor::send_closure(SelfId, &ValidatorGroup::get_validator_group_info_for_litequery_cont, round,
                                std::move(result), std::move(promise));
      });
}

void ValidatorGroup::get_validator_group_info_for_litequery_cont(
    td::uint32 expected_round, std::vector<tl_object_ptr<lite_api::liteServer_nonfinal_candidateInfo>> candidates,
    td::Promise<tl_object_ptr<lite_api::liteServer_nonfinal_validatorGroupInfo>> promise) {
  if (expected_round != last_known_round_id_) {
    candidates.clear();
  }

  BlockId next_block_id = create_next_block_id_simple();
  for (auto &candidate : candidates) {
    BlockIdExt id{next_block_id, candidate->id_->block_id_->root_hash_, candidate->id_->block_id_->file_hash_};
    candidate->id_->block_id_ = create_tl_lite_block_id(id);
    candidate->available_ =
        available_block_candidates_.count({candidate->id_->creator_, id, candidate->id_->collated_data_hash_});
  }

  auto result = create_tl_object<lite_api::liteServer_nonfinal_validatorGroupInfo>();
  result->next_block_id_ = create_tl_lite_block_id_simple(next_block_id);
  for (const BlockIdExt& prev : prev_block_ids_) {
    result->prev_.push_back(create_tl_lite_block_id(prev));
  }
  result->cc_seqno_ = validator_set_->get_catchain_seqno();
  result->candidates_ = std::move(candidates);
  promise.set_result(std::move(result));
}

}  // namespace validator

}  // namespace ton
