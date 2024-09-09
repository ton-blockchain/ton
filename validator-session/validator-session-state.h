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
#pragma once

#include "td/utils/int_types.h"
#include "td/utils/buffer.h"
#include "adnl/utils.hpp"
#include "common/io.hpp"

#include "persistent-vector.h"
#include "validator-session-description.h"
#include "validator-session-types.h"
#include "validator-session-round-attempt-state.h"

#include <limits>

namespace td {

td::StringBuilder& operator<<(td::StringBuilder& sb, const ton::ton_api::validatorSession_round_Message& message);
td::StringBuilder& operator<<(td::StringBuilder& sb, const ton::ton_api::validatorSession_round_Message* message);

}

namespace ton {

namespace validatorsession {

class ValidatorSessionOldRoundState : public ValidatorSessionDescription::RootObject {
 public:
  static HashType create_hash(ValidatorSessionDescription& desc, td::uint32 seqno, HashType block, HashType signatures,
                              HashType approve_signatures) {
    auto obj =
        create_tl_object<ton_api::hashable_validatorSessionOldRound>(seqno, block, signatures, approve_signatures);
    return desc.compute_hash(serialize_tl_object(obj, true).as_slice());
  }
  static bool compare(const RootObject* r, td::uint32 seqno, const SentBlock* block,
                      const SessionBlockCandidateSignatureVector* signatures,
                      const SessionBlockCandidateSignatureVector* approve_signatures, HashType hash) {
    if (!r || r->get_size() < sizeof(ValidatorSessionOldRoundState)) {
      return false;
    }
    auto R = static_cast<const ValidatorSessionOldRoundState*>(r);
    return R->seqno_ == seqno && R->block_ == block && R->signatures_ == signatures &&
           R->approve_signatures_ == approve_signatures && R->hash_ == hash;
  }
  static auto lookup(ValidatorSessionDescription& desc, td::uint32 seqno, const SentBlock* block,
                     const SessionBlockCandidateSignatureVector* signatures,
                     const SessionBlockCandidateSignatureVector* approve_signatures, HashType hash, bool temp) {
    auto r = desc.get_by_hash(hash, temp);
    if (compare(r, seqno, block, signatures, approve_signatures, hash)) {
      desc.on_reuse();
      return static_cast<const ValidatorSessionOldRoundState*>(r);
    }
    return static_cast<const ValidatorSessionOldRoundState*>(nullptr);
  }
  static const ValidatorSessionOldRoundState* move_to_persistent(ValidatorSessionDescription& desc,
                                                                 const ValidatorSessionOldRoundState* b) {
    if (desc.is_persistent(b)) {
      return b;
    }
    auto signatures = SessionBlockCandidateSignatureVector::move_to_persistent(desc, b->signatures_);
    auto approve_signatures = SessionBlockCandidateSignatureVector::move_to_persistent(desc, b->approve_signatures_);
    auto block = SentBlock::move_to_persistent(desc, b->block_);
    auto r = lookup(desc, b->seqno_, block, signatures, approve_signatures, b->hash_, false);
    if (r) {
      return r;
    }

    return new (desc, false)
        ValidatorSessionOldRoundState{desc, b->seqno_, block, signatures, approve_signatures, b->hash_};
  }
  static const ValidatorSessionOldRoundState* create(ValidatorSessionDescription& desc, td::uint32 seqno,
                                                     const SentBlock* block,
                                                     const SessionBlockCandidateSignatureVector* signatures,
                                                     const SessionBlockCandidateSignatureVector* approve_signatures) {
    auto hash = create_hash(desc, seqno, get_vs_hash(desc, block), get_vs_hash(desc, signatures),
                            get_vs_hash(desc, approve_signatures));

    auto r = lookup(desc, seqno, block, signatures, approve_signatures, hash, true);
    if (r) {
      return r;
    }

    return new (desc, true) ValidatorSessionOldRoundState(desc, seqno, block, signatures, approve_signatures, hash);
  }
  static const ValidatorSessionOldRoundState* create(ValidatorSessionDescription& desc,
                                                     const ValidatorSessionRoundState* round);
  ValidatorSessionOldRoundState(ValidatorSessionDescription& desc, td::uint32 seqno, const SentBlock* block,
                                const SessionBlockCandidateSignatureVector* signatures,
                                const SessionBlockCandidateSignatureVector* approve_signatures, HashType hash)
      : RootObject(sizeof(ValidatorSessionOldRoundState))
      , seqno_(seqno)
      , block_(std::move(block))
      , signatures_(std::move(signatures))
      , approve_signatures_(std::move(approve_signatures))
      , hash_(std::move(hash)) {
    desc.update_hash(this, hash_);
  }
  static const ValidatorSessionOldRoundState* merge(ValidatorSessionDescription& desc,
                                                    const ValidatorSessionOldRoundState* left,
                                                    const ValidatorSessionOldRoundState* right);
  static const ValidatorSessionOldRoundState* merge(ValidatorSessionDescription& desc,
                                                    const ValidatorSessionOldRoundState* left,
                                                    const ValidatorSessionRoundState* right);
  static const ValidatorSessionOldRoundState* action(ValidatorSessionDescription& desc,
                                                     const ValidatorSessionOldRoundState* state, td::uint32 src_idx,
                                                     td::uint32 att,
                                                     const ton_api::validatorSession_message_approvedBlock& action);
  static const ValidatorSessionOldRoundState* action(ValidatorSessionDescription& desc,
                                                     const ValidatorSessionOldRoundState* state, td::uint32 src_idx,
                                                     td::uint32 att,
                                                     const ton_api::validatorSession_message_commit& action);
  template <class T>
  static const ValidatorSessionOldRoundState* action(ValidatorSessionDescription& desc,
                                                     const ValidatorSessionOldRoundState* state, td::uint32 src_idx,
                                                     td::uint32 att, const T& action) {
    VLOG(VALIDATOR_SESSION_WARNING) << "[validator session][node " << desc.get_source_id(src_idx) << "][" << action
                                    << "]: invalid message in old round: expected APPROVE/COMMIT";
    return state;
  }
  static const ValidatorSessionOldRoundState* action(ValidatorSessionDescription& desc,
                                                     const ValidatorSessionOldRoundState* state, td::uint32 src_idx,
                                                     td::uint32 att,
                                                     const ton_api::validatorSession_round_Message* action);

  auto get_hash(ValidatorSessionDescription& desc) const {
    return hash_;
  }

  auto get_block() const {
    return block_;
  }

  auto get_signatures() const {
    return signatures_;
  }

  auto get_approve_signatures() const {
    return approve_signatures_;
  }

  auto get_seqno() const {
    return seqno_;
  }

  bool check_block_is_signed_by(td::uint32 src_idx) const {
    return signatures_->at(src_idx) != nullptr;
  }
  bool check_block_is_approved_by(td::uint32 src_idx) const {
    return approve_signatures_->at(src_idx) != nullptr;
  }

  ValidatorSessionCandidateId get_block_id() const {
    return SentBlock::get_block_id(block_);
  }

 private:
  const td::uint32 seqno_;
  const SentBlock* block_;
  const SessionBlockCandidateSignatureVector* signatures_;
  const SessionBlockCandidateSignatureVector* approve_signatures_;
  const HashType hash_;
};


class ValidatorSessionRoundState : public ValidatorSessionDescription::RootObject {
 public:
  static HashType create_hash(ValidatorSessionDescription& desc, const SentBlock* precommitted_block, td::uint32 seqno,
                              bool precommitted, const CntVector<td::uint32>* first_attempt,
                              const CntVector<td::uint32>* last_precommit, const ApproveVector* sent,
                              const CntVector<const SessionBlockCandidateSignature*>* signatures,
                              const AttemptVector* attempts) {
    auto obj = create_tl_object<ton_api::hashable_validatorSessionRound>(
        get_vs_hash(desc, precommitted_block), seqno, precommitted, get_vs_hash(desc, first_attempt),
        get_vs_hash(desc, last_precommit), get_vs_hash(desc, sent), get_vs_hash(desc, signatures),
        get_vs_hash(desc, attempts));
    return desc.compute_hash(serialize_tl_object(obj, true).as_slice());
  }
  static bool compare(const RootObject* root_object, const SentBlock* precommitted_block, td::uint32 seqno,
                      bool precommitted, const CntVector<td::uint32>* first_attempt,
                      const CntVector<td::uint32>* last_precommit, const ApproveVector* sent,
                      const CntVector<const SessionBlockCandidateSignature*>* signatures, const AttemptVector* attempts,
                      HashType hash) {
    if (!root_object || root_object->get_size() < sizeof(ValidatorSessionRoundState)) {
      return false;
    }
    auto obj = static_cast<const ValidatorSessionRoundState*>(root_object);
    return obj->precommitted_block_ == precommitted_block && obj->seqno_ == seqno &&
           obj->precommitted_ == precommitted && obj->first_attempt_ == first_attempt &&
           obj->last_precommit_ == last_precommit && obj->sent_blocks_ == sent && obj->signatures_ == signatures &&
           obj->attempts_ == attempts && obj->hash_ == hash;
  }
  static const ValidatorSessionRoundState* lookup(ValidatorSessionDescription& desc,
                                                  const SentBlock* precommitted_block, td::uint32 seqno,
                                                  bool precommitted, const CntVector<td::uint32>* first_attempt,
                                                  const CntVector<td::uint32>* last_precommit,
                                                  const ApproveVector* sent,
                                                  const CntVector<const SessionBlockCandidateSignature*>* signatures,
                                                  const AttemptVector* attempts, HashType hash, bool temp) {
    auto r = desc.get_by_hash(hash, temp);
    if (compare(r, precommitted_block, seqno, precommitted, first_attempt, last_precommit, sent, signatures, attempts,
                hash)) {
      desc.on_reuse();
      return static_cast<const ValidatorSessionRoundState*>(r);
    }

    return nullptr;
  }
  static const ValidatorSessionRoundState* create(ValidatorSessionDescription& desc,
                                                  const SentBlock* precommitted_block, td::uint32 seqno,
                                                  bool precommitted, const CntVector<td::uint32>* first_attempt,
                                                  const CntVector<td::uint32>* last_precommit,
                                                  const ApproveVector* sent,
                                                  const CntVector<const SessionBlockCandidateSignature*>* signatures,
                                                  const AttemptVector* attempts) {
    auto hash = create_hash(desc, precommitted_block, seqno, precommitted, first_attempt, last_precommit, sent,
                            signatures, attempts);

    auto r = lookup(desc, precommitted_block, seqno, precommitted, first_attempt, last_precommit, sent, signatures,
                    attempts, hash, true);
    if (r) {
      return r;
    }

    return new (desc, true) ValidatorSessionRoundState(desc, precommitted_block, seqno, precommitted, first_attempt,
                                                       last_precommit, sent, signatures, attempts, hash);
  }
  static const ValidatorSessionRoundState* create(ValidatorSessionDescription& desc, td::uint32 seqno) {
    std::vector<const SessionBlockCandidateSignature*> signs;
    signs.resize(desc.get_total_nodes(), nullptr);
    std::vector<td::uint32> v;
    v.resize(desc.get_total_nodes(), 0);
    auto first_attempt = CntVector<td::uint32>::create(desc, v);
    auto last_precommit = CntVector<td::uint32>::create(desc, v);
    auto signatures = CntVector<const SessionBlockCandidateSignature*>::create(desc, std::move(signs));
    return create(desc, nullptr, seqno, false, first_attempt, last_precommit, nullptr, signatures, nullptr);
  }
  static const ValidatorSessionRoundState* move_to_persistent(ValidatorSessionDescription& desc,
                                                              const ValidatorSessionRoundState* b) {
    if (desc.is_persistent(b)) {
      return b;
    }
    auto precommitted_block = SentBlock::move_to_persistent(desc, b->precommitted_block_);
    auto first_attempt = CntVector<td::uint32>::move_to_persistent(desc, b->first_attempt_);
    auto last_precommit = CntVector<td::uint32>::move_to_persistent(desc, b->last_precommit_);
    auto sent = ApproveVector::move_to_persistent(desc, b->sent_blocks_);
    auto signatures = CntVector<const SessionBlockCandidateSignature*>::move_to_persistent(desc, b->signatures_);
    auto attempts = AttemptVector::move_to_persistent(desc, b->attempts_);
    auto r = lookup(desc, precommitted_block, b->seqno_, b->precommitted_, first_attempt, last_precommit, sent,
                    signatures, attempts, b->hash_, false);
    if (r) {
      return r;
    }
    return new (desc, false)
        ValidatorSessionRoundState{desc, precommitted_block, b->seqno_, b->precommitted_, first_attempt, last_precommit,
                                   sent, signatures,         attempts,  b->hash_};
  }
  ValidatorSessionRoundState(ValidatorSessionDescription& desc, const SentBlock* precommitted_block, td::uint32 seqno,
                             bool precommitted, const CntVector<td::uint32>* first_attempt,
                             const CntVector<td::uint32>* last_precommit, const ApproveVector* sent_blocks,
                             const CntVector<const SessionBlockCandidateSignature*>* signatures,
                             const AttemptVector* attempts, HashType hash)
      : RootObject{sizeof(ValidatorSessionRoundState)}
      , precommitted_block_(precommitted_block)
      , seqno_(seqno)
      , precommitted_(precommitted)
      , first_attempt_(first_attempt)
      , last_precommit_(last_precommit)
      , sent_blocks_(std::move(sent_blocks))
      , signatures_(std::move(signatures))
      , attempts_(std::move(attempts))
      , hash_(std::move(hash)) {
    desc.update_hash(this, hash_);
  }
  auto get_hash(ValidatorSessionDescription& desc) const {
    return hash_;
  }

  static const ValidatorSessionRoundState* merge(ValidatorSessionDescription& desc,
                                                 const ValidatorSessionRoundState* left,
                                                 const ValidatorSessionRoundState* right);
  static const ValidatorSessionRoundState* action(ValidatorSessionDescription& desc,
                                                  const ValidatorSessionRoundState* state, td::uint32 src_idx,
                                                  td::uint32 att,
                                                  const ton_api::validatorSession_message_submittedBlock& action);
  static const ValidatorSessionRoundState* action(ValidatorSessionDescription& desc,
                                                  const ValidatorSessionRoundState* state, td::uint32 src_idx,
                                                  td::uint32 att,
                                                  const ton_api::validatorSession_message_approvedBlock& action);
  static const ValidatorSessionRoundState* action(ValidatorSessionDescription& desc,
                                                  const ValidatorSessionRoundState* state, td::uint32 src_idx,
                                                  td::uint32 att,
                                                  const ton_api::validatorSession_message_rejectedBlock& action);
  static const ValidatorSessionRoundState* action(ValidatorSessionDescription& desc,
                                                  const ValidatorSessionRoundState* state, td::uint32 src_idx,
                                                  td::uint32 att,
                                                  const ton_api::validatorSession_message_commit& action);
  static const ValidatorSessionRoundState* forward_action_to_attempt(
      ValidatorSessionDescription& desc, const ValidatorSessionRoundState* state, td::uint32 src_idx, td::uint32 att,
      const ton_api::validatorSession_round_Message* act);
  template <class T>
  static const ValidatorSessionRoundState* action(ValidatorSessionDescription& desc,
                                                  const ValidatorSessionRoundState* state, td::uint32 src_idx,
                                                  td::uint32 att, const T& act) {
    return forward_action_to_attempt(desc, state, src_idx, att, &act);
  }
  static const ValidatorSessionRoundState* action(ValidatorSessionDescription& desc,
                                                  const ValidatorSessionRoundState* state, td::uint32 src_idx,
                                                  td::uint32 att,
                                                  const ton_api::validatorSession_round_Message* action);
  static const ValidatorSessionRoundState* make_one(ValidatorSessionDescription& desc,
                                                    const ValidatorSessionRoundState* state, td::uint32 src_idx,
                                                    td::uint32 att, bool& made);

  auto get_precommitted_block(bool& f) const {
    f = precommitted_;
    return precommitted_block_;
  }
  auto get_signatures() const {
    return signatures_;
  }
  td::uint32 get_seqno() const {
    return seqno_;
  }
  auto get_first_attempt(td::uint32 src_idx) const {
    return first_attempt_->at(src_idx);
  }
  auto get_last_precommit(td::uint32 src_idx) const {
    return last_precommit_->at(src_idx);
  }
  auto get_sent_blocks() const {
    return sent_blocks_;
  }

  const SessionBlockCandidate* get_block(ValidatorSessionCandidateId block_hash) const;
  std::vector<td::uint32> get_block_approvers(ValidatorSessionDescription& desc,
                                              ValidatorSessionCandidateId block) const;
  std::vector<const SentBlock*> get_blocks_approved_by(ValidatorSessionDescription& desc, td::uint32 src_idx) const;

  bool check_block_is_signed(ValidatorSessionDescription& desc) const;
  bool check_block_is_signed_by(td::uint32 src_idx) const {
    return signatures_->at(src_idx) != nullptr;
  }
  bool check_block_is_approved(ValidatorSessionCandidateId block_id) const;
  bool check_block_is_approved_by(td::uint32 src_idx, ValidatorSessionCandidateId block_id) const;
  const SentBlock* check_block_is_sent(ValidatorSessionCandidateId block_id) const;
  bool check_block_is_sent_by(td::uint32 src_idx) const;

  bool check_need_generate_vote_for(ValidatorSessionDescription& desc, td::uint32 src_idx, td::uint32 att) const;
  tl_object_ptr<ton_api::validatorSession_message_voteFor> generate_vote_for(ValidatorSessionDescription& desc,
                                                                             td::uint32 src_idx, td::uint32 att) const;

  const SentBlock* choose_block_to_sign(ValidatorSessionDescription& desc, td::uint32 src_idx, bool& found) const;
  std::vector<const SentBlock*> choose_blocks_to_approve(ValidatorSessionDescription& desc, td::uint32 src_idx) const;
  const SentBlock* choose_block_to_vote(ValidatorSessionDescription& desc, td::uint32 src_idx, td::uint32 att,
                                        const SentBlock* vote_for, bool vote_for_inited, bool& found) const;

  tl_object_ptr<ton_api::validatorSession_round_Message> create_action(ValidatorSessionDescription& desc,
                                                                       td::uint32 src_idx, td::uint32 att) const;
  void dump(ValidatorSessionDescription& desc, td::StringBuilder& sb, td::uint32 att) const;
  void dump_cur_attempt(ValidatorSessionDescription& desc, td::StringBuilder& sb) const;

  void for_each_sent_block(std::function<void(const SessionBlockCandidate*)> foo) const {
    if (!sent_blocks_) {
      return;
    }
    for (td::uint32 i = 0; i < sent_blocks_->size(); ++i) {
      foo(sent_blocks_->at(i));
    }
  }

 private:
  const SentBlock* precommitted_block_;
  const td::uint32 seqno_;
  const bool precommitted_;
  const CntVector<td::uint32>* first_attempt_;
  const CntVector<td::uint32>* last_precommit_;
  const ApproveVector* sent_blocks_;
  const CntVector<const SessionBlockCandidateSignature*>* signatures_;
  const AttemptVector* attempts_;
  const HashType hash_;
};

class ValidatorSessionState : public ValidatorSessionDescription::RootObject {
 public:
  static HashType create_hash(ValidatorSessionDescription& desc, const CntVector<td::uint32>* att,
                              const CntVector<const ValidatorSessionOldRoundState*>* old_rounds,
                              const ValidatorSessionRoundState* cur_round) {
    auto obj = create_tl_object<ton_api::hashable_validatorSession>(
        get_vs_hash(desc, att), get_vs_hash(desc, old_rounds), get_vs_hash(desc, cur_round));
    return desc.compute_hash(serialize_tl_object(obj, true).as_slice());
  }
  static bool compare(const RootObject* r, const CntVector<td::uint32>* att,
                      const CntVector<const ValidatorSessionOldRoundState*>* old_rounds,
                      const ValidatorSessionRoundState* cur_round, HashType hash) {
    if (!r || r->get_size() < sizeof(ValidatorSessionState)) {
      return false;
    }
    auto R = static_cast<const ValidatorSessionState*>(r);
    return R->att_ == att && R->old_rounds_ == old_rounds && R->cur_round_ == cur_round && R->hash_ == hash;
  }
  static const ValidatorSessionState* lookup(ValidatorSessionDescription& desc, const CntVector<td::uint32>* att,
                                             const CntVector<const ValidatorSessionOldRoundState*>* old_rounds,
                                             const ValidatorSessionRoundState* cur_round, HashType hash, bool temp) {
    auto r = desc.get_by_hash(hash, temp);
    if (compare(r, att, old_rounds, cur_round, hash)) {
      desc.on_reuse();
      return static_cast<const ValidatorSessionState*>(r);
    }
    return nullptr;
  }
  static const ValidatorSessionState* create(ValidatorSessionDescription& desc, const CntVector<td::uint32>* att,
                                             const CntVector<const ValidatorSessionOldRoundState*>* old_rounds,
                                             const ValidatorSessionRoundState* cur_round) {
    auto hash = create_hash(desc, att, old_rounds, cur_round);
    auto r = lookup(desc, att, old_rounds, cur_round, hash, true);
    if (r) {
      return r;
    }

    return new (desc, true) ValidatorSessionState{desc, att, old_rounds, cur_round, hash};
  }
  static const ValidatorSessionState* create(ValidatorSessionDescription& desc) {
    std::vector<td::uint32> vec;
    vec.resize(desc.get_total_nodes(), 0);
    auto ts = CntVector<td::uint32>::create(desc, std::move(vec));
    auto cur_round = ValidatorSessionRoundState::create(desc, 0);
    return create(desc, ts, nullptr, std::move(cur_round));
  }
  static const ValidatorSessionState* move_to_persistent(ValidatorSessionDescription& desc,
                                                         const ValidatorSessionState* b) {
    if (desc.is_persistent(b)) {
      return b;
    }
    auto ts = CntVector<td::uint32>::move_to_persistent(desc, b->att_);
    auto old_rounds = CntVector<const ValidatorSessionOldRoundState*>::move_to_persistent(desc, b->old_rounds_);
    auto cur_round = ValidatorSessionRoundState::move_to_persistent(desc, b->cur_round_);
    auto r = lookup(desc, ts, old_rounds, cur_round, b->hash_, false);
    if (r) {
      return r;
    }
    return new (desc, false) ValidatorSessionState{desc, ts, old_rounds, cur_round, b->hash_};
  }
  ValidatorSessionState(ValidatorSessionDescription& desc, const CntVector<td::uint32>* att,
                        const CntVector<const ValidatorSessionOldRoundState*>* old_rounds,
                        const ValidatorSessionRoundState* cur_round, HashType hash)
      : RootObject{sizeof(ValidatorSessionState)}
      , att_(att)
      , old_rounds_(std::move(old_rounds))
      , cur_round_(std::move(cur_round))
      , hash_(std::move(hash)) {
    desc.update_hash(this, hash_);
  }

  auto get_hash(ValidatorSessionDescription& desc) const {
    return hash_;
  }

  auto cur_round_seqno() const {
    return cur_round_->get_seqno();
  }
  auto get_ts(td::uint32 src_idx) const {
    return att_->at(src_idx);
  }
  td::uint32 cur_attempt_in_round(const ValidatorSessionDescription& desc) const {
    td::uint32 first_attempt = cur_round_->get_first_attempt(desc.get_self_idx());
    td::uint32 cur_attempt = desc.get_attempt_seqno(desc.get_ts());
    if (cur_attempt < first_attempt || first_attempt == 0) {
      return 0;
    }
    return cur_attempt - first_attempt;
  }

  const SentBlock* choose_block_to_sign(ValidatorSessionDescription& desc, td::uint32 src_idx, bool& found) const;
  const SentBlock* get_committed_block(ValidatorSessionDescription& desc, td::uint32 seqno) const;
  const SentBlock* get_block(ValidatorSessionDescription& desc, ValidatorSessionCandidateId id, bool& found) const;
  std::vector<const SentBlock*> get_blocks_approved_by(ValidatorSessionDescription& desc, td::uint32 src_idx) const;
  const CntVector<const SessionBlockCandidateSignature*>* get_committed_block_signatures(
      ValidatorSessionDescription& desc, td::uint32 seqno) const;
  const CntVector<const SessionBlockCandidateSignature*>* get_committed_block_approve_signatures(
      ValidatorSessionDescription& desc, td::uint32 seqno) const;

  std::vector<const SentBlock*> choose_blocks_to_approve(ValidatorSessionDescription& desc, td::uint32 src_idx) const {
    return cur_round_->choose_blocks_to_approve(desc, src_idx);
  }
  bool check_block_is_sent_by(ValidatorSessionDescription& desc, td::uint32 src_idx) const {
    return cur_round_->check_block_is_sent_by(src_idx);
  }
  bool check_block_is_signed_by(ValidatorSessionDescription& desc, td::uint32 src_idx) const {
    return cur_round_->check_block_is_signed_by(src_idx);
  }
  bool check_block_is_approved_by(ValidatorSessionDescription& desc, td::uint32 src_idx,
                                  ValidatorSessionCandidateId id) const {
    auto B = cur_round_->get_block(id);
    if (!B) {
      return false;
    }
    return B->check_block_is_approved_by(src_idx);
  }
  bool check_need_generate_vote_for(ValidatorSessionDescription& desc, td::uint32 src_idx, td::uint32 att) const {
    return cur_round_->check_need_generate_vote_for(desc, src_idx, att);
  }
  tl_object_ptr<ton_api::validatorSession_message_voteFor> generate_vote_for(ValidatorSessionDescription& desc,
                                                                             td::uint32 src_idx, td::uint32 att) const {
    return cur_round_->generate_vote_for(desc, src_idx, att);
  }
  std::vector<td::uint32> get_block_approvers(ValidatorSessionDescription& desc,
                                              ValidatorSessionCandidateId block) const {
    return cur_round_->get_block_approvers(desc, block);
  }

  tl_object_ptr<ton_api::validatorSession_round_Message> create_action(ValidatorSessionDescription& desc,
                                                                       td::uint32 src_idx, td::uint32 att) const;

  void dump(ValidatorSessionDescription& desc, td::StringBuilder& sb, td::uint32 att) const;
  void dump_cur_attempt(ValidatorSessionDescription& desc, td::StringBuilder& sb) const {
    cur_round_->dump_cur_attempt(desc, sb);
  }

  void for_each_cur_round_sent_block(std::function<void(const SessionBlockCandidate*)> foo) const {
    cur_round_->for_each_sent_block(std::move(foo));
  }

  const SentBlock* get_cur_round_precommitted_block() const {
    bool found;
    return cur_round_->get_precommitted_block(found);
  }

  const CntVector<const SessionBlockCandidateSignature*>* get_cur_round_signatures() const {
    return cur_round_->get_signatures();
  }

  static const ValidatorSessionState* make_one(ValidatorSessionDescription& desc, const ValidatorSessionState* state,
                                               td::uint32 src_idx, td::uint32 att, bool& made);
  static const ValidatorSessionState* make_all(ValidatorSessionDescription& desc, const ValidatorSessionState* state,
                                               td::uint32 src_idx, td::uint32 att) {
    bool made;
    do {
      made = false;
      state = make_one(desc, state, src_idx, att, made);
    } while (made);
    return state;
  }

  static const ValidatorSessionState* merge(ValidatorSessionDescription& desc, const ValidatorSessionState* left,
                                            const ValidatorSessionState* right);
  static const ValidatorSessionState* action(ValidatorSessionDescription& desc, const ValidatorSessionState* state,
                                             td::uint32 src_idx, td::uint32 att,
                                             const ton_api::validatorSession_round_Message* action);

 private:
  const CntVector<td::uint32>* att_;
  const CntVector<const ValidatorSessionOldRoundState*>* old_rounds_;
  const ValidatorSessionRoundState* cur_round_;
  const HashType hash_;
};

}  // namespace validatorsession

}  // namespace ton
