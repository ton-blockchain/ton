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
#include "collator-node/collator-node.hpp"
#include "common/delay.h"
#include "td/utils/Random.h"
#include "td/utils/overloaded.h"
#include "ton/lite-tl.hpp"
#include "ton/ton-io.hpp"

#include "fabric.h"
#include "full-node-master.hpp"
#include "validator-group.hpp"

namespace ton {

namespace validator {

static bool need_send_candidate_broadcast(const validatorsession::BlockSourceInfo &source_info, bool is_masterchain) {
  return source_info.priority.first_block_round == source_info.priority.round && source_info.priority.priority == 0 &&
         !is_masterchain;
}

void ValidatorGroup::generate_block_candidate(validatorsession::BlockSourceInfo source_info,
                                              td::Promise<GeneratedCandidate> promise) {
  if (destroying_) {
    promise.set_error(td::Status::Error("validator session finished"));
    return;
  }
  td::uint32 round_id = source_info.priority.round;
  update_round_id(round_id);
  if (!started_) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "cannot collate block: group not started"));
    return;
  }
  if (cached_collated_block_) {
    if (cached_collated_block_->result) {
      auto res = cached_collated_block_->result.value().clone();
      res.is_cached = true;
      promise.set_value(std::move(res));
    } else {
      cached_collated_block_->promises.push_back(promise.wrap([](GeneratedCandidate &&res) {
        res.is_cached = true;
        return std::move(res);
      }));
    }
    return;
  }
  cached_collated_block_ = std::make_shared<CachedCollatedBlock>();
  cached_collated_block_->promises.push_back(std::move(promise));
  td::Promise<GeneratedCandidate> P = [SelfId = actor_id(this), cache = cached_collated_block_,
                                       source_info](td::Result<GeneratedCandidate> R) {
    td::actor::send_closure(SelfId, &ValidatorGroup::generated_block_candidate, source_info, std::move(cache),
                            std::move(R));
  };

  if (optimistic_generation_ && prev_block_ids_.size() == 1 && optimistic_generation_->prev == prev_block_ids_[0] &&
      optimistic_generation_->round == round_id) {
    if (optimistic_generation_->result) {
      P.set_value(optimistic_generation_->result.value().clone());
    } else {
      optimistic_generation_->promises.push_back(
          [=, SelfId = actor_id(this), P = std::move(P),
           cancellation_token =
               cancellation_token_source_.get_cancellation_token()](td::Result<GeneratedCandidate> R) mutable {
            if (R.is_error()) {
              td::actor::send_closure(SelfId, &ValidatorGroup::generate_block_candidate_cont, source_info, std::move(P),
                                      std::move(cancellation_token));
            } else {
              P.set_value(R.move_as_ok());
            }
          });
    }
    return;
  }
  generate_block_candidate_cont(source_info, std::move(P), cancellation_token_source_.get_cancellation_token());
}

void ValidatorGroup::generate_block_candidate_cont(validatorsession::BlockSourceInfo source_info,
                                                   td::Promise<GeneratedCandidate> promise,
                                                   td::CancellationToken cancellation_token) {
  TRY_STATUS_PROMISE(promise, cancellation_token.check());
  td::uint64 max_answer_size = config_.max_block_size + config_.max_collated_data_size + 1024;
  td::actor::send_closure(collation_manager_, &CollationManager::collate_block, shard_, min_masterchain_block_id_,
                          prev_block_ids_, Ed25519_PublicKey{local_id_full_.ed25519_value().raw()},
                          source_info.priority, validator_set_, max_answer_size, std::move(cancellation_token),
                          std::move(promise), config_.proto_version);
}

void ValidatorGroup::generated_block_candidate(validatorsession::BlockSourceInfo source_info,
                                               std::shared_ptr<CachedCollatedBlock> cache,
                                               td::Result<GeneratedCandidate> R) {
  if (R.is_error()) {
    for (auto &p : cache->promises) {
      p.set_error(R.error().clone());
    }
    if (cache == cached_collated_block_) {
      cached_collated_block_ = nullptr;
    }
  } else {
    auto c = R.move_as_ok();
    add_available_block_candidate(c.candidate.pubkey.as_bits256(), c.candidate.id, c.candidate.collated_file_hash);
    if (need_send_candidate_broadcast(source_info, shard_.is_masterchain())) {
      send_block_candidate_broadcast(c.candidate.id, c.candidate.data.clone());
    }
    if (!c.self_collated) {
      block_collator_node_id_[c.candidate.id] = adnl::AdnlNodeIdShort{c.collator_node_id};
    }
    cache->result = std::move(c);
    for (auto &p : cache->promises) {
      p.set_value(cache->result.value().clone());
    }
  }
  cache->promises.clear();
}

void ValidatorGroup::validate_block_candidate(validatorsession::BlockSourceInfo source_info, BlockCandidate block,
                                              td::Promise<std::pair<UnixTime, bool>> promise,
                                              td::optional<BlockCandidate> optimistic_prev_block) {
  if (destroying_) {
    promise.set_error(td::Status::Error("validator session finished"));
    return;
  }
  bool is_optimistic = (bool)optimistic_prev_block;
  if (is_optimistic && shard_.is_masterchain()) {
    promise.set_error(td::Status::Error("no optimistic validation in masterchain"));
    return;
  }
  td::uint32 round_id = source_info.priority.round;
  if (!is_optimistic) {
    update_round_id(round_id);
  }
  if (round_id < last_known_round_id_) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "too old"));
    return;
  }
  if (is_optimistic && round_id > last_known_round_id_ + 1) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "too new"));
    return;
  }
  if (is_optimistic && shard_.is_masterchain()) {
    promise.set_error(td::Status::Error("optimistic validation in masterchain is not supported"));
    return;
  }

  auto next_block_id = create_next_block_id(block.id.root_hash, block.id.file_hash);
  block.id = next_block_id;
  auto prev = prev_block_ids_;
  if (is_optimistic) {
    if (round_id > last_known_round_id_) {
      ++block.id.id.seqno;
    }
    optimistic_prev_block.value().id.id = block.id.id;
    --optimistic_prev_block.value().id.id.seqno;
    if (round_id == last_known_round_id_) {
      if (prev_block_ids_ != std::vector<BlockIdExt>{optimistic_prev_block.value().id}) {
        promise.set_error(td::Status::Error("wrong prev block for optimistic validation"));
        return;
      }
      optimistic_prev_block = {};
      is_optimistic = false;
    } else {
      prev = {optimistic_prev_block.value().id};
    }
  }

  CacheKey cache_key = block_to_cache_key(block);
  auto it = approved_candidates_cache_.find(cache_key);
  if (it != approved_candidates_cache_.end()) {
    promise.set_value({it->second, true});
    return;
  }

  auto it2 = block_collator_node_id_.find(block.id);
  adnl::AdnlNodeIdShort collator_node_id =
      it2 == block_collator_node_id_.end() ? adnl::AdnlNodeIdShort::zero() : it2->second;

  auto P = td::PromiseCreator::lambda(
      [=, SelfId = actor_id(this), block = block.clone(),
       optimistic_prev_block = is_optimistic ? optimistic_prev_block.value().clone() : td::optional<BlockCandidate>{},
       promise = std::move(promise),
       collation_manager = collation_manager_](td::Result<ValidateCandidateResult> R) mutable {
        if (R.is_error()) {
          auto S = R.move_as_error();
          if (S.code() != ErrorCode::timeout && S.code() != ErrorCode::notready) {
            LOG(ERROR) << "failed to validate candidate: " << S;
          }
          delay_action(
              [SelfId, source_info, block = std::move(block), promise = std::move(promise),
               optimistic_prev_block = std::move(optimistic_prev_block)]() mutable {
                td::actor::send_closure(SelfId, &ValidatorGroup::validate_block_candidate, std::move(source_info),
                                        std::move(block), std::move(promise), std::move(optimistic_prev_block));
              },
              td::Timestamp::in(0.1));
        } else {
          auto v = R.move_as_ok();
          v.visit(td::overloaded(
              [&](UnixTime ts) {
                td::actor::send_closure(SelfId, &ValidatorGroup::update_approve_cache, block_to_cache_key(block), ts);
                td::actor::send_closure(SelfId, &ValidatorGroup::add_available_block_candidate,
                                        block.pubkey.as_bits256(), block.id, block.collated_file_hash);
                if (need_send_candidate_broadcast(source_info, block.id.is_masterchain())) {
                  td::actor::send_closure(SelfId, &ValidatorGroup::send_block_candidate_broadcast, block.id,
                                          block.data.clone());
                }
                promise.set_value({ts, false});
              },
              [&](CandidateReject reject) {
                if (!collator_node_id.is_zero()) {
                  td::actor::send_closure(collation_manager, &CollationManager::ban_collator, collator_node_id,
                                          PSTRING() << "bad candidate " << block.id.to_str() << " : " << reject.reason);
                }
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
  td::Ref<BlockData> optimistic_prev_block_data;
  if (is_optimistic) {
    TRY_RESULT_PROMISE_PREFIX_ASSIGN(
        P, optimistic_prev_block_data,
        create_block(optimistic_prev_block.value().id, std::move(optimistic_prev_block.value().data)),
        "failed to parse optimistic prev block: ");
  }
  run_validate_query(std::move(block),
                     ValidateParams{.shard = shard_,
                                    .min_masterchain_block_id = min_masterchain_block_id_,
                                    .prev = std::move(prev),
                                    .validator_set = validator_set_,
                                    .local_validator_id = local_id_,
                                    .optimistic_prev_block = optimistic_prev_block_data,
                                    .parallel_validation = opts_.get()->get_parallel_validation()},
                     manager_, td::Timestamp::in(15.0), std::move(P));
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
  td::uint32 round_id = source_info.priority.round;
  update_round_id(round_id + 1);
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
  LOG(WARNING) << "Accepted block " << next_block_id.to_str();
  stats.block_id = next_block_id;
  td::actor::send_closure(manager_, &ValidatorManager::log_validator_session_stats, std::move(stats));
  auto block =
      block_data.size() > 0 ? create_block(next_block_id, std::move(block_data)).move_as_ok() : td::Ref<BlockData>{};

  // OLD BROADCAST BEHAVIOR:
  // Creator of the block sends broadcast to public overlays
  // Creator of the block sends broadcast to private block overlay unless candidate broadcast was sent
  // Any node sends broadcast to custom overlays unless candidate broadcast was sent
  int send_broadcast_mode = 0;
  bool sent_candidate = sent_candidate_broadcasts_.contains(next_block_id);
  if (source_info.source.compute_short_id() == local_id_) {
    send_broadcast_mode |= fullnode::FullNode::broadcast_mode_public;
    if (!sent_candidate) {
      send_broadcast_mode |= fullnode::FullNode::broadcast_mode_fast_sync;
    }
  }
  if (!sent_candidate) {
    send_broadcast_mode |= fullnode::FullNode::broadcast_mode_custom;
  }
  // NEW BROADCAST BEHAVIOR (activate later):
  // Masterchain block are broadcasted as Block Broadcast (with signatures). Shard blocks are broadcasted as Block Candidate Broadcast (only block data).
  // Public and private overlays: creator sends masterchain blocks, all validators send shard blocks.
  // Custom overlays: all nodes send all blocks.
  // If the block was broadcasted earlier as a candidate (to private and custom overlays), the broadcast is not repeated.
  /*int send_broadcast_mode = 0;
  bool sent_candidate = sent_candidate_broadcasts_.contains(next_block_id);
  if (!shard_.is_masterchain() || source_info.source.compute_short_id() == local_id_) {
    send_broadcast_mode |= fullnode::FullNode::broadcast_mode_public;
    if (!sent_candidate) {
      send_broadcast_mode |= fullnode::FullNode::broadcast_mode_fast_sync;
    }
  }
  if (!sent_candidate) {
    send_broadcast_mode |= fullnode::FullNode::broadcast_mode_custom;
  }*/
  accept_block_query(next_block_id, std::move(block), std::move(prev_block_ids_), std::move(sig_set),
                     std::move(approve_sig_set), send_broadcast_mode, std::move(promise));
  prev_block_ids_ = std::vector<BlockIdExt>{next_block_id};
  cached_collated_block_ = nullptr;
  cancellation_token_source_.cancel();
  if (optimistic_generation_ && optimistic_generation_->round == last_known_round_id_ &&
      optimistic_generation_->prev != next_block_id) {
    optimistic_generation_ = {};
  }
}

void ValidatorGroup::accept_block_query(BlockIdExt block_id, td::Ref<BlockData> block, std::vector<BlockIdExt> prev,
                                        td::Ref<BlockSignatureSet> sig_set, td::Ref<BlockSignatureSet> approve_sig_set,
                                        int send_broadcast_mode, td::Promise<td::Unit> promise, bool is_retry) {
  auto P = td::PromiseCreator::lambda([=, SelfId = actor_id(this),
                                       promise = std::move(promise)](td::Result<td::Unit> R) mutable {
    if (R.is_error()) {
      if (R.error().code() == ErrorCode::cancelled) {
        promise.set_value(td::Unit());
        return;
      }
      LOG_CHECK(R.error().code() == ErrorCode::timeout || R.error().code() == ErrorCode::notready) << R.move_as_error();
      td::actor::send_closure(SelfId, &ValidatorGroup::accept_block_query, block_id, std::move(block), std::move(prev),
                              std::move(sig_set), std::move(approve_sig_set), send_broadcast_mode, std::move(promise),
                              true);
    } else {
      promise.set_value(R.move_as_ok());
    }
  });

  run_accept_block_query(block_id, std::move(block), std::move(prev), validator_set_, std::move(sig_set),
                         std::move(approve_sig_set), send_broadcast_mode, monitoring_shard_, manager_, std::move(P));
}

void ValidatorGroup::skip_round(td::uint32 round_id) {
  update_round_id(round_id + 1);
}

void ValidatorGroup::get_approved_candidate(PublicKey source, RootHash root_hash, FileHash file_hash,
                                            FileHash collated_data_file_hash, td::Promise<BlockCandidate> promise) {
  BlockIdExt id = create_next_block_id(root_hash, file_hash);

  td::actor::send_closure(manager_, &ValidatorManager::get_block_candidate_from_db, source, id, collated_data_file_hash,
                          std::move(promise));
}

void ValidatorGroup::generate_block_optimistic(validatorsession::BlockSourceInfo source_info,
                                               td::BufferSlice prev_block, RootHash prev_root_hash,
                                               FileHash prev_file_hash, td::Promise<GeneratedCandidate> promise) {
  if (destroying_) {
    promise.set_error(td::Status::Error("validator session finished"));
    return;
  }
  if (shard_.is_masterchain()) {
    promise.set_error(td::Status::Error("no optimistic generation in masterchain"));
    return;
  }
  if (last_known_round_id_ + 1 != source_info.priority.round) {
    promise.set_error(td::Status::Error("too old round"));
    return;
  }
  if (optimistic_generation_ && optimistic_generation_->round >= source_info.priority.round) {
    promise.set_error(td::Status::Error("optimistic generation already in progress"));
    return;
  }
  BlockIdExt block_id{create_next_block_id_simple(), prev_root_hash, prev_file_hash};
  optimistic_generation_ = std::make_unique<OptimisticGeneration>();
  optimistic_generation_->round = source_info.priority.round;
  optimistic_generation_->prev = BlockIdExt{create_next_block_id_simple(), prev_root_hash, prev_file_hash};
  optimistic_generation_->promises.push_back(std::move(promise));

  td::Promise<GeneratedCandidate> P = [=, SelfId = actor_id(this)](td::Result<GeneratedCandidate> R) {
    td::actor::send_closure(SelfId, &ValidatorGroup::generated_block_optimistic, source_info, std::move(R));
  };
  LOG(WARNING) << "Optimistically generating next block after " << block_id.to_str();
  td::uint64 max_answer_size = config_.max_block_size + config_.max_collated_data_size + 1024;
  td::actor::send_closure(collation_manager_, &CollationManager::collate_block_optimistic, shard_,
                          min_masterchain_block_id_, block_id, std::move(prev_block),
                          Ed25519_PublicKey{local_id_full_.ed25519_value().raw()}, source_info.priority, validator_set_,
                          max_answer_size, optimistic_generation_->cancellation_token_source.get_cancellation_token(),
                          std::move(P), config_.proto_version);
}

void ValidatorGroup::generated_block_optimistic(validatorsession::BlockSourceInfo source_info,
                                                td::Result<GeneratedCandidate> R) {
  if (!optimistic_generation_ || optimistic_generation_->round != source_info.priority.round) {
    return;
  }
  if (R.is_error()) {
    LOG(WARNING) << "Optimistic generation failed: " << R.move_as_error();
    for (auto &promise : optimistic_generation_->promises) {
      promise.set_error(R.error().clone());
    }
    optimistic_generation_ = {};
    return;
  }
  GeneratedCandidate c = R.move_as_ok();
  if (!c.self_collated) {
    block_collator_node_id_[c.candidate.id] = adnl::AdnlNodeIdShort{c.collator_node_id};
  }
  optimistic_generation_->result = std::move(c);
  for (auto &promise : optimistic_generation_->promises) {
    promise.set_result(optimistic_generation_->result.value().clone());
  }
  optimistic_generation_->promises.clear();
}

void ValidatorGroup::update_round_id(td::uint32 round) {
  if (last_known_round_id_ >= round) {
    return;
  }
  last_known_round_id_ = round;
  if (optimistic_generation_ && optimistic_generation_->round < round) {
    optimistic_generation_ = {};
  }
}

BlockIdExt ValidatorGroup::create_next_block_id(RootHash root_hash, FileHash file_hash) const {
  return BlockIdExt{create_next_block_id_simple(), root_hash, file_hash};
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
                              std::move(candidate), std::move(P), td::optional<BlockCandidate>{});
    }
    void on_generate_slot(validatorsession::BlockSourceInfo source_info,
                          td::Promise<GeneratedCandidate> promise) override {
      td::actor::send_closure(id_, &ValidatorGroup::generate_block_candidate, std::move(source_info),
                              std::move(promise));
    }
    void on_block_committed(validatorsession::BlockSourceInfo source_info,
                            validatorsession::ValidatorSessionRootHash root_hash,
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
    void generate_block_optimistic(validatorsession::BlockSourceInfo source_info, td::BufferSlice prev_block,
                                   RootHash prev_root_hash, FileHash prev_file_hash,
                                   td::Promise<GeneratedCandidate> promise) override {
      td::actor::send_closure(id_, &ValidatorGroup::generate_block_optimistic, source_info, std::move(prev_block),
                              prev_root_hash, prev_file_hash, std::move(promise));
    }
    void on_optimistic_candidate(validatorsession::BlockSourceInfo source_info,
                                 validatorsession::ValidatorSessionRootHash root_hash, td::BufferSlice data,
                                 td::BufferSlice collated_data, PublicKey prev_source,
                                 validatorsession::ValidatorSessionRootHash prev_root_hash, td::BufferSlice prev_data,
                                 td::BufferSlice prev_collated_data) override {
      BlockCandidate candidate{Ed25519_PublicKey{source_info.source.ed25519_value().raw()},
                               BlockIdExt{0, 0, 0, root_hash, sha256_bits256(data.as_slice())},
                               sha256_bits256(collated_data.as_slice()), data.clone(), collated_data.clone()};
      BlockCandidate prev_candidate{Ed25519_PublicKey{prev_source.ed25519_value().raw()},
                                    BlockIdExt{0, 0, 0, prev_root_hash, sha256_bits256(prev_data.as_slice())},
                                    sha256_bits256(prev_collated_data.as_slice()), prev_data.clone(),
                                    prev_collated_data.clone()};

      td::actor::send_closure(
          id_, &ValidatorGroup::validate_block_candidate, std::move(source_info), std::move(candidate),
          [](td::Result<std::pair<td::uint32, bool>>) mutable {}, std::move(prev_candidate));
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
    if (el.addr.is_zero()) {
      n.adnl_id = adnl::AdnlNodeIdShort{n.pub_key.compute_short_id()};
    } else {
      n.adnl_id = adnl::AdnlNodeIdShort{el.addr};
    }
    if (n.pub_key.compute_short_id() == local_id_) {
      CHECK(!found);
      found = true;
      local_id_full_ = n.pub_key;
      local_adnl_id_ = n.adnl_id;
    }
    vec.emplace_back(std::move(n));
  }
  CHECK(found);

  td::actor::send_closure(rldp_, &rldp::Rldp::add_id, local_adnl_id_);
  td::actor::send_closure(rldp2_, &rldp2::Rldp::add_id, local_adnl_id_);

  config_.catchain_opts.broadcast_speed_multiplier = opts_->get_catchain_broadcast_speed_multiplier();
  if (!config_.new_catchain_ids) {
    session_ = validatorsession::ValidatorSession::create(session_id_, config_, local_id_, std::move(vec),
                                                          make_validator_session_callback(), keyring_, adnl_, rldp2_,
                                                          overlays_, db_root_, "-", allow_unsafe_self_blocks_resync_);
  } else {
    session_ = validatorsession::ValidatorSession::create(
        session_id_, config_, local_id_, std::move(vec), make_validator_session_callback(), keyring_, adnl_, rldp2_,
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
  started_ = true;

  if (init_) {
    td::actor::send_closure(session_, &validatorsession::ValidatorSession::start);
  }

  for (auto &p : postponed_accept_) {
    auto next_block_id = create_next_block_id(p.root_hash, p.file_hash);
    p.stats.block_id = next_block_id;
    td::actor::send_closure(manager_, &ValidatorManager::log_validator_session_stats, std::move(p.stats));

    auto block =
        p.block.size() > 0 ? create_block(next_block_id, std::move(p.block)).move_as_ok() : td::Ref<BlockData>{};
    accept_block_query(next_block_id, std::move(block), std::move(prev_block_ids_), std::move(p.sigs),
                       std::move(p.approve_sigs), 0, std::move(p.promise));
    prev_block_ids_ = std::vector<BlockIdExt>{next_block_id};
  }
  postponed_accept_.clear();

  validatorsession::NewValidatorGroupStats stats{.session_id = session_id_,
                                                 .shard = shard_,
                                                 .cc_seqno = validator_set_->get_catchain_seqno(),
                                                 .last_key_block_seqno = last_key_block_seqno_,
                                                 .started_at = td::Clocks::system(),
                                                 .prev = prev,
                                                 .self = local_id_};
  td::uint32 idx = 0;
  for (const auto &node : validator_set_->export_vector()) {
    PublicKeyHash id = ValidatorFullId{node.key}.compute_short_id();
    if (id == local_id_) {
      stats.self_idx = idx;
    }
    stats.nodes.push_back(validatorsession::NewValidatorGroupStats::Node{
        .id = id,
        .pubkey = PublicKey(pubkeys::Ed25519(node.key)),
        .adnl_id = (node.addr.is_zero() ? adnl::AdnlNodeIdShort{id} : adnl::AdnlNodeIdShort{node.addr}),
        .weight = node.weight});
    ++idx;
  }
  td::actor::send_closure(manager_, &ValidatorManager::log_new_validator_group_stats, std::move(stats));
}

void ValidatorGroup::destroy() {
  if (destroying_) {
    return;
  }
  destroying_ = true;
  if (!session_.empty()) {
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
  }
  cancellation_token_source_.cancel();
  delay_action([SelfId = actor_id(this)]() { td::actor::send_closure(SelfId, &ValidatorGroup::destroy_cont); },
               td::Timestamp::in(10.0));
}

void ValidatorGroup::destroy_cont() {
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
                              stats.block_id = block_id;
                              td::actor::send_closure(manager, &ValidatorManager::log_validator_session_stats,
                                                      std::move(stats));
                            });
    auto ses = session_.release();
    td::actor::send_closure(ses, &validatorsession::ValidatorSession::destroy);
  }
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
        available_block_candidates_.contains({candidate->id_->creator_, id, candidate->id_->collated_data_hash_});
  }

  auto result = create_tl_object<lite_api::liteServer_nonfinal_validatorGroupInfo>();
  result->next_block_id_ = create_tl_lite_block_id_simple(next_block_id);
  for (const BlockIdExt &prev : prev_block_ids_) {
    result->prev_.push_back(create_tl_lite_block_id(prev));
  }
  result->cc_seqno_ = validator_set_->get_catchain_seqno();
  result->candidates_ = std::move(candidates);
  promise.set_result(std::move(result));
}

void ValidatorGroup::send_block_candidate_broadcast(BlockIdExt id, td::BufferSlice data) {
  if (sent_candidate_broadcasts_.insert(id).second) {
    td::actor::send_closure(manager_, &ValidatorManager::send_block_candidate_broadcast, id,
                            validator_set_->get_catchain_seqno(), validator_set_->get_validator_set_hash(),
                            std::move(data),
                            fullnode::FullNode::broadcast_mode_fast_sync | fullnode::FullNode::broadcast_mode_custom);
  }
}

}  // namespace validator

}  // namespace ton
