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
#include "validator-session.hpp"
#include "td/utils/Random.h"
#include "td/utils/crypto.h"
#include "candidate-serializer.h"
#include "td/utils/overloaded.h"
#include "ton/ton-tl.hpp"

namespace ton {

namespace validatorsession {

void ValidatorSessionImpl::process_blocks(std::vector<catchain::CatChainBlock *> blocks) {
  VLOG(VALIDATOR_SESSION_DEBUG) << this << ": processing " << blocks.size() << " blocks";
  requested_new_block_ = false;
  requested_new_block_now_ = false;

  for (auto block : blocks) {
    auto e = dynamic_cast<const BlockExtra *>(block->extra());
    CHECK(e != nullptr);
    real_state_ = ValidatorSessionState::merge(description(), real_state_, e->get_ref());
  }

  if (real_state_->cur_round_seqno() != cur_round_) {
    on_new_round(real_state_->cur_round_seqno());
  }

  [[maybe_unused]] td::uint32 cnt = 0;
  auto ts = description().get_ts();
  auto att = description().get_attempt_seqno(ts);
  std::vector<tl_object_ptr<ton_api::validatorSession_round_Message>> msgs;

  if (generated_ && !sent_generated_) {
    auto it = blocks_.find(generated_block_);
    CHECK(it != blocks_.end());

    auto &B = it->second;
    auto file_hash = sha256_bits256(B->data_);
    auto collated_data_file_hash = sha256_bits256(B->collated_data_);
    msgs.emplace_back(create_tl_object<ton_api::validatorSession_message_submittedBlock>(
        cur_round_, B->root_hash_, file_hash, collated_data_file_hash));
    cnt++;
    sent_generated_ = true;
  }

  auto to_approve = real_state_->choose_blocks_to_approve(description(), local_idx());
  for (auto &block : to_approve) {
    auto id = SentBlock::get_block_id(block);
    if (approved_.count(id) && approved_[id].first <= td::Clocks::system()) {
      msgs.emplace_back(create_tl_object<ton_api::validatorSession_message_approvedBlock>(
          cur_round_, id, approved_[id].second.clone()));
      cnt++;
    }
  }
  for (auto &B : pending_reject_) {
    msgs.emplace_back(
        create_tl_object<ton_api::validatorSession_message_rejectedBlock>(cur_round_, B.first, std::move(B.second)));
  }
  pending_reject_.clear();

  if (signed_) {
    bool found = false;
    auto B = real_state_->choose_block_to_sign(description(), local_idx(), found);
    if (found) {
      CHECK(SentBlock::get_block_id(B) == signed_block_);
      msgs.emplace_back(
          create_tl_object<ton_api::validatorSession_message_commit>(cur_round_, signed_block_, std::move(signature_)));
      cnt++;
    }
  }

  for (auto &msg : msgs) {
    VLOG(VALIDATOR_SESSION_INFO) << this << ": applying action: " << msg.get();
    stats_process_action(local_idx(), *msg);
    real_state_ = ValidatorSessionState::action(description(), real_state_, local_idx(), att, msg.get());
  }

  if (real_state_->check_need_generate_vote_for(description(), local_idx(), att)) {
    VLOG(VALIDATOR_SESSION_INFO) << this << ": generating VOTEFOR";
    auto msg = real_state_->generate_vote_for(description(), local_idx(), att);
    CHECK(msg);
    real_state_ = ValidatorSessionState::action(description(), real_state_, local_idx(), att, msg.get());
    msgs.push_back(std::move(msg));
  }

  while (true) {
    auto msg = real_state_->create_action(description(), local_idx(), att);
    bool stop = false;
    if (msg->get_id() == ton_api::validatorSession_message_empty::ID) {
      stop = true;
    }
    VLOG(VALIDATOR_SESSION_INFO) << this << ": applying action: " << msg.get();
    real_state_ = ValidatorSessionState::action(description(), real_state_, local_idx(), att, msg.get());
    msgs.emplace_back(std::move(msg));
    cnt++;
    if (stop) {
      break;
    }
  }

  real_state_ = ValidatorSessionState::move_to_persistent(description(), real_state_);

  VLOG(VALIDATOR_SESSION_DEBUG) << this << ": created block: root_hash=" << real_state_->get_hash(description());

  auto payload = create_tl_object<ton_api::validatorSession_blockUpdate>(ts, std::move(msgs),
                                                                         real_state_->get_hash(description()));
  td::actor::send_closure(catchain_, &catchain::CatChain::processed_block, serialize_tl_object(payload, true));

  auto round = real_state_->cur_round_seqno();
  if (round > cur_round_) {
    on_new_round(round);
  }

  virtual_state_ = ValidatorSessionState::merge(description(), virtual_state_, real_state_);
  virtual_state_ = ValidatorSessionState::move_to_persistent(description(), virtual_state_);
  description().clear_temp_memory();
}

void ValidatorSessionImpl::finished_processing() {
  if (virtual_state_->get_hash(description()) != real_state_->get_hash(description())) {
    VLOG(VALIDATOR_SESSION_WARNING) << this << ": hash mismatch (maybe some node blamed)";
  }
  virtual_state_ = real_state_;
  check_all();
}

void ValidatorSessionImpl::preprocess_block(catchain::CatChainBlock *block) {
  auto start_time = td::Timestamp::now();
  td::PerfWarningTimer p_timer{"Loong block preprocess", 0.1};
  td::PerfWarningTimer q_timer{"Looong block preprocess", 0.1};

  auto prev = block->prev();
  const ValidatorSessionState *state;
  if (prev) {
    auto e = dynamic_cast<const BlockExtra *>(prev->extra());
    CHECK(e != nullptr);
    state = e->get_ref();
  } else {
    state = ValidatorSessionState::create(description());
  }
  auto deps = block->deps();
  for (auto b : deps) {
    auto e = dynamic_cast<const BlockExtra *>(b->extra());
    CHECK(e != nullptr);
    state = ValidatorSessionState::merge(description(), state, e->get_ref());
  }

  if (block->payload().size() != 0 || deps.size() != 0) {
    auto R = fetch_tl_object<ton_api::validatorSession_blockUpdate>(block->payload().clone(), true);
    if (!R.is_error()) {
      auto B = R.move_as_ok();
      auto att = description().get_attempt_seqno(B->ts_);
      for (auto &msg : B->actions_) {
        VLOG(VALIDATOR_SESSION_INFO) << this << "[node " << description().get_source_id(block->source()) << "][block "
                                     << block->hash() << "]: applying action " << msg.get();
        stats_process_action(block->source(), *msg);
        state = ValidatorSessionState::action(description(), state, block->source(), att, msg.get());
      }
      state = ValidatorSessionState::make_all(description(), state, block->source(), att);
      if (state->get_hash(description()) != static_cast<td::uint32>(B->state_)) {
        VLOG(VALIDATOR_SESSION_WARNING) << this << "[node " << description().get_source_id(block->source())
                                        << "][block " << block->hash()
                                        << "]: state hash mismatch: computed=" << state->get_hash(description())
                                        << " received=" << B->state_;
        for (auto &msg : B->actions_) {
          VLOG(VALIDATOR_SESSION_WARNING) << this << "[node " << description().get_source_id(block->source())
                                          << "][block " << block->hash() << "]: applied action " << msg.get();
        }
      }
    } else {
      VLOG(VALIDATOR_SESSION_WARNING) << this << "[node " << description().get_source_id(block->source()) << "][block "
                                      << block->hash() << "]: failed to parse: " << R.move_as_error();
      state = ValidatorSessionState::make_all(description(), state, block->source(), state->get_ts(block->source()));
    }
  }
  q_timer.reset();
  state = ValidatorSessionState::move_to_persistent(description(), state);
  block->set_extra(std::make_unique<BlockExtra>(state));
  if (block->source() == local_idx() && !catchain_started_) {
    real_state_ = state;
  }
  virtual_state_ = ValidatorSessionState::merge(description(), virtual_state_, state);
  virtual_state_ = ValidatorSessionState::move_to_persistent(description(), virtual_state_);
  description().clear_temp_memory();
  if (real_state_->cur_round_seqno() != cur_round_) {
    on_new_round(real_state_->cur_round_seqno());
  }
  check_all();
  VLOG(VALIDATOR_SESSION_DEBUG) << this << ": preprocessed block " << block->hash() << " in "
                                << static_cast<td::uint32>(1000 * (td::Timestamp::now().at() - start_time.at()))
                                << "ms: state=" << state->get_hash(description());
}

bool ValidatorSessionImpl::ensure_candidate_unique(td::uint32 src_idx, td::uint32 round,
                                                  ValidatorSessionCandidateId block_id) {
  auto it = src_round_candidate_[src_idx].find(round);
  if (it != src_round_candidate_[src_idx].end() && it->second != block_id) {
    VLOG(VALIDATOR_SESSION_WARNING) << this << "[node " << description_->get_source_adnl_id(src_idx) << "][candidate "
                                    << block_id << "]: this node already has candidate in round " << round;
    return false;
  }
  src_round_candidate_[src_idx][round] = block_id;
  return true;
}

void ValidatorSessionImpl::process_broadcast(PublicKeyHash src, td::BufferSlice data,
                                             td::optional<ValidatorSessionCandidateId> expected_id,
                                             bool is_overlay_broadcast) {
  // Note: src is not necessarily equal to the sender of this message:
  // If requested using get_broadcast_p2p, src is the creator of the block, sender possibly is some other node.
  auto src_idx = description().get_source_idx(src);
  td::Timer deserialize_timer;
  auto R =
      deserialize_candidate(data, compress_block_candidates_,
                            description().opts().max_block_size + description().opts().max_collated_data_size + 1024);
  double deserialize_time = deserialize_timer.elapsed();
  if (R.is_error()) {
    VLOG(VALIDATOR_SESSION_WARNING) << this << "[node " << src << "][broadcast " << sha256_bits256(data.as_slice())
                                    << "]: failed to parse: " << R.move_as_error();
    return;
  }
  auto candidate = R.move_as_ok();
  if (PublicKeyHash{candidate->src_} != src) {
    VLOG(VALIDATOR_SESSION_WARNING) << this << "[node " << src << "][broadcast " << sha256_bits256(data.as_slice())
                                    << "]: source mismatch";
    return;
  }
  if (candidate->data_.size() > description().opts().max_block_size ||
      candidate->collated_data_.size() > description().opts().max_collated_data_size) {
    VLOG(VALIDATOR_SESSION_WARNING) << this << "[node " << src << "][broadcast " << sha256_bits256(data.as_slice())
                                    << "]: too big broadcast size=" << candidate->data_.size() << " "
                                    << candidate->collated_data_.size();
    return;
  }

  auto file_hash = sha256_bits256(candidate->data_.as_slice());
  auto collated_data_file_hash = sha256_bits256(candidate->collated_data_.as_slice());
  auto block_round = static_cast<td::uint32>(candidate->round_);
  auto block_id = description().candidate_id(src_idx, candidate->root_hash_, file_hash, collated_data_file_hash);

  if (expected_id && expected_id.value() != block_id) {
    VLOG(VALIDATOR_SESSION_WARNING) << this << "[node " << src << "][broadcast " << sha256_bits256(data.as_slice())
                                    << "]: id mismatch";
    return;
  }

  auto stat = stats_get_candidate_stat(block_round, src, block_id);
  if (stat) {
    if (stat->block_status == ValidatorSessionStats::status_none) {
      stat->block_status = ValidatorSessionStats::status_received;
    }
    if (stat->block_timestamp <= 0.0) {
      stat->block_timestamp = td::Clocks::system();
    }
    stat->deserialize_time = deserialize_time;
    stat->serialized_size = data.size();
    stat->root_hash = candidate->root_hash_;
    stat->file_hash = file_hash;
  }

  if ((td::int32)block_round < (td::int32)cur_round_ - MAX_PAST_ROUND_BLOCK ||
      block_round >= cur_round_ + MAX_FUTURE_ROUND_BLOCK) {
    VLOG(VALIDATOR_SESSION_NOTICE) << this << "[node " << src << "][broadcast " << block_id
                                   << "]: bad round=" << block_round << " cur_round" << cur_round_;
    return;
  }
  auto it = blocks_.find(block_id);
  if (it != blocks_.end()) {
    it->second->round_ = std::max<td::uint32>(it->second->round_, block_round);
    VLOG(VALIDATOR_SESSION_INFO) << this << "[node " << src << "][broadcast " << block_id << "]: duplicate";
    return;
  }

  auto priority = description().get_node_priority(src_idx, block_round);
  if (priority < 0) {
    VLOG(VALIDATOR_SESSION_WARNING) << this << "[node " << src << "][broadcast " << block_id
                                    << "]: source is not allowed to generate blocks in this round";
    return;
  }

  if (is_overlay_broadcast && !ensure_candidate_unique(src_idx, block_round, block_id)) {
    return;
  }

  blocks_[block_id] = std::move(candidate);

  VLOG(VALIDATOR_SESSION_WARNING) << this << ": received broadcast " << block_id;
  if (block_round != cur_round_) {
    return;
  }

  CHECK(!pending_approve_.count(block_id));
  CHECK(!approved_.count(block_id));
  CHECK(!pending_reject_.count(block_id));
  CHECK(!rejected_.count(block_id));

  auto v = virtual_state_->choose_blocks_to_approve(description(), local_idx());
  for (auto &b : v) {
    if (b && SentBlock::get_block_id(b) == block_id) {
      try_approve_block(b);
      break;
    }
  }
}

void ValidatorSessionImpl::process_message(PublicKeyHash src, td::BufferSlice data) {
}

void ValidatorSessionImpl::process_query(PublicKeyHash src, td::BufferSlice data,
                                         td::Promise<td::BufferSlice> promise) {
  if (!started_) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "not started"));
    return;
  }
  auto F = fetch_tl_object<ton_api::validatorSession_downloadCandidate>(std::move(data), true);
  if (F.is_error()) {
    promise.set_error(F.move_as_error_prefix("validator session: cannot parse query: "));
    return;
  }
  auto f = F.move_as_ok();

  auto round_id = static_cast<td::uint32>(f->round_);
  if (round_id > real_state_->cur_round_seqno()) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "too big round id"));
    return;
  }
  const SentBlock *block = nullptr;
  auto id = description().candidate_id(description().get_source_idx(PublicKeyHash{f->id_->src_}), f->id_->root_hash_,
                                       f->id_->file_hash_, f->id_->collated_data_file_hash_);
  if (round_id < real_state_->cur_round_seqno()) {
    block = real_state_->get_committed_block(description(), round_id);
    if (!block || SentBlock::get_block_id(block) != id) {
      promise.set_error(td::Status::Error(ErrorCode::notready, "wrong block in old round"));
      return;
    }
  } else {
    CHECK(round_id == real_state_->cur_round_seqno());
    bool found;
    block = real_state_->get_block(description(), id, found);
    if (!found || !block) {
      promise.set_error(td::Status::Error(ErrorCode::notready, "wrong block"));
      return;
    }
    if (!real_state_->check_block_is_approved_by(description(), local_idx(), id)) {
      promise.set_error(td::Status::Error(ErrorCode::notready, "not approved block"));
      return;
    }
  }
  CHECK(block);

  auto P = td::PromiseCreator::lambda([promise = std::move(promise), src = f->id_->src_, round_id,
                                       compress = compress_block_candidates_](td::Result<BlockCandidate> R) mutable {
    if (R.is_error()) {
      promise.set_error(R.move_as_error_prefix("failed to get candidate: "));
    } else {
      auto c = R.move_as_ok();
      auto obj = create_tl_object<ton_api::validatorSession_candidate>(src, round_id, c.id.root_hash, std::move(c.data),
                                                                       std::move(c.collated_data));
      promise.set_result(serialize_candidate(obj, compress));
    }
  });

  callback_->get_approved_candidate(description().get_source_public_key(block->get_src_idx()), f->id_->root_hash_,
                                    f->id_->file_hash_, f->id_->collated_data_file_hash_, std::move(P));
}

void ValidatorSessionImpl::candidate_decision_fail(td::uint32 round, ValidatorSessionCandidateId hash,
                                                   std::string result, td::uint32 src, td::BufferSlice proof,
                                                   double validation_time, bool validation_cached) {
  auto stat = stats_get_candidate_stat(round, description().get_source_id(src), hash);
  if (stat) {
    stat->block_status = ValidatorSessionStats::status_rejected;
    stat->comment = result;
    stat->validation_time = validation_time;
    stat->validated_at = td::Clocks::system();
    stat->validation_cached = validation_cached;
  }
  if (round != cur_round_) {
    return;
  }
  LOG(ERROR) << this << ": failed candidate " << hash << ": " << result;
  pending_approve_.erase(hash);
  if (result.size() > MAX_REJECT_REASON_SIZE) {
    result.resize(MAX_REJECT_REASON_SIZE);
  }
  pending_reject_.emplace(hash, td::BufferSlice{result});
  rejected_.insert(hash);
}

void ValidatorSessionImpl::candidate_decision_ok(td::uint32 round, ValidatorSessionCandidateId hash, RootHash root_hash,
                                                 FileHash file_hash, td::uint32 src, td::uint32 ok_from,
                                                 double validation_time, bool validation_cached) {
  auto stat = stats_get_candidate_stat(round, description().get_source_id(src), hash);
  if (stat) {
    stat->block_status = ValidatorSessionStats::status_approved;
    stat->comment = PSTRING() << "ts=" << ok_from;
    stat->validation_time = validation_time;
    stat->gen_utime = (double)ok_from;
    stat->validated_at = td::Clocks::system();
    stat->validation_cached = validation_cached;
  }
  if (round != cur_round_) {
    return;
  }

  LOG(INFO) << this << ": approved candidate " << hash;

  auto obj = create_tl_object<ton_api::ton_blockIdApprove>(root_hash, file_hash);
  auto data = serialize_tl_object(obj, true);

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), print_id = print_id(), hash, ok_from,
                                       round = cur_round_](td::Result<td::BufferSlice> R) {
    if (R.is_error()) {
      LOG(FATAL) << print_id << ": failed to sign: " << R.move_as_error();
    } else {
      td::actor::send_closure(SelfId, &ValidatorSessionImpl::candidate_approved_signed, round, hash, ok_from,
                              R.move_as_ok());
    }
  });

  td::actor::send_closure(keyring_, &keyring::Keyring::sign_message, local_id(), std::move(data), std::move(P));
}

void ValidatorSessionImpl::candidate_approved_signed(td::uint32 round, ValidatorSessionCandidateId hash,
                                                     td::uint32 ok_from, td::BufferSlice signature) {
  pending_approve_.erase(hash);
  approved_[hash] = std::pair<td::uint32, td::BufferSlice>{ok_from, std::move(signature)};

  if (ok_from <= td::Clocks::system()) {
    request_new_block(false);
  } else {
    LOG(WARNING) << "too new block. ts=" << ok_from;
    alarm_timestamp().relax(td::Timestamp::at_unix(ok_from));
  }
}

void ValidatorSessionImpl::generated_block(td::uint32 round, ValidatorSessionCandidateId root_hash,
                                           td::BufferSlice data, td::BufferSlice collated_data, double collation_time,
                                           bool collation_cached) {
  if (data.size() > description().opts().max_block_size ||
      collated_data.size() > description().opts().max_collated_data_size) {
    LOG(ERROR) << this << ": generated candidate is too big. Dropping. size=" << data.size() << " "
               << collated_data.size();
    return;
  }
  auto file_hash = sha256_bits256(data.as_slice());
  auto collated_data_file_hash = sha256_bits256(collated_data.as_slice());
  auto block_id = description().candidate_id(local_idx(), root_hash, file_hash, collated_data_file_hash);

  auto stat = stats_get_candidate_stat(round, local_id(), block_id);
  if (stat) {
    stat->block_status = ValidatorSessionStats::status_received;
    stat->collation_time = collation_time;
    stat->collated_at = td::Clocks::system();
    stat->block_timestamp = td::Clocks::system();
    stat->collation_cached = collation_cached;
    stat->root_hash = root_hash;
    stat->file_hash = file_hash;
  }
  if (round != cur_round_) {
    return;
  }
  td::Timer serialize_timer;
  auto b = create_tl_object<ton_api::validatorSession_candidate>(local_id().tl(), round, root_hash, std::move(data),
                                                                 std::move(collated_data));
  auto B = serialize_candidate(b, compress_block_candidates_).move_as_ok();
  if (stat) {
    stat->serialize_time = serialize_timer.elapsed();
    stat->serialized_size = B.size();
  }

  td::actor::send_closure(catchain_, &catchain::CatChain::send_broadcast, std::move(B));

  blocks_.emplace(block_id, std::move(b));
  pending_generate_ = false;
  generated_ = true;
  generated_block_ = block_id;

  request_new_block(true);
}

void ValidatorSessionImpl::signed_block(td::uint32 round, ValidatorSessionCandidateId hash, td::BufferSlice signature) {
  if (round != cur_round_) {
    return;
  }
  pending_sign_ = false;
  signed_ = true;
  signed_block_ = hash;
  signature_ = std::move(signature);

  request_new_block(false);
}

void ValidatorSessionImpl::alarm() {
  alarm_timestamp() = td::Timestamp::never();
  check_all();
}

void ValidatorSessionImpl::check_vote_for_slot(td::uint32 att) {
  if (!catchain_started_) {
    return;
  }
  if (!started_) {
    return;
  }
  if (virtual_state_->check_need_generate_vote_for(description(), local_idx(), att)) {
    request_new_block(false);
  }
}

void ValidatorSessionImpl::check_generate_slot() {
  if (!catchain_started_) {
    return;
  }
  if (!generated_ && !pending_generate_ && started_) {
    if (real_state_->check_block_is_sent_by(description(), local_idx())) {
      generated_ = true;
      sent_generated_ = true;
      return;
    }
    auto priority = description().get_node_priority(local_idx(), cur_round_);
    if (priority >= 0) {
      auto t = td::Timestamp::at(round_started_at_.at() + description().get_delay(priority));
      if (t.is_in_past()) {
        pending_generate_ = true;

        td::PerfWarningTimer timer{"too long block generation", 1.0};

        auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), print_id = print_id(), timer = std::move(timer),
                                             round = cur_round_](td::Result<GeneratedCandidate> R) {
          if (R.is_ok()) {
            auto c = std::move(R.ok_ref().candidate);
            td::actor::send_closure(SelfId, &ValidatorSessionImpl::generated_block, round, c.id.root_hash,
                                    c.data.clone(), c.collated_data.clone(), timer.elapsed(), R.ok().is_cached);
          } else {
            LOG(WARNING) << print_id << ": failed to generate block candidate: " << R.move_as_error();
          }
        });
        callback_->on_generate_slot(
            BlockSourceInfo{cur_round_, first_block_round_, description().get_source_public_key(local_idx()), priority},
            std::move(P));
      } else {
        alarm_timestamp().relax(t);
      }
    }
  }
}

void ValidatorSessionImpl::try_approve_block(const SentBlock *block) {
  auto block_id = SentBlock::get_block_id(block);
  {
    auto it = approved_.find(block_id);
    if (it != approved_.end()) {
      if (it->second.first <= td::Clocks::system()) {
        request_new_block(false);
      } else {
        alarm_timestamp().relax(td::Timestamp::at_unix(it->second.first));
      }
      return;
    }
  }
  if (pending_approve_.count(block_id) || rejected_.count(block_id)) {
    return;
  }

  auto delay = block ? description().get_delay(description().get_node_priority(block->get_src_idx(), cur_round_))
                     : description().get_empty_block_delay();
  auto T = td::Timestamp::at(round_started_at_.at() + delay);
  if (!T.is_in_past()) {
    alarm_timestamp().relax(T);
    return;
  }

  if (block) {
    if (!ensure_candidate_unique(block->get_src_idx(), cur_round_, SentBlock::get_block_id(block))) {
      return;
    }
    auto T = td::Timestamp::at(round_started_at_.at() + description().get_delay(block->get_src_idx()) +
                               REQUEST_BROADCAST_P2P_DELAY);
    auto it = blocks_.find(block_id);

    if (it != blocks_.end()) {
      it->second->round_ = std::max<td::uint32>(it->second->round_, cur_round_);
      td::PerfWarningTimer timer{"too long block validation", 1.0};
      auto &B = it->second;
      auto stat = stats_get_candidate_stat(B->round_, PublicKeyHash{B->src_});
      if (stat) {
        // Can happen if block is cached from previous round
        if (stat->block_status == ValidatorSessionStats::status_none) {
          stat->block_status = ValidatorSessionStats::status_received;
        }
        if (stat->block_timestamp <= 0.0) {
          stat->block_timestamp = td::Clocks::system();
        }
        stat->root_hash = B->root_hash_;
        stat->file_hash = td::sha256_bits256(B->data_);
      }

      auto P = td::PromiseCreator::lambda([round = cur_round_, hash = block_id, root_hash = block->get_root_hash(),
                                           file_hash = block->get_file_hash(), timer = std::move(timer),
                                           src = block->get_src_idx(),
                                           SelfId = actor_id(this)](td::Result<CandidateDecision> res) {
        if (res.is_error()) {
          LOG(ERROR) << "round " << round << " failed to validate candidate " << hash << ": " << res.move_as_error();
          return;
        }

        auto R = res.move_as_ok();
        if (R.is_ok()) {
          td::actor::send_closure(SelfId, &ValidatorSessionImpl::candidate_decision_ok, round, hash, root_hash,
                                  file_hash, src, R.ok_from(), timer.elapsed(), R.is_cached());
        } else {
          td::actor::send_closure(SelfId, &ValidatorSessionImpl::candidate_decision_fail, round, hash, R.reason(),
                                  src, R.proof(), timer.elapsed(), R.is_cached());
        }
      });
      pending_approve_.insert(block_id);

      callback_->on_candidate(
          BlockSourceInfo{cur_round_, first_block_round_, description().get_source_public_key(block->get_src_idx()),
                          description().get_node_priority(block->get_src_idx(), cur_round_)},
          B->root_hash_, B->data_.clone(), B->collated_data_.clone(), std::move(P));
    } else if (T.is_in_past()) {
      if (!active_requests_.count(block_id)) {
        auto v = virtual_state_->get_block_approvers(description(), block_id);
        if (v.size() > 0) {
          auto id = description().get_source_id(v[td::Random::fast(0, static_cast<td::int32>(v.size() - 1))]);
          auto src_id = description().get_source_id(block->get_src_idx());
          active_requests_.insert(block_id);
          auto P = td::PromiseCreator::lambda(
              [SelfId = actor_id(this), id, src_id, print_id = print_id(), hash = block_id, round = cur_round_,
               candidate_id = SentBlock::get_block_id(block)](td::Result<td::BufferSlice> R) {
                td::actor::send_closure(SelfId, &ValidatorSessionImpl::end_request, round, hash);
                if (R.is_error()) {
                  VLOG(VALIDATOR_SESSION_WARNING) << print_id << ": failed to get candidate " << hash << " from " << id
                                                  << ": " << R.move_as_error();
                } else {
                  td::actor::send_closure(SelfId, &ValidatorSessionImpl::process_broadcast, src_id, R.move_as_ok(),
                                          candidate_id, false);
                }
              });

          get_broadcast_p2p(id, block->get_file_hash(), block->get_collated_data_file_hash(),
                            description().get_source_id(block->get_src_idx()), cur_round_, block->get_root_hash(),
                            std::move(P), td::Timestamp::in(15.0));
        } else {
          LOG(VALIDATOR_SESSION_DEBUG) << this << ": no nodes to download candidate " << block << " from";
        }
      }
    } else {
      alarm_timestamp().relax(T);
    }
  } else {
    approved_[block_id] = std::pair<td::uint32, td::BufferSlice>{0, td::BufferSlice{}};

    request_new_block(false);
  }
}

void ValidatorSessionImpl::get_broadcast_p2p(PublicKeyHash node, ValidatorSessionFileHash file_hash,
                                             ValidatorSessionCollatedDataFileHash collated_data_file_hash,
                                             PublicKeyHash src, td::uint32 round, ValidatorSessionRootHash root_hash,
                                             td::Promise<td::BufferSlice> promise, td::Timestamp timeout) {
  if (timeout.is_in_past()) {
    promise.set_error(td::Status::Error(ErrorCode::timeout, "timeout"));
    return;
  }

  auto obj = create_tl_object<ton_api::validatorSession_downloadCandidate>(
      round,
      create_tl_object<ton_api::validatorSession_candidateId>(src.tl(), root_hash, file_hash, collated_data_file_hash));

  td::actor::send_closure(
      catchain_, &catchain::CatChain::send_query_via, node, "download candidate", std::move(promise), timeout,
      serialize_tl_object(obj, true),
      description().opts().max_block_size + description().opts().max_collated_data_size + MAX_CANDIDATE_EXTRA_SIZE,
      rldp_);
}

void ValidatorSessionImpl::check_sign_slot() {
  if (!catchain_started_) {
    return;
  }
  if (pending_sign_ || !started_) {
    return;
  }

  if (real_state_->check_block_is_signed_by(description(), local_idx())) {
    signed_ = true;
    return;
  }
  if (signed_) {
    request_new_block(false);
    return;
  }
  auto found = false;
  auto B = virtual_state_->choose_block_to_sign(description(), local_idx(), found);
  if (found) {
    if (!B) {
      signed_ = true;
      signed_block_ = skip_round_candidate_id();

      request_new_block(false);
    } else {
      pending_sign_ = true;

      auto obj = create_tl_object<ton_api::ton_blockId>(B->get_root_hash(), B->get_file_hash());
      auto data = serialize_tl_object(obj, true);

      auto P =
          td::PromiseCreator::lambda([SelfId = actor_id(this), print_id = print_id(), hash = SentBlock::get_block_id(B),
                                      round = cur_round_](td::Result<td::BufferSlice> R) {
            if (R.is_error()) {
              LOG(FATAL) << print_id << ": failed to sign: " << R.move_as_error();
            } else {
              td::actor::send_closure(SelfId, &ValidatorSessionImpl::signed_block, round, hash, R.move_as_ok());
            }
          });

      td::actor::send_closure(keyring_, &keyring::Keyring::sign_message, local_id(), std::move(data), std::move(P));
    }
  }
}

void ValidatorSessionImpl::check_approve() {
  if (!catchain_started_) {
    return;
  }
  if (!started_) {
    return;
  }
  auto to_approve = virtual_state_->choose_blocks_to_approve(description(), local_idx());
  for (auto &block : to_approve) {
    try_approve_block(block);
  }
}

void ValidatorSessionImpl::check_action(td::uint32 att) {
  if (!catchain_started_) {
    return;
  }
  if (!requested_new_block_) {
    auto action = virtual_state_->create_action(description(), local_idx(), att);
    if (action && action->get_id() != ton_api::validatorSession_message_empty::ID) {
      request_new_block(false);
    }
  }
}

void ValidatorSessionImpl::check_all() {
  if (!catchain_started_) {
    return;
  }
  if (virtual_state_->cur_round_seqno() != cur_round_) {
    request_new_block(false);
    return;
  }
  if (round_debug_at_.is_in_past()) {
    td::BufferSlice buf{10000};
    td::StringBuilder sb{buf.as_slice()};
    real_state_->dump_cur_attempt(description(), sb);
    virtual_state_->dump_cur_attempt(description(), sb);
    auto to_approve = virtual_state_->choose_blocks_to_approve(description(), local_idx());
    sb << "to approve: \n";
    for (auto &B : to_approve) {
      if (B) {
        auto block_id = SentBlock::get_block_id(B);
        auto pending = pending_approve_.count(block_id) == 1;
        auto rejected = rejected_.count(block_id) == 1;
        auto accepted = approved_.count(block_id) == 1;
        sb << "    " << block_id << " pending:  " << pending << " rejected: " << rejected << " accepted: " << accepted
           << "\n";
      } else {
        sb << "    SKIP\n";
      }
    }
    LOG(ERROR) << sb.as_cslice();
    round_debug_at_ = td::Timestamp::in(60.0);
  }
  auto att = description().get_attempt_seqno(description().get_ts());
  check_sign_slot();
  check_approve();
  check_generate_slot();
  check_action(att);
  check_vote_for_slot(att);
  alarm_timestamp().relax(round_debug_at_);
  alarm_timestamp().relax(description().attempt_start_at(att + 1));
}

void ValidatorSessionImpl::request_new_block(bool now) {
  if (requested_new_block_now_) {
    return;
  }
  if (!now && requested_new_block_) {
    return;
  }
  requested_new_block_ = true;
  if (now) {
    requested_new_block_now_ = true;
    td::actor::send_closure(catchain_, &catchain::CatChain::need_new_block, td::Timestamp::now());
  } else {
    double lambda = 10.0 / description().get_total_nodes();
    double x = -1 / lambda * log(td::Random::fast(1, 999) * 0.001);
    x = std::min(x, get_current_max_block_delay());  // default = 0.4
    td::actor::send_closure(catchain_, &catchain::CatChain::need_new_block, td::Timestamp::in(x));
  }
}

double ValidatorSessionImpl::get_current_max_block_delay() const {
  td::uint32 att = real_state_->cur_attempt_in_round(*description_);
  td::uint32 att1 = description_->opts().max_round_attempts;
  if (att <= att1) {
    return catchain_max_block_delay_;
  }
  td::uint32 att2 = att1 + 4;
  if (att >= att2) {
    return catchain_max_block_delay_slow_;
  }
  return catchain_max_block_delay_ +
         (catchain_max_block_delay_slow_ - catchain_max_block_delay_) * (double)(att - att1) / (double)(att2 - att1);
}

void ValidatorSessionImpl::on_new_round(td::uint32 round) {
  if (round != 0) {
    CHECK(cur_round_ < round);
    pending_generate_ = false;
    generated_ = false;
    sent_generated_ = false;

    pending_approve_.clear();
    rejected_.clear();
    pending_reject_.clear();
    approved_.clear();

    pending_sign_ = false;
    signed_ = false;
    signature_ = td::BufferSlice{};
    signed_block_ = skip_round_candidate_id();

    active_requests_.clear();
  }

  while (cur_round_ < round) {
    auto block = real_state_->get_committed_block(description(), cur_round_);
    auto sigs = real_state_->get_committed_block_signatures(description(), cur_round_);
    CHECK(sigs);
    auto approve_sigs = real_state_->get_committed_block_approve_signatures(description(), cur_round_);
    CHECK(approve_sigs);

    std::vector<std::pair<PublicKeyHash, td::BufferSlice>> export_sigs;
    ValidatorWeight signatures_weight = 0;
    CHECK(sigs->size() == description().get_total_nodes());
    for (td::uint32 i = 0; i < description().get_total_nodes(); i++) {
      auto sig = sigs->at(i);
      if (sig) {
        CHECK(description().is_persistent(sig));
        export_sigs.emplace_back(description().get_source_id(i), sig->value().clone());
        signatures_weight += description().get_node_weight(i);
      }
    }

    std::vector<std::pair<PublicKeyHash, td::BufferSlice>> export_approve_sigs;
    ValidatorWeight approve_signatures_weight = 0;
    CHECK(approve_sigs->size() == description().get_total_nodes());
    for (td::uint32 i = 0; i < description().get_total_nodes(); i++) {
      auto sig = approve_sigs->at(i);
      if (sig) {
        CHECK(description().is_persistent(sig));
        export_approve_sigs.emplace_back(description().get_source_id(i), sig->value().clone());
        approve_signatures_weight += description().get_node_weight(i);
      }
    }

    auto it = blocks_.find(SentBlock::get_block_id(block));
    bool have_block = (bool)block;
    if (!have_block) {
      callback_->on_block_skipped(cur_round_);
    } else {
      cur_stats_.success = true;
      cur_stats_.timestamp = td::Clocks::system();
      cur_stats_.signatures = (td::uint32)export_sigs.size();
      cur_stats_.signatures_weight = signatures_weight;
      cur_stats_.approve_signatures = (td::uint32)export_approve_sigs.size();
      cur_stats_.approve_signatures_weight = approve_signatures_weight;
      cur_stats_.creator = description().get_source_id(block->get_src_idx());
      auto stat = stats_get_candidate_stat(cur_round_, cur_stats_.creator);
      if (stat) {
        stat->is_accepted = true;
      }
      auto stats = cur_stats_;
      while (!stats.rounds.empty() && stats.rounds.size() + stats.first_round - 1 > cur_round_) {
        stats.rounds.pop_back();
      }

      BlockSourceInfo source_info{cur_round_, first_block_round_,
                                  description().get_source_public_key(block->get_src_idx()),
                                  description().get_node_priority(block->get_src_idx(), cur_round_)};
      if (it == blocks_.end()) {
        callback_->on_block_committed(std::move(source_info), block->get_root_hash(), block->get_file_hash(),
                                      td::BufferSlice(), std::move(export_sigs), std::move(export_approve_sigs),
                                      std::move(stats));
      } else {
        callback_->on_block_committed(std::move(source_info), block->get_root_hash(), block->get_file_hash(),
                                      it->second->data_.clone(), std::move(export_sigs), std::move(export_approve_sigs),
                                      std::move(stats));
      }
      first_block_round_ = cur_round_ + 1;
    }
    cur_round_++;
    if (have_block) {
      stats_init();
    } else {
      size_t round_idx = cur_round_ - cur_stats_.first_round;
      while (round_idx >= cur_stats_.rounds.size()) {
        stats_add_round();
      }
      cur_stats_.rounds[round_idx].timestamp = td::Clocks::system();
    }
    auto it2 = blocks_.begin();
    while (it2 != blocks_.end()) {
      if (it2->second->round_ < (td::int32)cur_round_ - MAX_PAST_ROUND_BLOCK) {
        it2 = blocks_.erase(it2);
      } else {
        ++it2;
      }
    }
  }

  round_started_at_ = td::Timestamp::now();
  round_debug_at_ = td::Timestamp::in(60.0);
  check_all();
}

void ValidatorSessionImpl::on_catchain_started() {
  catchain_started_ = true;

  auto X = virtual_state_->get_blocks_approved_by(description(), local_idx());

  for (auto &x : X) {
    if (x) {
      auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), round = virtual_state_->cur_round_seqno(),
                                           src = description().get_source_id(x->get_src_idx()),
                                           root_hash = x->get_root_hash(),
                                           compress = compress_block_candidates_](td::Result<BlockCandidate> R) {
        if (R.is_error()) {
          LOG(ERROR) << "failed to get candidate: " << R.move_as_error();
        } else {
          auto B = R.move_as_ok();
          auto broadcast = create_tl_object<ton_api::validatorSession_candidate>(
              src.tl(), round, root_hash, std::move(B.data), std::move(B.collated_data));
          td::actor::send_closure(SelfId, &ValidatorSessionImpl::process_broadcast, src,
                                  serialize_candidate(broadcast, compress).move_as_ok(), td::optional<ValidatorSessionCandidateId>(),
                                  false);
        }
      });
      callback_->get_approved_candidate(description().get_source_public_key(x->get_src_idx()), x->get_root_hash(),
                                        x->get_file_hash(), x->get_collated_data_file_hash(), std::move(P));
    }
  }

  check_all();
}

ValidatorSessionImpl::ValidatorSessionImpl(catchain::CatChainSessionId session_id, ValidatorSessionOptions opts,
                                           PublicKeyHash local_id, std::vector<ValidatorSessionNode> nodes,
                                           std::unique_ptr<Callback> callback,
                                           td::actor::ActorId<keyring::Keyring> keyring,
                                           td::actor::ActorId<adnl::Adnl> adnl, td::actor::ActorId<rldp::Rldp> rldp,
                                           td::actor::ActorId<overlay::Overlays> overlays, std::string db_root,
                                           std::string db_suffix, bool allow_unsafe_self_blocks_resync)
    : unique_hash_(session_id)
    , callback_(std::move(callback))
    , db_root_(db_root)
    , db_suffix_(db_suffix)
    , keyring_(keyring)
    , adnl_(adnl)
    , rldp_(rldp)
    , overlay_manager_(overlays)
    , allow_unsafe_self_blocks_resync_(allow_unsafe_self_blocks_resync) {
  compress_block_candidates_ = opts.proto_version >= 4;
  description_ = ValidatorSessionDescription::create(std::move(opts), nodes, local_id);
  src_round_candidate_.resize(description_->get_total_nodes());
}

void ValidatorSessionImpl::start() {
  round_started_at_ = td::Timestamp::now();
  round_debug_at_ = td::Timestamp::in(60.0);
  stats_init();
  started_ = true;
  VLOG(VALIDATOR_SESSION_NOTICE) << this << ": started";

  auto w = description().export_catchain_nodes();

  catchain_ = catchain::CatChain::create(
      make_catchain_callback(), description().opts().catchain_opts, keyring_, adnl_, overlay_manager_, std::move(w),
      local_id(), unique_hash_, db_root_, db_suffix_, allow_unsafe_self_blocks_resync_);

  check_all();
}

void ValidatorSessionImpl::destroy() {
  if (!catchain_.empty()) {
    td::actor::send_closure(catchain_, &catchain::CatChain::destroy);
    catchain_.release();
  }
  stop();
}

void ValidatorSessionImpl::get_current_stats(td::Promise<ValidatorSessionStats> promise) {
  promise.set_result(cur_stats_);
}

void ValidatorSessionImpl::get_end_stats(td::Promise<EndValidatorGroupStats> promise) {
  if (!started_) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "not started"));
    return;
  }
  EndValidatorGroupStats stats;
  stats.session_id = unique_hash_;
  stats.timestamp = td::Clocks::system();
  stats.nodes.resize(description().get_total_nodes());
  for (size_t i = 0; i < stats.nodes.size(); ++i) {
    stats.nodes[i].id = description().get_source_id(i);
  }
  td::actor::send_closure(catchain_, &catchain::CatChain::get_source_heights,
                          [promise = std::move(promise),
                           stats = std::move(stats)](td::Result<std::vector<catchain::CatChainBlockHeight>> R) mutable {
                            TRY_RESULT_PROMISE(promise, heights, std::move(R));
                            for (size_t i = 0; i < std::min(heights.size(), stats.nodes.size()); ++i) {
                              stats.nodes[i].catchain_blocks = heights[i];
                            }
                            promise.set_result(std::move(stats));
                          });
}

void ValidatorSessionImpl::get_validator_group_info_for_litequery(
    td::uint32 cur_round,
    td::Promise<std::vector<tl_object_ptr<lite_api::liteServer_nonfinal_candidateInfo>>> promise) {
  if (cur_round != cur_round_ || real_state_->cur_round_seqno() != cur_round) {
    promise.set_value({});
    return;
  }
  std::vector<tl_object_ptr<lite_api::liteServer_nonfinal_candidateInfo>> result;
  real_state_->for_each_cur_round_sent_block([&](const SessionBlockCandidate *block) {
    if (block->get_block() == nullptr) {
      return;
    }
    auto candidate = create_tl_object<lite_api::liteServer_nonfinal_candidateInfo>();

    candidate->id_ = create_tl_object<lite_api::liteServer_nonfinal_candidateId>();
    candidate->id_->block_id_ = create_tl_object<lite_api::tonNode_blockIdExt>();
    candidate->id_->block_id_->root_hash_ =
        block->get_block()->get_root_hash();  // other fields will be filled in validator-group.cpp
    candidate->id_->block_id_->file_hash_ = block->get_block()->get_file_hash();
    candidate->id_->creator_ =
        description().get_source_public_key(block->get_block()->get_src_idx()).ed25519_value().raw();
    candidate->id_->collated_data_hash_ = block->get_block()->get_collated_data_file_hash();

    candidate->total_weight_ = description().get_total_weight();
    candidate->approved_weight_ = 0;
    candidate->signed_weight_ = 0;
    for (td::uint32 i = 0; i < description().get_total_nodes(); ++i) {
      if (real_state_->check_block_is_approved_by(description(), i, block->get_id())) {
        candidate->approved_weight_ += description().get_node_weight(i);
      }
    }
    auto precommited = real_state_->get_cur_round_precommitted_block();
    if (SentBlock::get_block_id(precommited) == SentBlock::get_block_id(block->get_block())) {
      auto signatures = real_state_->get_cur_round_signatures();
      if (signatures) {
        for (td::uint32 i = 0; i < description().get_total_nodes(); ++i) {
          if (signatures->at(i)) {
            candidate->signed_weight_ += description().get_node_weight(i);
          }
        }
      }
    }
    result.push_back(std::move(candidate));
  });
  promise.set_result(std::move(result));
}

void ValidatorSessionImpl::start_up() {
  CHECK(!rldp_.empty());
  cur_round_ = 0;
  round_started_at_ = td::Timestamp::now();
  round_debug_at_ = td::Timestamp::in(60.0);
  real_state_ = ValidatorSessionState::create(description());
  real_state_ = ValidatorSessionState::move_to_persistent(description(), real_state_);
  virtual_state_ = real_state_;

  check_all();
  td::actor::send_closure(rldp_, &rldp::Rldp::add_id, description().get_source_adnl_id(local_idx()));
}

void ValidatorSessionImpl::stats_init() {
  auto old_rounds = std::move(cur_stats_.rounds);
  if (stats_inited_ && cur_stats_.first_round + old_rounds.size() > cur_round_) {
    old_rounds.erase(old_rounds.begin(), old_rounds.end() - (cur_stats_.first_round + old_rounds.size() - cur_round_));
  } else {
    old_rounds.clear();
  }
  cur_stats_ = ValidatorSessionStats();
  cur_stats_.rounds = std::move(old_rounds);
  cur_stats_.first_round = cur_round_;
  cur_stats_.session_id = unique_hash_;
  cur_stats_.total_validators = description().get_total_nodes();
  cur_stats_.total_weight = description().get_total_weight();
  cur_stats_.self = description().get_source_id(local_idx());

  for (auto it = stats_pending_approve_.begin(); it != stats_pending_approve_.end(); ) {
    if (it->first.first < cur_round_) {
      it = stats_pending_approve_.erase(it);
    } else {
      ++it;
    }
  }
  for (auto it = stats_pending_sign_.begin(); it != stats_pending_sign_.end(); ) {
    if (it->first.first < cur_round_) {
      it = stats_pending_sign_.erase(it);
    } else {
      ++it;
    }
  }

  if (cur_stats_.rounds.empty()) {
    stats_add_round();
  }
  cur_stats_.rounds[0].timestamp = td::Clocks::system();
  stats_inited_ = true;
}

void ValidatorSessionImpl::stats_add_round() {
  td::uint32 round = cur_stats_.first_round + cur_stats_.rounds.size();
  cur_stats_.rounds.emplace_back();
  auto& stat = cur_stats_.rounds.back();
  stat.producers.resize(description().get_max_priority() + 1);
  for (td::uint32 i = 0; i < description().get_total_nodes(); i++) {
    td::int32 priority = description().get_node_priority(i, round);
    if (priority >= 0) {
      CHECK((size_t)priority < stat.producers.size());
      stat.producers[priority].id = description().get_source_id(i);
      stat.producers[priority].is_ours = (local_idx() == i);
      stat.producers[priority].approvers.resize(description().get_total_nodes(), false);
      stat.producers[priority].signers.resize(description().get_total_nodes(), false);
    }
  }
  while (!stat.producers.empty() && stat.producers.back().id.is_zero()) {
    stat.producers.pop_back();
  }
}

ValidatorSessionStats::Producer *ValidatorSessionImpl::stats_get_candidate_stat(
    td::uint32 round, PublicKeyHash src, ValidatorSessionCandidateId candidate_id) {
  if (round < cur_stats_.first_round || round > cur_round_ + 5) {
    return nullptr;
  }
  while (round - cur_stats_.first_round >= cur_stats_.rounds.size()) {
    stats_add_round();
  }
  auto &stats_round = cur_stats_.rounds[round - cur_stats_.first_round];
  auto it = std::find_if(stats_round.producers.begin(), stats_round.producers.end(),
                         [&](const ValidatorSessionStats::Producer &p) { return p.id == src; });
  if (it == stats_round.producers.end()) {
    return nullptr;
  }
  if (!candidate_id.is_zero()) {
    it->candidate_id = candidate_id;
  }
  auto it2 = stats_pending_approve_.find({round, it->candidate_id});
  if (it2 != stats_pending_approve_.end()) {
    for (td::uint32 node_id : it2->second) {
      it->set_approved_by(node_id, description().get_node_weight(node_id), description().get_total_weight());
    }
    stats_pending_approve_.erase(it2);
  }
  it2 = stats_pending_sign_.find({round, it->candidate_id});
  if (it2 != stats_pending_sign_.end()) {
    for (td::uint32 node_id : it2->second) {
      it->set_signed_by(node_id, description().get_node_weight(node_id), description().get_total_weight());
    }
    stats_pending_sign_.erase(it2);
  }
  return &*it;
}

ValidatorSessionStats::Producer *ValidatorSessionImpl::stats_get_candidate_stat_by_id(
    td::uint32 round, ValidatorSessionCandidateId candidate_id) {
  if (round < cur_stats_.first_round || round > cur_round_ + 5) {
    return nullptr;
  }
  while (round - cur_stats_.first_round >= cur_stats_.rounds.size()) {
    stats_add_round();
  }
  auto &stats_round = cur_stats_.rounds[round - cur_stats_.first_round];
  auto it = std::find_if(stats_round.producers.begin(), stats_round.producers.end(),
                         [&](const ValidatorSessionStats::Producer &p) { return p.candidate_id == candidate_id; });
  if (it == stats_round.producers.end()) {
    return nullptr;
  }
  return &*it;
}

void ValidatorSessionImpl::stats_process_action(td::uint32 node_id, ton_api::validatorSession_round_Message &action) {
  ton_api::downcast_call(action, td::overloaded(
                                     [&](const ton_api::validatorSession_message_submittedBlock &obj) {
                                       auto candidate_id = description().candidate_id(
                                           node_id, obj.root_hash_, obj.file_hash_, obj.collated_data_file_hash_);
                                       auto stat = stats_get_candidate_stat(
                                           obj.round_, description().get_source_id(node_id), candidate_id);
                                       if (stat && stat->got_submit_at <= 0.0) {
                                         stat->got_submit_at = td::Clocks::system();
                                       }
                                     },
                                     [&](const ton_api::validatorSession_message_approvedBlock &obj) {
                                       if (obj.candidate_ == skip_round_candidate_id()) {
                                         return;
                                       }
                                       auto stat = stats_get_candidate_stat_by_id(obj.round_, obj.candidate_);
                                       if (stat) {
                                         stat->set_approved_by(node_id, description().get_node_weight(node_id),
                                                               description().get_total_weight());
                                       } else {
                                         stats_pending_approve_[{obj.round_, obj.candidate_}].push_back(node_id);
                                       }
                                     },
                                     [&](const ton_api::validatorSession_message_commit &obj) {
                                       if (obj.candidate_ == skip_round_candidate_id()) {
                                         return;
                                       }
                                       auto stat = stats_get_candidate_stat_by_id(obj.round_, obj.candidate_);
                                       if (stat) {
                                         stat->set_signed_by(node_id, description().get_node_weight(node_id),
                                                             description().get_total_weight());
                                       } else {
                                         stats_pending_sign_[{obj.round_, obj.candidate_}].push_back(node_id);
                                       }
                                     },
                                     [](const auto &) {}));
}

td::actor::ActorOwn<ValidatorSession> ValidatorSession::create(
    catchain::CatChainSessionId session_id, ValidatorSessionOptions opts, PublicKeyHash local_id,
    std::vector<ValidatorSessionNode> nodes, std::unique_ptr<Callback> callback,
    td::actor::ActorId<keyring::Keyring> keyring, td::actor::ActorId<adnl::Adnl> adnl,
    td::actor::ActorId<rldp::Rldp> rldp, td::actor::ActorId<overlay::Overlays> overlays, std::string db_root,
    std::string db_suffix, bool allow_unsafe_self_blocks_resync) {
  return td::actor::create_actor<ValidatorSessionImpl>("session", session_id, std::move(opts), local_id,
                                                       std::move(nodes), std::move(callback), keyring, adnl, rldp,
                                                       overlays, db_root, db_suffix, allow_unsafe_self_blocks_resync);
}

td::Bits256 ValidatorSessionOptions::get_hash() const {
  if (proto_version == 0) {
    if (!new_catchain_ids) {
      return create_hash_tl_object<ton_api::validatorSession_config>(
          catchain_opts.idle_timeout, catchain_opts.max_deps, round_candidates, next_candidate_delay,
          round_attempt_duration, max_round_attempts, max_block_size, max_collated_data_size);
    } else {
      return create_hash_tl_object<ton_api::validatorSession_configNew>(
          catchain_opts.idle_timeout, catchain_opts.max_deps, round_candidates, next_candidate_delay,
          round_attempt_duration, max_round_attempts, max_block_size, max_collated_data_size, new_catchain_ids);
    }
  } else if (proto_version == 1) {
    return create_hash_tl_object<ton_api::validatorSession_configVersioned>(
        catchain_opts.idle_timeout, catchain_opts.max_deps, round_candidates, next_candidate_delay,
        round_attempt_duration, max_round_attempts, max_block_size, max_collated_data_size, proto_version);
  } else {
    return create_hash_tl_object<ton_api::validatorSession_configVersionedV2>(
        create_tl_object<ton_api::validatorSession_catchainOptions>(
            catchain_opts.idle_timeout, catchain_opts.max_deps, catchain_opts.max_serialized_block_size,
            catchain_opts.block_hash_covers_data, catchain_opts.max_block_height_coeff, catchain_opts.debug_disable_db),
        round_candidates, next_candidate_delay, round_attempt_duration, max_round_attempts, max_block_size,
        max_collated_data_size, proto_version);
  }
}

ValidatorSessionOptions::ValidatorSessionOptions(const ValidatorSessionConfig &conf) {
  proto_version = conf.proto_version;
  catchain_opts = conf.catchain_opts;
  max_block_size = conf.max_block_size;
  max_collated_data_size = conf.max_collated_data_size;
  max_round_attempts = conf.max_round_attempts;
  next_candidate_delay = conf.next_candidate_delay;
  round_attempt_duration = conf.round_attempt_duration;
  round_candidates = conf.round_candidates;
  new_catchain_ids = conf.new_catchain_ids;
}

}  // namespace validatorsession

}  // namespace ton
