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
#include "validator-session-state.h"
#include "td/utils/overloaded.h"
#include "td/utils/Random.h"
#include "auto/tl/ton_api.hpp"

#include <set>

namespace td {

td::StringBuilder& operator<<(td::StringBuilder& sb, const ton::ton_api::validatorSession_round_Message& message) {
  ton::ton_api::downcast_call(
      const_cast<ton::ton_api::validatorSession_round_Message&>(message),
      td::overloaded(
          [&](const ton::ton_api::validatorSession_message_submittedBlock& obj) {
            sb << "SUBMIT(" << obj.round_ << "," << obj.root_hash_ << "," << obj.file_hash_
               << obj.collated_data_file_hash_ << ")";
          },
          [&](const ton::ton_api::validatorSession_message_approvedBlock& obj) {
            sb << "APPROVE(" << obj.round_ << "," << obj.candidate_ << ")";
          },
          [&](const ton::ton_api::validatorSession_message_rejectedBlock& obj) {
            sb << "REJECT(" << obj.round_ << "," << obj.candidate_ << ")";
          },
          [&](const ton::ton_api::validatorSession_message_commit& obj) {
            sb << "COMMIT(" << obj.round_ << "," << obj.candidate_ << ")";
          },
          [&](const ton::ton_api::validatorSession_message_vote& obj) {
            sb << "VOTE(" << obj.round_ << "," << obj.attempt_ << "," << obj.candidate_ << ")";
          },
          [&](const ton::ton_api::validatorSession_message_voteFor& obj) {
            sb << "VOTEFOR(" << obj.round_ << "," << obj.attempt_ << "," << obj.candidate_ << ")";
          },
          [&](const ton::ton_api::validatorSession_message_precommit& obj) {
            sb << "PRECOMMIT(" << obj.round_ << "," << obj.attempt_ << "," << obj.candidate_ << ")";
          },
          [&](const ton::ton_api::validatorSession_message_empty& obj) {
            sb << "EMPTY(" << obj.round_ << "," << obj.attempt_ << ")";
          }));
  return sb;
}

td::StringBuilder& operator<<(td::StringBuilder& sb, const ton::ton_api::validatorSession_round_Message* message) {
  return sb << *message;
}

}  // namespace td

namespace ton {

namespace validatorsession {

static td::uint32 get_round_id(const ton_api::validatorSession_round_Message* message) {
  td::uint32 round = 0;
  bool is_called = ton_api::downcast_call(*const_cast<ton_api::validatorSession_round_Message*>(message),
                                          [&](auto& obj) { round = obj.round_; });
  CHECK(is_called);
  return round;
}

static const ValidatorSessionRoundAttemptState* get_attempt(const AttemptVector* vec, td::uint32 seqno) {
  if (!vec) {
    return nullptr;
  }
  td::int32 l = -1;
  td::int32 r = vec->size();
  while (r - l > 1) {
    auto x = (r + l) / 2;
    if (vec->at(x)->get_seqno() < seqno) {
      l = x;
    } else if (vec->at(x)->get_seqno() > seqno) {
      r = x;
    } else {
      return vec->at(x);
    }
  }
  return nullptr;
}

static const SessionBlockCandidate* get_candidate(const ApproveVector* vec, ValidatorSessionCandidateId id) {
  if (!vec) {
    return nullptr;
  }
  auto size = vec->size();
  auto v = vec->data();
  for (td::uint32 i = 0; i < size; i++) {
    if (v[i]->get_id() == id) {
      return v[i];
    }
  }
  return nullptr;
}

static const SessionVoteCandidate* get_candidate(const VoteVector* vec, ValidatorSessionCandidateId id) {
  if (!vec) {
    return nullptr;
  }
  auto size = vec->size();
  auto v = vec->data();
  for (td::uint32 i = 0; i < size; i++) {
    if (v[i]->get_id() == id) {
      return v[i];
    }
  }
  return nullptr;
}

//
//
// SessionBlockCandidateSignature
//
//

const SessionBlockCandidateSignature* SessionBlockCandidateSignature::merge(ValidatorSessionDescription& desc,
                                                                            const SessionBlockCandidateSignature* l,
                                                                            const SessionBlockCandidateSignature* r) {
  if (!l) {
    return r;
  }
  if (!r) {
    return l;
  }
  if (l == r) {
    return l;
  }
  if (l->as_slice() < r->as_slice()) {
    return l;
  } else {
    return r;
  }
}

//
//
// SessionBlockCandidate
//
//

bool SessionBlockCandidate::check_block_is_approved(ValidatorSessionDescription& desc) const {
  ValidatorWeight w = 0;
  for (td::uint32 i = 0; i < desc.get_total_nodes(); i++) {
    if (approved_by_->at(i)) {
      w += desc.get_node_weight(i);
      if (w >= desc.get_cutoff_weight()) {
        return true;
      }
    }
  }
  return false;
}

const SessionBlockCandidate* SessionBlockCandidate::merge(ValidatorSessionDescription& desc,
                                                          const SessionBlockCandidate* l,
                                                          const SessionBlockCandidate* r) {
  if (!l) {
    return r;
  }
  if (!r) {
    return l;
  }
  if (l == r) {
    return l;
  }
  CHECK(l->get_id() == r->get_id());
  auto v = SessionBlockCandidateSignatureVector::merge(
      desc, l->approved_by_, r->approved_by_,
      [&](const SessionBlockCandidateSignature* l, const SessionBlockCandidateSignature* r) {
        return SessionBlockCandidateSignature::merge(desc, l, r);
      });
  return SessionBlockCandidate::create(desc, l->block_, std::move(v));
}

//
//
// SessionVoteCandidate
//
//

bool SessionVoteCandidate::check_block_is_voted(ValidatorSessionDescription& desc) const {
  ValidatorWeight w = 0;
  for (td::uint32 i = 0; i < desc.get_total_nodes(); i++) {
    if (voted_by_->at(i)) {
      w += desc.get_node_weight(i);
      if (w >= desc.get_cutoff_weight()) {
        return true;
      }
    }
  }
  return false;
}

const SessionVoteCandidate* SessionVoteCandidate::merge(ValidatorSessionDescription& desc,
                                                        const SessionVoteCandidate* l, const SessionVoteCandidate* r) {
  if (!l) {
    return r;
  }
  if (!r) {
    return l;
  }
  if (l == r) {
    return l;
  }
  CHECK(l->get_id() == r->get_id());
  auto v = CntVector<bool>::merge(desc, l->voted_by_, r->voted_by_);
  return SessionVoteCandidate::create(desc, l->block_, std::move(v));
}

//
//
// OLD ROUND STATE
//
//

const ValidatorSessionOldRoundState* ValidatorSessionOldRoundState::create(ValidatorSessionDescription& desc,
                                                                           const ValidatorSessionRoundState* round) {
  bool found;
  auto B = round->get_precommitted_block(found);
  CHECK(found);
  CHECK(round->check_block_is_signed(desc));
  auto E = round->get_block(SentBlock::get_block_id(B));
  CHECK(E);
  return create(desc, round->get_seqno(), B, round->get_signatures(), E->get_approvers_list());
}

const ValidatorSessionOldRoundState* ValidatorSessionOldRoundState::merge(ValidatorSessionDescription& desc,
                                                                          const ValidatorSessionOldRoundState* left,
                                                                          const ValidatorSessionOldRoundState* right) {
  if (!left) {
    return right;
  }
  if (!right) {
    return left;
  }
  if (left == right) {
    return left;
  }
  CHECK(left->seqno_ == right->seqno_);

  auto signs = SessionBlockCandidateSignatureVector::merge(
      desc, left->signatures_, right->signatures_,
      [&](const SessionBlockCandidateSignature* l, const SessionBlockCandidateSignature* r) {
        return SessionBlockCandidateSignature::merge(desc, l, r);
      });
  auto approve_signs = SessionBlockCandidateSignatureVector::merge(
      desc, left->approve_signatures_, right->approve_signatures_,
      [&](const SessionBlockCandidateSignature* l, const SessionBlockCandidateSignature* r) {
        return SessionBlockCandidateSignature::merge(desc, l, r);
      });

  return ValidatorSessionOldRoundState::create(desc, left->seqno_, left->block_, std::move(signs),
                                               std::move(approve_signs));
}

const ValidatorSessionOldRoundState* ValidatorSessionOldRoundState::merge(ValidatorSessionDescription& desc,
                                                                          const ValidatorSessionOldRoundState* left,
                                                                          const ValidatorSessionRoundState* right) {
  CHECK(left->seqno_ == right->get_seqno());

  auto signs = SessionBlockCandidateSignatureVector::merge(
      desc, left->signatures_, right->get_signatures(),
      [&](const SessionBlockCandidateSignature* l, const SessionBlockCandidateSignature* r) {
        return SessionBlockCandidateSignature::merge(desc, l, r);
      });

  auto C = right->get_block(left->get_block_id());
  auto approve_signs = C ? SessionBlockCandidateSignatureVector::merge(
                               desc, left->approve_signatures_, C->get_approvers_list(),
                               [&](const SessionBlockCandidateSignature* l, const SessionBlockCandidateSignature* r) {
                                 return SessionBlockCandidateSignature::merge(desc, l, r);
                               })
                         : left->approve_signatures_;

  return ValidatorSessionOldRoundState::create(desc, left->seqno_, left->block_, std::move(signs),
                                               std::move(approve_signs));
}

const ValidatorSessionOldRoundState* ValidatorSessionOldRoundState::action(
    ValidatorSessionDescription& desc, const ValidatorSessionOldRoundState* state, td::uint32 src_idx, td::uint32 att,
    const ton_api::validatorSession_message_approvedBlock& act) {
  if (act.candidate_ != state->get_block_id()) {
    VLOG(VALIDATOR_SESSION_WARNING) << "[validator session][node " << desc.get_source_id(src_idx) << "][" << act
                                    << "]: approved not committed block in old round. Ignoring";
    return state;
  }
  if (state->approve_signatures_->at(src_idx)) {
    VLOG(VALIDATOR_SESSION_WARNING) << "[validator session][node " << desc.get_source_id(src_idx) << "][" << act
                                    << "]: double approve. Ignoring";
    return state;
  }
  if (act.candidate_ == skip_round_candidate_id()) {
    if (act.signature_.size() != 0) {
      VLOG(VALIDATOR_SESSION_WARNING) << "[validator session][node " << desc.get_source_id(src_idx) << "][" << act
                                      << "]: bad signature in APPROVE. Ignoring";
      return state;
    }
  } else {
    auto S = desc.check_approve_signature(state->get_block()->get_root_hash(), state->get_block()->get_file_hash(),
                                          src_idx, act.signature_.as_slice());
    if (S.is_error()) {
      VLOG(VALIDATOR_SESSION_WARNING) << "[validator session][node " << desc.get_source_id(src_idx) << "][" << act
                                      << "]: bad signature in APPROVE. Ignoring: " << S;
      return state;
    }
  }

  auto sig = SessionBlockCandidateSignature::create(desc, act.signature_.clone());
  auto approve = SessionBlockCandidateSignatureVector::change(desc, state->approve_signatures_, src_idx, sig);

  return ValidatorSessionOldRoundState::create(desc, state->seqno_, state->get_block(), state->signatures_,
                                               std::move(approve));
}

const ValidatorSessionOldRoundState* ValidatorSessionOldRoundState::action(
    ValidatorSessionDescription& desc, const ValidatorSessionOldRoundState* state, td::uint32 src_idx, td::uint32 att,
    const ton_api::validatorSession_message_commit& act) {
  auto block_id = state->get_block_id();
  if (act.candidate_ != block_id) {
    VLOG(VALIDATOR_SESSION_WARNING) << "[validator session][node " << desc.get_source_id(src_idx) << "][" << act
                                    << "]: invalid message: signed wrong block "
                                    << "should be " << block_id;
    return state;
  }
  if (act.candidate_ == skip_round_candidate_id()) {
    if (act.signature_.size() != 0) {
      VLOG(VALIDATOR_SESSION_WARNING) << "[validator session][node " << desc.get_source_id(src_idx) << "][" << act
                                      << "]: invalid skip block signature";
      return state;
    }
  } else {
    auto S = desc.check_signature(state->get_block()->get_root_hash(), state->get_block()->get_file_hash(), src_idx,
                                  act.signature_.as_slice());
    if (S.is_error()) {
      VLOG(VALIDATOR_SESSION_WARNING) << "[validator session][node " << desc.get_source_id(src_idx) << "][" << act
                                      << "]: invalid message: bad signature: " << S.move_as_error();
      return state;
    }
  }

  if (state->check_block_is_signed_by(src_idx)) {
    VLOG(VALIDATOR_SESSION_WARNING) << "[validator session][node " << desc.get_source_id(src_idx) << "][" << act
                                    << "]: invalid message: duplicate signature";
    return state;
  }

  auto signs = SessionBlockCandidateSignatureVector::change(
      desc, state->get_signatures(), src_idx, SessionBlockCandidateSignature::create(desc, act.signature_.clone()));

  return ValidatorSessionOldRoundState::create(desc, state->seqno_, state->get_block(), std::move(signs),
                                               state->approve_signatures_);
}

const ValidatorSessionOldRoundState* ValidatorSessionOldRoundState::action(
    ValidatorSessionDescription& desc, const ValidatorSessionOldRoundState* state, td::uint32 src_idx, td::uint32 att,
    const ton_api::validatorSession_round_Message* act) {
  ton_api::downcast_call(*const_cast<ton_api::validatorSession_round_Message*>(act),
                         [&](const auto& obj) { state = action(desc, state, src_idx, att, obj); });
  return state;
}

//
//
// ATTEMPT STATE
//
//

const ValidatorSessionRoundAttemptState* ValidatorSessionRoundAttemptState::merge(
    ValidatorSessionDescription& desc, const ValidatorSessionRoundAttemptState* left,
    const ValidatorSessionRoundAttemptState* right) {
  if (!left) {
    return right;
  }
  if (!right) {
    return left;
  }
  if (left == right) {
    return left;
  }
  CHECK(left->seqno_ == right->seqno_);

  const SentBlock* vote_for = nullptr;
  bool vote_for_inited = false;
  if (!left->vote_for_inited_) {
    vote_for = right->vote_for_;
    vote_for_inited = right->vote_for_inited_;
  } else if (!right->vote_for_inited_) {
    vote_for = left->vote_for_;
    vote_for_inited = left->vote_for_inited_;
  } else if (left->vote_for_ == right->vote_for_) {
    vote_for_inited = true;
    vote_for = left->vote_for_;
  } else {
    auto l = SentBlock::get_block_id(left->vote_for_);
    auto r = SentBlock::get_block_id(right->vote_for_);

    vote_for_inited = true;
    if (l < r) {
      vote_for = left->vote_for_;
    } else {
      vote_for = right->vote_for_;
    }
  }

  auto precommitted = CntVector<bool>::merge(desc, left->precommitted_, right->precommitted_);
  auto votes = VoteVector::merge(desc, left->votes_, right->votes_,
                                 [&](const SessionVoteCandidate* l, const SessionVoteCandidate* r) {
                                   return SessionVoteCandidate::merge(desc, l, r);
                                 });

  return ValidatorSessionRoundAttemptState::create(desc, left->seqno_, std::move(votes), std::move(precommitted),
                                                   vote_for, vote_for_inited);
}

const ValidatorSessionRoundAttemptState* ValidatorSessionRoundAttemptState::action(
    ValidatorSessionDescription& desc, const ValidatorSessionRoundAttemptState* state, td::uint32 src_idx,
    td::uint32 att, const ton_api::validatorSession_message_voteFor& act, const ValidatorSessionRoundState* round) {
  if (state->vote_for_inited_) {
    VLOG(VALIDATOR_SESSION_WARNING) << "[validator session][node " << desc.get_source_id(src_idx) << "][" << act
                                    << "]: invalid message: duplicate VOTEFOR";
    return state;
  }
  if (src_idx != desc.get_vote_for_author(att)) {
    VLOG(VALIDATOR_SESSION_WARNING) << "[validator session][node " << desc.get_source_id(src_idx) << "][" << act
                                    << "]: invalid message: bad VOTEFOR author";
    return state;
  }
  if (round->get_first_attempt(src_idx) == 0 && desc.opts().max_round_attempts > 0) {
    VLOG(VALIDATOR_SESSION_WARNING) << "[validator session][node " << desc.get_source_id(src_idx) << "][" << act
                                    << "]: invalid message: too early for VOTEFOR";
    return state;
  }
  if (round->get_first_attempt(src_idx) + desc.opts().max_round_attempts > att && desc.opts().max_round_attempts == 0) {
    VLOG(VALIDATOR_SESSION_WARNING) << "[validator session][node " << desc.get_source_id(src_idx) << "][" << act
                                    << "]: invalid message: too early for VOTEFOR";
    return state;
  }

  auto x = round->get_block(act.candidate_);

  if (!x) {
    VLOG(VALIDATOR_SESSION_WARNING) << "[validator session][node " << desc.get_source_id(src_idx) << "][" << act
                                    << "]: invalid message: VOTEFOR for not approved block";
    return state;
  }
  if (!x->check_block_is_approved(desc)) {
    VLOG(VALIDATOR_SESSION_WARNING) << "[validator session][node " << desc.get_source_id(src_idx) << "][" << act
                                    << "]: invalid message: VOTEFOR for not approved block";
    return state;
  }

  return ValidatorSessionRoundAttemptState::create(desc, state->seqno_, state->votes_, state->precommitted_,
                                                   x->get_block(), true);
}

const ValidatorSessionRoundAttemptState* ValidatorSessionRoundAttemptState::action(
    ValidatorSessionDescription& desc, const ValidatorSessionRoundAttemptState* state, td::uint32 src_idx,
    td::uint32 att, const ton_api::validatorSession_message_vote& act, const ValidatorSessionRoundState* round) {
  bool made;
  return make_one(desc, state, src_idx, att, round, &act, made);
}

const ValidatorSessionRoundAttemptState* ValidatorSessionRoundAttemptState::action(
    ValidatorSessionDescription& desc, const ValidatorSessionRoundAttemptState* state, td::uint32 src_idx,
    td::uint32 att, const ton_api::validatorSession_message_precommit& act, const ValidatorSessionRoundState* round) {
  bool made;
  return make_one(desc, state, src_idx, att, round, &act, made);
}

const ValidatorSessionRoundAttemptState* ValidatorSessionRoundAttemptState::action(
    ValidatorSessionDescription& desc, const ValidatorSessionRoundAttemptState* state, td::uint32 src_idx,
    td::uint32 att, const ton_api::validatorSession_message_empty& act, const ValidatorSessionRoundState* round) {
  bool made;
  return make_one(desc, state, src_idx, att, round, &act, made);
}

const ValidatorSessionRoundAttemptState* ValidatorSessionRoundAttemptState::try_vote(
    ValidatorSessionDescription& desc, const ValidatorSessionRoundAttemptState* state, td::uint32 src_idx,
    td::uint32 att, const ValidatorSessionRoundState* round, const ton_api::validatorSession_round_Message* act,
    bool& made) {
  made = false;
  if (state->check_vote_received_from(src_idx)) {
    return state;
  }
  auto found = false;
  auto block = round->choose_block_to_vote(desc, src_idx, att, state->vote_for_, state->vote_for_inited_, found);
  if (!found) {
    return state;
  }
  auto block_id = SentBlock::get_block_id(block);
  made = true;

  if (act) {
    if (act->get_id() != ton_api::validatorSession_message_vote::ID) {
      VLOG(VALIDATOR_SESSION_WARNING) << "[validator session][node " << desc.get_source_id(src_idx) << "][" << act
                                      << "]: expected VOTE(" << block_id << ")";
    } else {
      auto x = static_cast<const ton_api::validatorSession_message_vote*>(act);
      if (x->candidate_ != block_id) {
        VLOG(VALIDATOR_SESSION_WARNING) << "[validator session][node " << desc.get_source_id(src_idx) << "][" << act
                                        << "]: expected VOTE(" << block_id << ")";
      }
    }
  } else {
    VLOG(VALIDATOR_SESSION_WARNING) << "[validator session][node " << desc.get_source_id(src_idx)
                                    << "]: making implicit VOTE(" << block_id << ")";
  }

  auto candidate = get_candidate(state->votes_, block_id);
  if (!candidate) {
    candidate = SessionVoteCandidate::create(desc, block);
  }
  candidate = SessionVoteCandidate::push(desc, candidate, src_idx);
  auto v = VoteVector::push(desc, state->votes_, candidate);
  return ValidatorSessionRoundAttemptState::create(desc, state->seqno_, std::move(v), state->precommitted_,
                                                   state->vote_for_, state->vote_for_inited_);
}

const ValidatorSessionRoundAttemptState* ValidatorSessionRoundAttemptState::try_precommit(
    ValidatorSessionDescription& desc, const ValidatorSessionRoundAttemptState* state, td::uint32 src_idx,
    td::uint32 att, const ValidatorSessionRoundState* round, const ton_api::validatorSession_round_Message* act,
    bool& made) {
  made = false;
  if (state->check_precommit_received_from(src_idx)) {
    return state;
  }
  bool found;
  auto block = state->get_voted_block(desc, found);
  if (!found) {
    return state;
  }
  made = true;
  auto block_id = SentBlock::get_block_id(block);

  if (act) {
    if (act->get_id() != ton_api::validatorSession_message_precommit::ID) {
      VLOG(VALIDATOR_SESSION_WARNING) << "[validator session][node " << desc.get_source_id(src_idx) << "][" << act
                                      << "]: expected PRECOMMIT(" << block_id << ")";
    } else {
      auto x = static_cast<const ton_api::validatorSession_message_precommit*>(act);
      if (x->candidate_ != block_id) {
        VLOG(VALIDATOR_SESSION_WARNING) << "[validator session][node " << desc.get_source_id(src_idx) << "][" << act
                                        << "]: expected PRECOMMIT(" << block_id << ")";
      }
    }
  } else {
    VLOG(VALIDATOR_SESSION_WARNING) << "[validator session][node " << desc.get_source_id(src_idx)
                                    << "]: making implicit PRECOMMIT(" << block_id << ")";
  }

  auto v = CntVector<bool>::change(desc, state->precommitted_, src_idx, true);
  return ValidatorSessionRoundAttemptState::create(desc, state->seqno_, state->votes_, std::move(v), state->vote_for_,
                                                   state->vote_for_inited_);
}

const ValidatorSessionRoundAttemptState* ValidatorSessionRoundAttemptState::make_one(
    ValidatorSessionDescription& desc, const ValidatorSessionRoundAttemptState* state, td::uint32 src_idx,
    td::uint32 att, const ValidatorSessionRoundState* round, const ton_api::validatorSession_round_Message* act,
    bool& made) {
  made = false;
  state = try_vote(desc, state, src_idx, att, round, act, made);
  if (made) {
    return state;
  }
  state = try_precommit(desc, state, src_idx, att, round, act, made);
  if (made) {
    return state;
  }
  if (act && act->get_id() != ton_api::validatorSession_message_empty::ID) {
    VLOG(VALIDATOR_SESSION_WARNING) << "[validator session][node " << desc.get_source_id(src_idx) << "][" << act
                                    << "]: invalid message: expected EMPTY";
  }
  return state;
}

const ValidatorSessionRoundAttemptState* ValidatorSessionRoundAttemptState::action(
    ValidatorSessionDescription& desc, const ValidatorSessionRoundAttemptState* state, td::uint32 src_idx,
    td::uint32 att, const ton_api::validatorSession_round_Message* act, const ValidatorSessionRoundState* round) {
  ton_api::downcast_call(*const_cast<ton_api::validatorSession_round_Message*>(act),
                         [&](auto& obj) { state = action(desc, state, src_idx, att, obj, round); });
  return state;
}

bool ValidatorSessionRoundAttemptState::check_vote_received_from(td::uint32 src_idx) const {
  if (!votes_) {
    return false;
  }
  auto size = votes_->size();
  auto v = votes_->data();
  for (td::uint32 i = 0; i < size; i++) {
    if (v[i]->check_block_is_voted_by(src_idx)) {
      return true;
    }
  }
  return false;
}

bool ValidatorSessionRoundAttemptState::check_precommit_received_from(td::uint32 src_idx) const {
  return precommitted_->at(src_idx);
}

const SentBlock* ValidatorSessionRoundAttemptState::get_voted_block(ValidatorSessionDescription& desc, bool& f) const {
  f = false;
  if (!votes_) {
    return nullptr;
  }

  auto size = votes_->size();
  auto v = votes_->data();
  for (td::uint32 i = 0; i < size; i++) {
    if (v[i]->check_block_is_voted(desc)) {
      f = true;
      return v[i]->get_block();
    }
  }
  return nullptr;
}

bool ValidatorSessionRoundAttemptState::check_attempt_is_precommitted(ValidatorSessionDescription& desc) const {
  ValidatorWeight weight = 0;
  for (td::uint32 i = 0; i < desc.get_total_nodes(); i++) {
    if (precommitted_->at(i)) {
      weight += desc.get_node_weight(i);
      if (weight >= desc.get_cutoff_weight()) {
        return true;
      }
    }
  }
  return false;
}

tl_object_ptr<ton_api::validatorSession_round_Message> ValidatorSessionRoundAttemptState::create_action(
    ValidatorSessionDescription& desc, const ValidatorSessionRoundState* round, td::uint32 src_idx,
    td::uint32 att) const {
  if (!check_vote_received_from(src_idx)) {
    auto found = false;
    auto B = round->choose_block_to_vote(desc, src_idx, att, vote_for_, vote_for_inited_, found);
    if (found) {
      auto block_id = SentBlock::get_block_id(B);
      return create_tl_object<ton_api::validatorSession_message_vote>(round->get_seqno(), seqno_, block_id);
    }
  }
  if (!check_precommit_received_from(src_idx)) {
    bool f = false;
    auto B = get_voted_block(desc, f);

    if (f) {
      auto block_id = SentBlock::get_block_id(B);

      return create_tl_object<ton_api::validatorSession_message_precommit>(round->get_seqno(), seqno_, block_id);
    }
  }

  return create_tl_object<ton_api::validatorSession_message_empty>(round->get_seqno(), seqno_);
}

void ValidatorSessionRoundAttemptState::dump(ValidatorSessionDescription& desc, td::StringBuilder& sb) const {
  sb << "attempt=" << seqno_ << "\n";
  sb << ">>>>\n";

  if (vote_for_inited_) {
    sb << "vote_for=" << (vote_for_ ? vote_for_->get_src_idx() : std::numeric_limits<td::uint32>::max()) << "\n";
  } else {
    sb << "vote_for=NONE\n";
  }

  if (votes_) {
    auto s = votes_->size();
    sb << "votes: ";
    std::vector<td::int32> R;
    R.resize(desc.get_total_nodes(), -1);
    for (td::uint32 i = 0; i < s; i++) {
      const auto e = votes_->at(i);
      const auto& x = e->get_voters_list();
      for (td::uint32 j = 0; j < desc.get_total_nodes(); j++) {
        if (x->at(j)) {
          R[j] = e->get_src_idx();
        }
      }
    }
    for (td::uint32 i = 0; i < desc.get_total_nodes(); i++) {
      sb << R[i] << " ";
    }
    sb << "\n";
  } else {
    sb << "votes: EMPTY\n";
  }

  sb << "precommits: ";
  for (td::uint32 i = 0; i < desc.get_total_nodes(); i++) {
    const auto e = precommitted_->at(i);
    if (e) {
      sb << "+ ";
    } else {
      sb << "- ";
    }
  }
  sb << "\n";
  sb << "<<<<\n";
}

//
//
// ROUND STATE
//
//

const ValidatorSessionRoundState* ValidatorSessionRoundState::merge(ValidatorSessionDescription& desc,
                                                                    const ValidatorSessionRoundState* left,
                                                                    const ValidatorSessionRoundState* right) {
  if (!left) {
    return right;
  }
  if (!right) {
    return left;
  }
  if (left == right) {
    return left;
  }
  CHECK(left->seqno_ == right->seqno_);

  if (left->precommitted_ && right->precommitted_) {
    CHECK(SentBlock::get_block_id(left->precommitted_block_) == SentBlock::get_block_id(right->precommitted_block_));
  }

  const SentBlock* precommitted_block =
      left->precommitted_block_ ? left->precommitted_block_ : right->precommitted_block_;

  bool precommitted = left->precommitted_ || right->precommitted_;

  auto first_attempt =
      CntVector<td::uint32>::merge(desc, left->first_attempt_, right->first_attempt_, [&](td::uint32 l, td::uint32 r) {
        if (l == 0) {
          return r;
        }
        if (r == 0) {
          return l;
        }
        return std::min(l, r);
      });

  auto att_vec =
      AttemptVector::merge(desc, left->attempts_, right->attempts_,
                           [&](const ValidatorSessionRoundAttemptState* l, const ValidatorSessionRoundAttemptState* r) {
                             return ValidatorSessionRoundAttemptState::merge(desc, l, r);
                           });

  const SentBlock* f[2];
  td::uint32 f_att[2] = {};
  td::uint32 f_cnt = 0;

  for (td::uint32 i = att_vec->size(); i > 0; i--) {
    auto b = att_vec->at(i - 1);
    if (f_cnt <= 1) {
      bool found;
      auto B = b->get_voted_block(desc, found);
      if (found) {
        if (f_cnt == 1) {
          auto l = SentBlock::get_block_id(f[0]);
          auto r = SentBlock::get_block_id(B);
          if (l == r) {
            found = false;
          }
        }
        if (found) {
          f[f_cnt] = B;
          f_att[f_cnt] = b->get_seqno();
          f_cnt++;
        }
      }
    }
    if (!precommitted && b->check_attempt_is_precommitted(desc)) {
      precommitted = true;
      bool found;
      precommitted_block = b->get_voted_block(desc, found);
      CHECK(found);
    }
    if (precommitted && f_cnt == 2) {
      break;
    }
  }

  if (f_cnt >= 1) {
    CHECK(f_att[0] > f_att[1]);
  }

  auto last_precommit = CntVector<td::uint32>::merge(
      desc, left->last_precommit_, right->last_precommit_,
      [&](td::uint32 l, td::uint32 r) -> td::uint32 {
        auto x = std::max(l, r);
        if (f_cnt == 0) {
          CHECK(x == 0);
          return x;
        }
        if (x > f_att[1]) {
          return x;
        } else {
          return 0;
        }
      },
      true);

  auto signs = CntVector<const SessionBlockCandidateSignature*>::merge(
      desc, left->signatures_, right->signatures_,
      [&](const SessionBlockCandidateSignature* l, const SessionBlockCandidateSignature* r) {
        return SessionBlockCandidateSignature::merge(desc, l, r);
      });

  //auto sent_vec = SentBlockVector::merge(desc, left->sent_blocks_, right->sent_blocks_);

  auto sent = ApproveVector::merge(desc, left->sent_blocks_, right->sent_blocks_,
                                   [&](const SessionBlockCandidate* l, const SessionBlockCandidate* r) {
                                     return SessionBlockCandidate::merge(desc, l, r);
                                   });

  return ValidatorSessionRoundState::create(desc, precommitted_block, left->seqno_, precommitted,
                                            std::move(first_attempt), last_precommit, std::move(sent), std::move(signs),
                                            std::move(att_vec));
}

const ValidatorSessionRoundState* ValidatorSessionRoundState::action(
    ValidatorSessionDescription& desc, const ValidatorSessionRoundState* state, td::uint32 src_idx, td::uint32 att,
    const ton_api::validatorSession_message_submittedBlock& act) {
  if (desc.get_node_priority(src_idx, state->seqno_) < 0) {
    VLOG(VALIDATOR_SESSION_WARNING) << "[validator session][node " << desc.get_source_id(src_idx) << "][" << act
                                    << "]: invalid message: cannot propose blocks on this round";
    return state;
  }

  if (state->check_block_is_sent_by(src_idx)) {
    VLOG(VALIDATOR_SESSION_WARNING) << "[validator session][node " << desc.get_source_id(src_idx) << "][" << act
                                    << "]: invalid message: duplicate block propose";
    return state;
  }

  auto b = SentBlock::create(desc, src_idx, act.root_hash_, act.file_hash_, act.collated_data_file_hash_);
  auto a = SessionBlockCandidate::create(desc, b);

  auto sent = ApproveVector::push(desc, state->sent_blocks_, a);

  return ValidatorSessionRoundState::create(desc, state->precommitted_block_, state->seqno_, state->precommitted_,
                                            state->first_attempt_, state->last_precommit_, sent, state->signatures_,
                                            state->attempts_);
}

const ValidatorSessionRoundState* ValidatorSessionRoundState::action(
    ValidatorSessionDescription& desc, const ValidatorSessionRoundState* state, td::uint32 src_idx, td::uint32 att,
    const ton_api::validatorSession_message_approvedBlock& act) {
  auto sent_block = state->get_block(act.candidate_);
  if (act.candidate_ != skip_round_candidate_id() && !sent_block) {
    VLOG(VALIDATOR_SESSION_WARNING) << "[validator session][node " << desc.get_source_id(src_idx) << "][" << act
                                    << "]: invalid message: approved unknown block " << act.candidate_;
    return state;
  }

  if (sent_block && sent_block->check_block_is_approved_by(src_idx)) {
    VLOG(VALIDATOR_SESSION_WARNING) << "[validator session][node " << desc.get_source_id(src_idx) << "][" << act
                                    << "]: invalid message: duplicate block " << act.candidate_ << " approve";
    return state;
  }

  if (act.candidate_ != skip_round_candidate_id()) {
    auto v = state->sent_blocks_->data();
    auto s = state->sent_blocks_->size();
    for (td::uint32 i = 0; i < s; i++) {
      auto B = v[i];
      if (B->get_src_idx() == sent_block->get_src_idx() && B->check_block_is_approved_by(src_idx)) {
        VLOG(VALIDATOR_SESSION_WARNING)
            << "[validator session][node " << desc.get_source_id(src_idx) << "][" << act
            << "]: invalid message: approved block, but already approved another from this node";
        return state;
      }
    }
  }

  if (act.candidate_ == skip_round_candidate_id()) {
    if (act.signature_.size() != 0) {
      VLOG(VALIDATOR_SESSION_WARNING) << "[validator session][node " << desc.get_source_id(src_idx) << "][" << act
                                      << "]: bad signature in APPROVE. Ignoring";
      return state;
    }
  } else {
    auto S = desc.check_approve_signature(sent_block->get_block()->get_root_hash(),
                                          sent_block->get_block()->get_file_hash(), src_idx, act.signature_.as_slice());
    if (S.is_error()) {
      VLOG(VALIDATOR_SESSION_WARNING) << "[validator session][node " << desc.get_source_id(src_idx) << "][" << act
                                      << "]: bad signature in APPROVE. Ignoring: " << S;
      return state;
    }
  }

  if (!sent_block) {
    CHECK(act.candidate_ == skip_round_candidate_id());
    sent_block = SessionBlockCandidate::create(desc, nullptr);
  }
  sent_block = SessionBlockCandidate::push(desc, sent_block, src_idx,
                                           SessionBlockCandidateSignature::create(desc, act.signature_.clone()));
  auto v = ApproveVector::push(desc, state->sent_blocks_, sent_block);

  return ValidatorSessionRoundState::create(desc, state->precommitted_block_, state->seqno_, state->precommitted_,
                                            state->first_attempt_, state->last_precommit_, std::move(v),
                                            state->signatures_, state->attempts_);
}

const ValidatorSessionRoundState* ValidatorSessionRoundState::action(
    ValidatorSessionDescription& desc, const ValidatorSessionRoundState* state, td::uint32 src_idx, td::uint32 att,
    const ton_api::validatorSession_message_rejectedBlock& act) {
  LOG(ERROR) << "VALIDATOR SESSION: NODE " << desc.get_source_id(src_idx) << " REJECTED CANDIDATE " << act.candidate_
             << " WITH REASON " << act.reason_.as_slice();
  return state;
}

const ValidatorSessionRoundState* ValidatorSessionRoundState::action(
    ValidatorSessionDescription& desc, const ValidatorSessionRoundState* state, td::uint32 src_idx, td::uint32 att,
    const ton_api::validatorSession_message_commit& act) {
  if (!state->precommitted_) {
    VLOG(VALIDATOR_SESSION_WARNING) << "[validator session][node " << desc.get_source_id(src_idx) << "][" << act
                                    << "]: invalid message: committing not precommitted block";
    return state;
  }
  auto block_id = SentBlock::get_block_id(state->precommitted_block_);
  if (block_id != act.candidate_) {
    VLOG(VALIDATOR_SESSION_WARNING) << "[validator session][node " << desc.get_source_id(src_idx) << "][" << act
                                    << "]: invalid message: committing wrong block " << act.candidate_;
    return state;
  }

  if (state->signatures_->at(src_idx)) {
    VLOG(VALIDATOR_SESSION_WARNING) << "[validator session][node " << desc.get_source_id(src_idx) << "][" << act
                                    << "]: invalid message: duplicate signature";
    return state;
  }

  if (act.candidate_ == skip_round_candidate_id()) {
    if (act.signature_.size() != 0) {
      VLOG(VALIDATOR_SESSION_WARNING) << "[validator session][node " << desc.get_source_id(src_idx) << "][" << act
                                      << "]: bad signature in COMMIT. Ignoring";
      return state;
    }
  } else {
    auto S = desc.check_signature(state->precommitted_block_->get_root_hash(),
                                  state->precommitted_block_->get_file_hash(), src_idx, act.signature_.as_slice());
    if (S.is_error()) {
      VLOG(VALIDATOR_SESSION_WARNING) << "[validator session][node " << desc.get_source_id(src_idx) << "][" << act
                                      << "]: invalid message: bad signature: " << S.move_as_error();
      return state;
    }
  }

  auto s = CntVector<const SessionBlockCandidateSignature*>::change(
      desc, state->signatures_, src_idx, SessionBlockCandidateSignature::create(desc, act.signature_.clone()));

  return ValidatorSessionRoundState::create(desc, state->precommitted_block_, state->seqno_, state->precommitted_,
                                            state->first_attempt_, state->last_precommit_, state->sent_blocks_,
                                            std::move(s), state->attempts_);
}

const ValidatorSessionRoundState* ValidatorSessionRoundState::forward_action_to_attempt(
    ValidatorSessionDescription& desc, const ValidatorSessionRoundState* state, td::uint32 src_idx, td::uint32 att,
    const ton_api::validatorSession_round_Message* act) {
  if (state->precommitted_) {
    if (act->get_id() != ton_api::validatorSession_message_empty::ID) {
      VLOG(VALIDATOR_SESSION_WARNING) << "[validator session][node " << desc.get_source_id(src_idx) << "][" << *act
                                      << "]: invalid message in precommitted round: expected EMPTY";
    }
    return state;
  }
  auto attempt = get_attempt(state->attempts_, att);
  if (!attempt) {
    attempt = ValidatorSessionRoundAttemptState::create(desc, att);
  }

  bool had_voted_block;
  attempt->get_voted_block(desc, had_voted_block);

  ton_api::downcast_call(*const_cast<ton_api::validatorSession_round_Message*>(act), [&](auto& obj) {
    attempt = ValidatorSessionRoundAttemptState::action(desc, attempt, src_idx, att, obj, state);
  });

  bool has_voted_block;
  auto block = attempt->get_voted_block(desc, has_voted_block);

  auto precommitted = state->precommitted_;
  auto precommitted_block = state->precommitted_block_;
  if (!precommitted && attempt->check_attempt_is_precommitted(desc)) {
    precommitted = true;
    CHECK(has_voted_block);
    precommitted_block = block;
  }

  auto last_precommit = state->last_precommit_;

  td::uint32 e = 0;
  for (td::uint32 i = 0; i < last_precommit->size(); i++) {
    e = std::max(e, last_precommit->at(i));
  }

  bool eq = true;
  if (e && has_voted_block) {
    auto attempt = get_attempt(state->attempts_, e);
    CHECK(attempt);
    bool f;
    auto B = attempt->get_voted_block(desc, f);
    CHECK(f);
    auto l = SentBlock::get_block_id(block);
    auto r = SentBlock::get_block_id(B);
    eq = (l == r);

    if (!eq) {
      last_precommit = CntVector<td::uint32>::modify(desc, last_precommit, [&](td::uint32 a) -> td::uint32 {
        if (a > att) {
          return a;
        } else {
          return 0;
        }
      });
    }
  }

  if (attempt->check_precommit_received_from(src_idx) && last_precommit->at(src_idx) < att && (att > e || eq)) {
    CHECK(has_voted_block);
    last_precommit = CntVector<td::uint32>::change(desc, last_precommit, src_idx, att);
  }

  auto vec = AttemptVector::push(desc, state->attempts_, std::move(attempt));
  return ValidatorSessionRoundState::create(desc, precommitted_block, state->seqno_, precommitted,
                                            state->first_attempt_, last_precommit, state->sent_blocks_,
                                            state->signatures_, std::move(vec));
}

const ValidatorSessionRoundState* ValidatorSessionRoundState::action(
    ValidatorSessionDescription& desc, const ValidatorSessionRoundState* state, td::uint32 src_idx, td::uint32 att,
    const ton_api::validatorSession_round_Message* act) {
  auto first_attempt = state->first_attempt_;
  if (first_attempt->at(src_idx) == 0 || first_attempt->at(src_idx) > att) {
    first_attempt = CntVector<td::uint32>::change(desc, first_attempt, src_idx, att);
    state = ValidatorSessionRoundState::create(desc, state->precommitted_block_, state->seqno_, state->precommitted_,
                                               first_attempt, state->last_precommit_, state->sent_blocks_,
                                               state->signatures_, state->attempts_);
  }

  ton_api::downcast_call(*const_cast<ton_api::validatorSession_round_Message*>(act),
                         [&](auto& obj) { state = action(desc, state, src_idx, att, obj); });
  return state;
}

bool ValidatorSessionRoundState::check_block_is_sent_by(td::uint32 src_idx) const {
  if (!sent_blocks_) {
    return false;
  }
  auto v = sent_blocks_->data();
  auto size = sent_blocks_->size();
  for (td::uint32 i = 0; i < size; i++) {
    auto B = v[i]->get_block();
    if (B && B->get_src_idx() == src_idx) {
      return true;
    }
  }
  return false;
}

bool ValidatorSessionRoundState::check_block_is_signed(ValidatorSessionDescription& desc) const {
  ValidatorWeight weight = 0;
  for (td::uint32 i = 0; i < desc.get_total_nodes(); i++) {
    if (signatures_->at(i)) {
      weight += desc.get_node_weight(i);
      if (weight >= desc.get_cutoff_weight()) {
        return true;
      }
    }
  }
  return false;
}

bool ValidatorSessionRoundState::check_need_generate_vote_for(ValidatorSessionDescription& desc, td::uint32 src_idx,
                                                              td::uint32 att) const {
  if (src_idx != desc.get_vote_for_author(att)) {
    return false;
  }
  if (precommitted_) {
    return false;
  }
  if (get_first_attempt(src_idx) == 0 && desc.opts().max_round_attempts > 0) {
    return false;
  }
  if (get_first_attempt(src_idx) + desc.opts().max_round_attempts > att && desc.opts().max_round_attempts > 0) {
    return false;
  }
  auto attempt = get_attempt(attempts_, att);
  if (attempt) {
    bool vote_for_inited;
    attempt->get_vote_for_block(desc, vote_for_inited);
    if (attempt && vote_for_inited) {
      return false;
    }
  }
  if (!sent_blocks_) {
    return false;
  }

  for (td::uint32 i = 0; i < sent_blocks_->size(); i++) {
    auto B = sent_blocks_->at(i);
    if (B->check_block_is_approved(desc)) {
      return true;
    }
  }

  return false;
}

tl_object_ptr<ton_api::validatorSession_message_voteFor> ValidatorSessionRoundState::generate_vote_for(
    ValidatorSessionDescription& desc, td::uint32 src_idx, td::uint32 att) const {
  CHECK(src_idx == desc.get_vote_for_author(att));
  std::vector<ValidatorSessionCandidateId> v;
  for (td::uint32 i = 0; i < sent_blocks_->size(); i++) {
    auto B = sent_blocks_->at(i);
    if (B->check_block_is_approved(desc)) {
      v.push_back(B->get_id());
    }
  }
  CHECK(v.size() > 0);
  td::uint32 x = td::Random::secure_uint32();
  return create_tl_object<ton_api::validatorSession_message_voteFor>(get_seqno(), static_cast<td::int32>(att),
                                                                     v[x % v.size()]);
}

const SentBlock* ValidatorSessionRoundState::choose_block_to_vote(ValidatorSessionDescription& desc, td::uint32 src_idx,
                                                                  td::uint32 att, const SentBlock* vote_for,
                                                                  bool vote_for_inited, bool& found) const {
  found = false;
  if (!sent_blocks_) {
    return nullptr;
  }
  if (last_precommit_->at(src_idx) > 0) {
    auto attempt = get_attempt(attempts_, last_precommit_->at(src_idx));
    CHECK(attempt);
    bool f;
    auto B = attempt->get_voted_block(desc, f);
    CHECK(f);
    found = true;
    return B;
  }

  auto t = first_attempt_->at(src_idx);
  auto slow_mode = (t > 0 && t + desc.opts().max_round_attempts <= att) || desc.opts().max_round_attempts == 0;

  if (!slow_mode) {
    bool f = false;
    const SentBlock* block = nullptr;

    auto size = attempts_ ? attempts_->size() : 0;
    for (td::uint32 i = size; i > 0; i--) {
      auto attempt = attempts_->at(i - 1);
      CHECK(attempt);
      block = attempt->get_voted_block(desc, f);
      if (f) {
        break;
      }
    }
    if (f) {
      found = true;
      return block;
    }

    td::int32 min_priority = desc.get_max_priority() + 2;
    for (td::uint32 i = 0; i < sent_blocks_->size(); i++) {
      auto B = sent_blocks_->at(i);
      if (!B->check_block_is_approved(desc)) {
        continue;
      }
      td::int32 e = B->get_block() ? desc.get_node_priority(B->get_src_idx(), seqno_) : desc.get_max_priority() + 1;
      CHECK(e >= 0);
      if (e < min_priority) {
        min_priority = e;
        block = B->get_block();
      }
    }

    found = min_priority < static_cast<td::int32>(desc.get_max_priority() + 2);
    return block;
  }

  found = vote_for_inited;
  return vote_for;
}

bool ValidatorSessionRoundState::check_block_is_approved_by(td::uint32 src_idx,
                                                            ValidatorSessionCandidateId block_id) const {
  if (!sent_blocks_) {
    return false;
  }
  auto size = sent_blocks_->size();
  auto v = sent_blocks_->data();
  for (td::uint32 i = 0; i < size; i++) {
    if (v[i]->get_id() == block_id) {
      return v[i]->check_block_is_approved_by(src_idx);
    }
  }
  return false;
}

tl_object_ptr<ton_api::validatorSession_round_Message> ValidatorSessionRoundState::create_action(
    ValidatorSessionDescription& desc, td::uint32 src_idx, td::uint32 att) const {
  if (precommitted_) {
    return create_tl_object<ton_api::validatorSession_message_empty>(seqno_, att);
  }

  auto attempt = get_attempt(attempts_, att);
  if (attempt) {
    return attempt->create_action(desc, this, src_idx, att);
  }

  bool found = false;
  auto block = choose_block_to_vote(desc, src_idx, att, nullptr, false, found);
  if (!found) {
    return create_tl_object<ton_api::validatorSession_message_empty>(seqno_, att);
  }
  auto block_id = SentBlock::get_block_id(block);

  return create_tl_object<ton_api::validatorSession_message_vote>(seqno_, att, block_id);
}

const SentBlock* ValidatorSessionRoundState::choose_block_to_sign(ValidatorSessionDescription& desc, td::uint32 src_idx,
                                                                  bool& found) const {
  found = false;
  if (!precommitted_) {
    return nullptr;
  }
  found = true;
  return precommitted_block_;
}

const ValidatorSessionRoundState* ValidatorSessionRoundState::make_one(ValidatorSessionDescription& desc,
                                                                       const ValidatorSessionRoundState* state,
                                                                       td::uint32 src_idx, td::uint32 att, bool& made) {
  made = false;
  auto first_attempt = state->first_attempt_;
  if (first_attempt->at(src_idx) == 0 || first_attempt->at(src_idx) > att) {
    made = true;
    first_attempt = CntVector<td::uint32>::change(desc, state->first_attempt_, src_idx, att);
    state = ValidatorSessionRoundState::create(desc, state->precommitted_block_, state->seqno_, state->precommitted_,
                                               first_attempt, state->last_precommit_, state->sent_blocks_,
                                               state->signatures_, state->attempts_);
  }
  if (state->precommitted_) {
    return state;
  }
  auto attempt = get_attempt(state->attempts_, att);
  if (!attempt) {
    attempt = ValidatorSessionRoundAttemptState::create(desc, att);
  }

  bool m;
  auto a = ValidatorSessionRoundAttemptState::make_one(desc, attempt, src_idx, att, state, nullptr, m);
  if (!m) {
    return state;
  }
  made = true;

  auto att_vec = AttemptVector::push(desc, state->attempts_, std::move(a));

  return ValidatorSessionRoundState::create(desc, state->precommitted_block_, state->seqno_, state->precommitted_,
                                            state->first_attempt_, state->last_precommit_, state->sent_blocks_,
                                            state->signatures_, std::move(att_vec));
}

std::vector<const SentBlock*> ValidatorSessionRoundState::choose_blocks_to_approve(ValidatorSessionDescription& desc,
                                                                                   td::uint32 src_idx) const {
  if (!sent_blocks_) {
    return {nullptr};
  }

  std::set<td::uint32> was_source;

  std::vector<const SessionBlockCandidate*> x;
  x.resize(desc.get_max_priority() + 2, nullptr);

  bool was_empty = false;

  for (td::uint32 i = 0; i < sent_blocks_->size(); i++) {
    auto B = sent_blocks_->at(i);
    if (!B->get_block()) {
      was_empty = B->check_block_is_approved_by(src_idx);
      continue;
    }
    td::int32 prio = desc.get_node_priority(B->get_src_idx(), seqno_);
    CHECK(prio >= 0);
    td::uint32 blk_src_idx = B->get_src_idx();
    if (was_source.count(blk_src_idx) > 0) {
      x[prio] = nullptr;
    } else {
      was_source.insert(blk_src_idx);
      if (!B->check_block_is_approved_by(src_idx)) {
        x[prio] = B;
      }
    }
  }

  std::vector<const SentBlock*> res;
  for (auto& B : x) {
    if (B) {
      res.push_back(B->get_block());
    }
  }
  if (!was_empty) {
    res.push_back(nullptr);
  }

  return res;
}

const SessionBlockCandidate* ValidatorSessionRoundState::get_block(ValidatorSessionCandidateId block_hash) const {
  if (!sent_blocks_) {
    return nullptr;
  }
  return get_candidate(sent_blocks_, block_hash);
}

std::vector<const SentBlock*> ValidatorSessionRoundState::get_blocks_approved_by(ValidatorSessionDescription& desc,
                                                                                 td::uint32 src_idx) const {
  if (!sent_blocks_) {
    return {};
  }
  std::vector<const SentBlock*> res;
  for (td::uint32 i = 0; i < sent_blocks_->size(); i++) {
    auto B = sent_blocks_->at(i);
    if (B->check_block_is_approved_by(src_idx)) {
      res.push_back(B->get_block());
    }
  }
  return res;
}

std::vector<td::uint32> ValidatorSessionRoundState::get_block_approvers(ValidatorSessionDescription& desc,
                                                                        ValidatorSessionCandidateId block) const {
  auto B = get_candidate(sent_blocks_, block);
  if (!B) {
    return {};
  }
  std::vector<td::uint32> v;
  for (td::uint32 i = 0; i < desc.get_total_nodes(); i++) {
    if (B->check_block_is_approved_by(i)) {
      v.push_back(i);
    }
  }
  return v;
}

//
//
// STATE
//
//

const ValidatorSessionState* ValidatorSessionState::merge(ValidatorSessionDescription& desc,
                                                          const ValidatorSessionState* left,
                                                          const ValidatorSessionState* right) {
  if (!left) {
    return right;
  }
  if (!right) {
    return left;
  }
  if (left == right) {
    return left;
  }
  CHECK(left->att_->size() == desc.get_total_nodes());
  CHECK(right->att_->size() == desc.get_total_nodes());

  auto ts = CntVector<td::uint32>::merge(desc, left->att_, right->att_,
                                         [](td::uint32 l, td::uint32 r) { return std::max(l, r); });

  auto old = CntVector<const ValidatorSessionOldRoundState*>::merge(
      desc, left->old_rounds_, right->old_rounds_,
      [&](const ValidatorSessionOldRoundState* l, const ValidatorSessionOldRoundState* r) {
        return ValidatorSessionOldRoundState::merge(desc, l, r);
      });

  const ValidatorSessionRoundState* round;
  if (left->cur_round_->get_seqno() < right->cur_round_->get_seqno()) {
    auto r = old->at(left->cur_round_->get_seqno());
    old = CntVector<const ValidatorSessionOldRoundState*>::change(
        desc, old, left->cur_round_->get_seqno(), ValidatorSessionOldRoundState::merge(desc, r, left->cur_round_));
    round = right->cur_round_;
  } else if (left->cur_round_->get_seqno() > right->cur_round_->get_seqno()) {
    auto r = old->at(right->cur_round_->get_seqno());
    old = CntVector<const ValidatorSessionOldRoundState*>::change(
        desc, old, right->cur_round_->get_seqno(), ValidatorSessionOldRoundState::merge(desc, r, right->cur_round_));
    round = left->cur_round_;
  } else {
    round = ValidatorSessionRoundState::merge(desc, left->cur_round_, right->cur_round_);
  }

  if (round->check_block_is_signed(desc)) {
    old = CntVector<const ValidatorSessionOldRoundState*>::push(desc, old, round->get_seqno(),
                                                                ValidatorSessionOldRoundState::create(desc, round));
    round = ValidatorSessionRoundState::create(desc, round->get_seqno() + 1);
  }

  return ValidatorSessionState::create(desc, std::move(ts), std::move(old), std::move(round));
}

const ValidatorSessionState* ValidatorSessionState::action(ValidatorSessionDescription& desc,
                                                           const ValidatorSessionState* state, td::uint32 src_idx,
                                                           td::uint32 att,
                                                           const ton_api::validatorSession_round_Message* action) {
  if (action) {
    VLOG(VALIDATOR_SESSION_DEBUG) << "[validator session][node " << desc.get_source_id(src_idx) << "][" << action
                                  << "]: applying message " << *action;
  }
  if (att < state->att_->at(src_idx)) {
    VLOG(VALIDATOR_SESSION_WARNING) << "[validator session][node " << desc.get_source_id(src_idx) << "][" << action
                                    << "]: invalid message: bad ts: times goes back: " << state->att_->at(src_idx)
                                    << "->" << att;
    att = state->att_->at(src_idx);
  }
  if (att < 1024) {
    VLOG(VALIDATOR_SESSION_WARNING) << "[validator session][node " << desc.get_source_id(src_idx) << "][" << action
                                    << "]: invalid message: bad ts: too small: " << att;
    att = 1024;
  }

  auto ts_vec = CntVector<td::uint32>::change(desc, state->att_, src_idx, att);

  auto round_id = get_round_id(action);
  if (round_id > state->cur_round_->get_seqno()) {
    VLOG(VALIDATOR_SESSION_WARNING) << "[validator session][node " << desc.get_source_id(src_idx) << "][" << action
                                    << "]: too big round id";
    return ValidatorSessionState::create(desc, std::move(ts_vec), state->old_rounds_, state->cur_round_);
  }
  if (round_id == state->cur_round_->get_seqno()) {
    auto round = ValidatorSessionRoundState::action(desc, state->cur_round_, src_idx, att, std::move(action));

    auto old = state->old_rounds_;
    if (round->check_block_is_signed(desc)) {
      old = CntVector<const ValidatorSessionOldRoundState*>::push(desc, old, round->get_seqno(),
                                                                  ValidatorSessionOldRoundState::create(desc, round));
      round = ValidatorSessionRoundState::create(desc, round->get_seqno() + 1);
    }

    return ValidatorSessionState::create(desc, std::move(ts_vec), std::move(old), std::move(round));
  } else {
    auto old = state->old_rounds_;
    old = CntVector<const ValidatorSessionOldRoundState*>::change(
        desc, old, round_id,
        ValidatorSessionOldRoundState::action(desc, old->at(round_id), src_idx, att, std::move(action)));

    return ValidatorSessionState::create(desc, std::move(ts_vec), std::move(old), state->cur_round_);
  }
}

tl_object_ptr<ton_api::validatorSession_round_Message> ValidatorSessionState::create_action(
    ValidatorSessionDescription& desc, td::uint32 src_idx, td::uint32 att) const {
  return cur_round_->create_action(desc, src_idx, att);
}

const SentBlock* ValidatorSessionState::choose_block_to_sign(ValidatorSessionDescription& desc, td::uint32 src_idx,
                                                             bool& found) const {
  found = false;
  if (cur_round_->check_block_is_signed_by(src_idx)) {
    return nullptr;
  }
  return cur_round_->choose_block_to_sign(desc, src_idx, found);
}

const ValidatorSessionState* ValidatorSessionState::make_one(ValidatorSessionDescription& desc,
                                                             const ValidatorSessionState* state, td::uint32 src_idx,
                                                             td::uint32 att, bool& made) {
  made = false;
  if (att < state->att_->at(src_idx)) {
    VLOG(VALIDATOR_SESSION_WARNING) << "[validator session][node " << desc.get_source_id(src_idx)
                                    << "]: time goes back, using old (bigger) value ";
    att = state->att_->at(src_idx);
  }
  if (att < 1024) {
    VLOG(VALIDATOR_SESSION_WARNING) << "[validator session][node " << desc.get_source_id(src_idx)
                                    << "] bad ts: too small: " << att;
    att = 1024;
  }

  bool upd_time = false;
  auto ts_vec = state->att_;
  if (ts_vec->at(src_idx) < att) {
    upd_time = true;
    ts_vec = CntVector<td::uint32>::change(desc, ts_vec, src_idx, att);
    VLOG(VALIDATOR_SESSION_WARNING) << "[validator session][node " << desc.get_source_id(src_idx)
                                    << "]: updating time in make_all()";
  }

  auto round = ValidatorSessionRoundState::make_one(desc, state->cur_round_, src_idx, att, made);
  if (!made && !upd_time) {
    return state;
  }
  made = true;
  CHECK(!round->check_block_is_signed(desc));

  return ValidatorSessionState::create(desc, std::move(ts_vec), state->old_rounds_, std::move(round));
}

const SentBlock* ValidatorSessionState::get_committed_block(ValidatorSessionDescription& desc, td::uint32 seqno) const {
  if (seqno < old_rounds_->size()) {
    return old_rounds_->at(seqno)->get_block();
  } else {
    return nullptr;
  }
}

const SentBlock* ValidatorSessionState::get_block(ValidatorSessionDescription& desc, ValidatorSessionCandidateId id,
                                                  bool& found) const {
  auto B = cur_round_->get_block(id);

  if (!B) {
    found = false;
    return nullptr;
  } else {
    found = true;
    return B->get_block();
  }
}

std::vector<const SentBlock*> ValidatorSessionState::get_blocks_approved_by(ValidatorSessionDescription& desc,
                                                                            td::uint32 src_idx) const {
  return cur_round_->get_blocks_approved_by(desc, src_idx);
}

const CntVector<const SessionBlockCandidateSignature*>* ValidatorSessionState::get_committed_block_signatures(
    ValidatorSessionDescription& desc, td::uint32 seqno) const {
  if (seqno < old_rounds_->size()) {
    return old_rounds_->at(seqno)->get_signatures();
  } else {
    return nullptr;
  }
}

const CntVector<const SessionBlockCandidateSignature*>* ValidatorSessionState::get_committed_block_approve_signatures(
    ValidatorSessionDescription& desc, td::uint32 seqno) const {
  if (seqno < old_rounds_->size()) {
    return old_rounds_->at(seqno)->get_approve_signatures();
  } else {
    return nullptr;
  }
}

void ValidatorSessionState::dump(ValidatorSessionDescription& desc, td::StringBuilder& sb, td::uint32 att) const {
  cur_round_->dump(desc, sb, att);
}

void ValidatorSessionRoundState::dump_cur_attempt(ValidatorSessionDescription& desc, td::StringBuilder& sb) const {
  dump(desc, sb, desc.get_attempt_seqno(desc.get_ts()));
}

void ValidatorSessionRoundState::dump(ValidatorSessionDescription& desc, td::StringBuilder& sb, td::uint32 att) const {
  sb << "round_id=" << seqno_ << " total_weight=" << desc.get_total_weight()
     << " cutoff_weight=" << desc.get_cutoff_weight() << " precommitted=" << precommitted_ << "\n";
  sb << "sent blocks:>>>>\n";
  if (sent_blocks_) {
    const auto& v = sent_blocks_->data();
    auto size = sent_blocks_->size();
    for (td::uint32 i = 0; i < size; i++) {
      const auto& el = v[i];
      const auto& b = el->get_block();
      auto priority = b ? desc.get_node_priority(el->get_src_idx(), seqno_) : desc.get_max_priority() + 1;
      const auto& x = el->get_approvers_list();
      ValidatorWeight cnt = 0;
      if (x) {
        auto s = desc.get_total_nodes();
        for (td::uint32 j = 0; j < s; j++) {
          if (x->at(j)) {
            cnt += desc.get_node_weight(j);
          }
        }
      }
      if (b) {
        sb << "  block hash=" << SentBlock::get_block_id(b) << " root_hash=" << b->get_root_hash()
           << " file_hash=" << b->get_file_hash() << " approved=" << cnt << " priority=" << priority << "\n";
      } else {
        sb << "  SKIP block approved=" << cnt << "\n";
      }
    }
  }
  sb << "  first attempt: ";
  for (td::uint32 i = 0; i < desc.get_total_nodes(); i++) {
    sb << first_attempt_->at(i) << " ";
  }
  sb << "\n";
  sb << "<<<<\n";
  auto attempt = get_attempt(attempts_, att);
  if (attempt) {
    attempt->dump(desc, sb);
  }
}

}  // namespace validatorsession

}  // namespace ton
