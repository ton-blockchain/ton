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
#include "td/utils/Random.h"
#include "auto/tl/ton_api.hpp"

#include <set>

namespace ton {

namespace validatorsession {

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
  if (round->get_first_attempt(src_idx) + desc.opts().max_round_attempts > att && desc.opts().max_round_attempts > 0) {
    VLOG(VALIDATOR_SESSION_WARNING) << "[validator session][node " << desc.get_source_id(src_idx) << "][" << act
                                    << "]: invalid message: too early for VOTEFOR";
    return state;
  }

  auto x = round->get_block(act.candidate_);

  if (!x) {
    VLOG(VALIDATOR_SESSION_WARNING) << "[validator session][node " << desc.get_source_id(src_idx) << "][" << act
                                    << "]: invalid message: VOTEFOR for not submitted block";
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


}  // namespace validatorsession

}  // namespace ton
