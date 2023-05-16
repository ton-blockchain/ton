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
*/

#pragma once

#include "td/utils/int_types.h"
#include "td/utils/buffer.h"
#include "adnl/utils.hpp"
#include "common/io.hpp"

#include "persistent-vector.h"

#include "validator-session-description.h"

#include "validator-session-types.h"

#include <limits>


namespace ton {

namespace validatorsession {

using HashType = ValidatorSessionDescription::HashType;

struct SessionBlockCandidateSignature : public ValidatorSessionDescription::RootObject {
 public:
  static auto create_hash(ValidatorSessionDescription& desc, td::Slice data) {
    auto obj = create_tl_object<ton_api::hashable_blockSignature>(desc.compute_hash(data));
    return desc.compute_hash(serialize_tl_object(obj, true).as_slice());
  }

  static bool compare(const RootObject* r, td::Slice data, HashType hash) {
    if (!r || r->get_size() < sizeof(SessionBlockCandidateSignature)) {
      return false;
    }
    auto R = static_cast<const SessionBlockCandidateSignature*>(r);
    return R->hash_ == hash && R->data_.ubegin() == data.ubegin() && R->data_.size() == data.size();
  }

  static auto lookup(ValidatorSessionDescription& desc, td::Slice data, HashType hash, bool temp) {
    auto r = desc.get_by_hash(hash, temp);
    if (compare(r, data, hash)) {
      desc.on_reuse();
      return static_cast<const SessionBlockCandidateSignature*>(r);
    }
    return static_cast<const SessionBlockCandidateSignature*>(nullptr);
  }
  static SessionBlockCandidateSignature* create(ValidatorSessionDescription& desc, td::BufferSlice value) {
    auto hash = create_hash(desc, value.as_slice());
    auto d = static_cast<td::uint8*>(desc.alloc(value.size(), 8, false));
    td::MutableSlice s{d, value.size()};
    s.copy_from(value.as_slice());
    return new (desc, true) SessionBlockCandidateSignature{desc, s, hash};
  }
  static const SessionBlockCandidateSignature* move_to_persistent(ValidatorSessionDescription& desc,
                                                                  const SessionBlockCandidateSignature* b) {
    if (desc.is_persistent(b)) {
      return b;
    }
    CHECK(desc.is_persistent(b->data_.ubegin()));
    auto r = lookup(desc, b->data_, b->hash_, false);
    if (r) {
      return r;
    }
    return new (desc, false) SessionBlockCandidateSignature{desc, b->data_, b->hash_};
  }
  static const SessionBlockCandidateSignature* merge(ValidatorSessionDescription& desc,
                                                     const SessionBlockCandidateSignature* l,
                                                     const SessionBlockCandidateSignature* r);
  SessionBlockCandidateSignature(ValidatorSessionDescription& desc, td::Slice data, HashType hash)
      : RootObject(sizeof(SessionBlockCandidateSignature)), data_{data}, hash_(std::move(hash)) {
    desc.update_hash(this, hash_);
  }
  td::BufferSlice value() const {
    return td::BufferSlice{data_};
  }
  td::Slice as_slice() const {
    return data_;
  }
  auto get_hash(ValidatorSessionDescription& desc) const {
    return hash_;
  }

 private:
  const td::Slice data_;
  const HashType hash_;
};

using SessionBlockCandidateSignatureVector = CntVector<const SessionBlockCandidateSignature*>;

class SentBlock : public ValidatorSessionDescription::RootObject {
 public:
  static HashType create_hash(ValidatorSessionDescription& desc, td::uint32 src_idx, ValidatorSessionRootHash root_hash,
                              ValidatorSessionFileHash file_hash,
                              ValidatorSessionCollatedDataFileHash collated_data_file_hash) {
    auto obj = create_tl_object<ton_api::hashable_sentBlock>(src_idx, get_vs_hash(desc, root_hash),
                                                             get_vs_hash(desc, file_hash),
                                                             get_vs_hash(desc, collated_data_file_hash));
    return desc.compute_hash(serialize_tl_object(obj, true).as_slice());
  }
  static bool compare(const RootObject* root_object, td::uint32 src_idx, const ValidatorSessionRootHash& root_hash,
                      const ValidatorSessionFileHash& file_hash,
                      const ValidatorSessionCollatedDataFileHash& collated_data_file_hash, HashType hash) {
    if (!root_object || root_object->get_size() < sizeof(SentBlock)) {
      return false;
    }
    auto obj = static_cast<const SentBlock*>(root_object);
    return obj->src_idx_ == src_idx && obj->root_hash_ == root_hash && obj->file_hash_ == file_hash &&
           obj->collated_data_file_hash_ == collated_data_file_hash && obj->hash_ == hash;
  }
  static auto lookup(ValidatorSessionDescription& desc, td::uint32 src_idx, const ValidatorSessionRootHash& root_hash,
                     const ValidatorSessionFileHash& file_hash,
                     const ValidatorSessionCollatedDataFileHash& collated_data_file_hash, HashType hash, bool temp) {
    auto r = desc.get_by_hash(hash, temp);
    if (compare(r, src_idx, root_hash, file_hash, collated_data_file_hash, hash)) {
      desc.on_reuse();
      return static_cast<const SentBlock*>(r);
    }
    return static_cast<const SentBlock*>(nullptr);
  }
  static const SentBlock* create(ValidatorSessionDescription& desc, td::uint32 src_idx,
                                 const ValidatorSessionRootHash& root_hash, const ValidatorSessionFileHash& file_hash,
                                 const ValidatorSessionCollatedDataFileHash& collated_data_file_hash) {
    auto hash = create_hash(desc, src_idx, root_hash, file_hash, collated_data_file_hash);
    auto r = lookup(desc, src_idx, root_hash, file_hash, collated_data_file_hash, hash, true);
    if (r) {
      return r;
    }

    auto candidate_id = desc.candidate_id(src_idx, root_hash, file_hash, collated_data_file_hash);

    return new (desc, true) SentBlock{desc, src_idx, root_hash, file_hash, collated_data_file_hash, candidate_id, hash};
  }
  static const SentBlock* create(ValidatorSessionDescription& desc, const ValidatorSessionCandidateId& zero) {
    CHECK(zero.is_zero());
    auto hash = create_hash(desc, 0, ValidatorSessionRootHash::zero(), ValidatorSessionFileHash::zero(),
                            ValidatorSessionCollatedDataFileHash::zero());

    return new (desc, true) SentBlock{desc,
                                      0,
                                      ValidatorSessionRootHash::zero(),
                                      ValidatorSessionFileHash::zero(),
                                      ValidatorSessionCollatedDataFileHash::zero(),
                                      zero,
                                      hash};
  }
  static const SentBlock* move_to_persistent(ValidatorSessionDescription& desc, const SentBlock* b) {
    if (desc.is_persistent(b)) {
      return b;
    }
    auto r = lookup(desc, b->src_idx_, b->root_hash_, b->file_hash_, b->collated_data_file_hash_, b->hash_, false);
    if (r) {
      return r;
    }

    return new (desc, false) SentBlock{
        desc, b->src_idx_, b->root_hash_, b->file_hash_, b->collated_data_file_hash_, b->candidate_id_, b->hash_};
  }
  SentBlock(ValidatorSessionDescription& desc, td::uint32 src_idx, ValidatorSessionRootHash root_hash,
            ValidatorSessionFileHash file_hash, ValidatorSessionCollatedDataFileHash collated_data_file_hash,
            ValidatorSessionCandidateId candidate_id, HashType hash)
      : RootObject(sizeof(SentBlock))
      , src_idx_(src_idx)
      , root_hash_(std::move(root_hash))
      , file_hash_(std::move(file_hash))
      , collated_data_file_hash_(std::move(collated_data_file_hash))
      , candidate_id_(candidate_id)
      , hash_(std::move(hash)) {
    desc.update_hash(this, hash_);
  }
  auto get_src_idx() const {
    return src_idx_;
  }
  auto get_root_hash() const {
    return root_hash_;
  }
  auto get_file_hash() const {
    return file_hash_;
  }
  auto get_collated_data_file_hash() const {
    return collated_data_file_hash_;
  }
  static ValidatorSessionCandidateId get_block_id(const SentBlock* block) {
    return block ? block->candidate_id_ : skip_round_candidate_id();
  }
  HashType get_hash(ValidatorSessionDescription& desc) const {
    return hash_;
  }
  bool operator<(const SentBlock& block) const {
    if (src_idx_ < block.src_idx_) {
      return true;
    }
    if (src_idx_ > block.src_idx_) {
      return false;
    }
    if (candidate_id_ < block.candidate_id_) {
      return true;
    }
    return false;
  }
  struct Compare {
    bool operator()(const SentBlock* a, const SentBlock* b) const {
      return *a < *b;
    }
  };

 private:
  const td::uint32 src_idx_;
  const ValidatorSessionRootHash root_hash_;
  const ValidatorSessionFileHash file_hash_;
  const ValidatorSessionCollatedDataFileHash collated_data_file_hash_;
  const ValidatorSessionCandidateId candidate_id_;
  const HashType hash_;
};

class SessionBlockCandidate : public ValidatorSessionDescription::RootObject {
 public:
  static HashType create_hash(ValidatorSessionDescription& desc, HashType block, HashType approved) {
    auto obj = create_tl_object<ton_api::hashable_blockCandidate>(block, approved);
    return desc.compute_hash(serialize_tl_object(obj, true).as_slice());
  }
  static bool compare(const RootObject* r, const SentBlock* block, const SessionBlockCandidateSignatureVector* approved,
                      HashType hash) {
    if (!r || r->get_size() < sizeof(SessionBlockCandidate)) {
      return false;
    }
    auto R = static_cast<const SessionBlockCandidate*>(r);
    return R->block_ == block && R->approved_by_ == approved && R->hash_ == hash;
  }
  static auto lookup(ValidatorSessionDescription& desc, const SentBlock* block,
                     const SessionBlockCandidateSignatureVector* approved, HashType hash, bool temp) {
    auto r = desc.get_by_hash(hash, temp);
    if (compare(r, block, approved, hash)) {
      desc.on_reuse();
      return static_cast<const SessionBlockCandidate*>(r);
    }
    return static_cast<const SessionBlockCandidate*>(nullptr);
  }
  static const SessionBlockCandidate* create(ValidatorSessionDescription& desc, const SentBlock* block,
                                             const SessionBlockCandidateSignatureVector* approved) {
    auto hash = create_hash(desc, get_vs_hash(desc, block), get_vs_hash(desc, approved));

    auto r = lookup(desc, block, approved, hash, true);
    if (r) {
      return r;
    }

    return new (desc, true) SessionBlockCandidate(desc, block, approved, hash);
  }
  static const SessionBlockCandidate* create(ValidatorSessionDescription& desc, const SentBlock* block) {
    std::vector<const SessionBlockCandidateSignature*> v;
    v.resize(desc.get_total_nodes(), nullptr);
    auto vec = SessionBlockCandidateSignatureVector::create(desc, std::move(v));
    return create(desc, block, vec);
  }
  static const SessionBlockCandidate* move_to_persistent(ValidatorSessionDescription& desc,
                                                         const SessionBlockCandidate* b) {
    if (desc.is_persistent(b)) {
      return b;
    }
    auto block = SentBlock::move_to_persistent(desc, b->block_);
    auto approved = SessionBlockCandidateSignatureVector::move_to_persistent(desc, b->approved_by_);
    auto r = lookup(desc, block, approved, b->hash_, false);
    if (r) {
      return r;
    }

    return new (desc, false) SessionBlockCandidate{desc, block, approved, b->hash_};
  }
  SessionBlockCandidate(ValidatorSessionDescription& desc, const SentBlock* block,
                        const SessionBlockCandidateSignatureVector* approved, HashType hash)
      : RootObject{sizeof(SessionBlockCandidate)}, block_(block), approved_by_(approved), hash_(hash) {
    desc.update_hash(this, hash_);
  }
  static const SessionBlockCandidate* merge(ValidatorSessionDescription& desc, const SessionBlockCandidate* l,
                                            const SessionBlockCandidate* r);
  auto get_block() const {
    return block_;
  }
  auto get_id() const {
    return SentBlock::get_block_id(block_);
  }
  auto get_src_idx() const {
    return block_ ? block_->get_src_idx() : std::numeric_limits<td::uint32>::max();
  }
  bool check_block_is_approved_by(td::uint32 src_idx) const {
    return approved_by_->at(src_idx);
  }
  bool check_block_is_approved(ValidatorSessionDescription& desc) const;
  auto get_hash(ValidatorSessionDescription& desc) const {
    return hash_;
  }
  auto get_approvers_list() const {
    return approved_by_;
  }
  static const SessionBlockCandidate* push(ValidatorSessionDescription& desc, const SessionBlockCandidate* state,
                                           td::uint32 src_idx, const SessionBlockCandidateSignature* sig) {
    CHECK(state);
    if (state->approved_by_->at(src_idx)) {
      return state;
    }
    return create(desc, state->block_,
                  SessionBlockCandidateSignatureVector::change(desc, state->approved_by_, src_idx, sig));
  }
  class Compare {
   public:
    bool operator()(const SessionBlockCandidate* l, const SessionBlockCandidate* r) {
      return l->get_id() < r->get_id();
    }
  };

 private:
  const SentBlock* block_;
  const SessionBlockCandidateSignatureVector* approved_by_;
  const HashType hash_;
};

class SessionVoteCandidate : public ValidatorSessionDescription::RootObject {
 public:
  static HashType create_hash(ValidatorSessionDescription& desc, HashType block, HashType voted) {
    auto obj = create_tl_object<ton_api::hashable_blockVoteCandidate>(block, voted);
    return desc.compute_hash(serialize_tl_object(obj, true).as_slice());
  }
  static bool compare(const RootObject* r, const SentBlock* block, const CntVector<bool>* voted, HashType hash) {
    if (!r || r->get_size() < sizeof(SessionVoteCandidate)) {
      return false;
    }
    auto R = static_cast<const SessionVoteCandidate*>(r);
    return R->block_ == block && R->voted_by_ == voted && R->hash_ == hash;
  }
  static auto lookup(ValidatorSessionDescription& desc, const SentBlock* block, const CntVector<bool>* voted,
                     HashType hash, bool temp) {
    auto r = desc.get_by_hash(hash, temp);
    if (compare(r, block, voted, hash)) {
      desc.on_reuse();
      return static_cast<const SessionVoteCandidate*>(r);
    }
    return static_cast<const SessionVoteCandidate*>(nullptr);
  }
  static const SessionVoteCandidate* create(ValidatorSessionDescription& desc, const SentBlock* block,
                                            const CntVector<bool>* voted) {
    auto hash = create_hash(desc, get_vs_hash(desc, block), get_vs_hash(desc, voted));

    auto r = lookup(desc, block, voted, hash, true);
    if (r) {
      return r;
    }

    return new (desc, true) SessionVoteCandidate(desc, block, voted, hash);
  }
  static const SessionVoteCandidate* create(ValidatorSessionDescription& desc, const SentBlock* block) {
    std::vector<bool> v;
    v.resize(desc.get_total_nodes(), false);
    auto vec = CntVector<bool>::create(desc, std::move(v));
    return create(desc, block, vec);
  }
  static const SessionVoteCandidate* move_to_persistent(ValidatorSessionDescription& desc,
                                                        const SessionVoteCandidate* b) {
    if (desc.is_persistent(b)) {
      return b;
    }
    auto block = SentBlock::move_to_persistent(desc, b->block_);
    auto voted = CntVector<bool>::move_to_persistent(desc, b->voted_by_);
    auto r = lookup(desc, block, voted, b->hash_, false);
    if (r) {
      return r;
    }

    return new (desc, false) SessionVoteCandidate{desc, block, voted, b->hash_};
  }
  SessionVoteCandidate(ValidatorSessionDescription& desc, const SentBlock* block, const CntVector<bool>* voted,
                       HashType hash)
      : RootObject{sizeof(SessionVoteCandidate)}, block_(block), voted_by_(voted), hash_(hash) {
    desc.update_hash(this, hash_);
  }
  static const SessionVoteCandidate* merge(ValidatorSessionDescription& desc, const SessionVoteCandidate* l,
                                           const SessionVoteCandidate* r);
  auto get_block() const {
    return block_;
  }
  auto get_id() const {
    return SentBlock::get_block_id(block_);
  }
  auto get_src_idx() const {
    return block_ ? block_->get_src_idx() : std::numeric_limits<td::uint32>::max();
  }
  bool check_block_is_voted_by(td::uint32 src_idx) const {
    return voted_by_->at(src_idx);
  }
  bool check_block_is_voted(ValidatorSessionDescription& desc) const;
  auto get_hash(ValidatorSessionDescription& desc) const {
    return hash_;
  }
  auto get_voters_list() const {
    return voted_by_;
  }
  static const SessionVoteCandidate* push(ValidatorSessionDescription& desc, const SessionVoteCandidate* state,
                                          td::uint32 src_idx) {
    CHECK(state);
    if (state->voted_by_->at(src_idx)) {
      return state;
    }
    return create(desc, state->block_, CntVector<bool>::change(desc, state->voted_by_, src_idx, true));
  }
  class Compare {
   public:
    bool operator()(const SessionVoteCandidate* l, const SessionVoteCandidate* r) {
      return l->get_id() < r->get_id();
    }
  };

 private:
  const SentBlock* block_;
  const CntVector<bool>* voted_by_;
  const HashType hash_;
};

using VoteVector = CntSortedVector<const SessionVoteCandidate*, SessionVoteCandidate::Compare>;
using ApproveVector = CntSortedVector<const SessionBlockCandidate*, SessionBlockCandidate::Compare>;
class ValidatorSessionRoundState;

class ValidatorSessionRoundAttemptState : public ValidatorSessionDescription::RootObject {
 public:
  static HashType create_hash(ValidatorSessionDescription& desc, td::uint32 seqno, HashType votes,
                              HashType precommitted, bool vote_for_inited, HashType vote_for) {
    auto obj = create_tl_object<ton_api::hashable_validatorSessionRoundAttempt>(seqno, votes, precommitted,
                                                                                vote_for_inited, vote_for);
    return desc.compute_hash(serialize_tl_object(obj, true).as_slice());
  }
  static bool compare(const RootObject* r, td::uint32 seqno, const VoteVector* votes,
                      const CntVector<bool>* precommitted, const SentBlock* vote_for, bool vote_for_inited,
                      HashType hash) {
    if (!r || r->get_size() < sizeof(ValidatorSessionRoundAttemptState)) {
      return false;
    }
    auto R = static_cast<const ValidatorSessionRoundAttemptState*>(r);
    return R->seqno_ == seqno && R->votes_ == votes && R->precommitted_ == precommitted && R->vote_for_ == vote_for &&
           R->vote_for_inited_ == vote_for_inited && R->hash_ == hash;
  }
  static auto lookup(ValidatorSessionDescription& desc, td::uint32 seqno, const VoteVector* votes,
                     const CntVector<bool>* precommitted, const SentBlock* vote_for, bool vote_for_inited,
                     HashType hash, bool temp) {
    auto r = desc.get_by_hash(hash, temp);
    if (compare(r, seqno, votes, precommitted, vote_for, vote_for_inited, hash)) {
      desc.on_reuse();
      return static_cast<const ValidatorSessionRoundAttemptState*>(r);
    }
    return static_cast<const ValidatorSessionRoundAttemptState*>(nullptr);
  }
  static const ValidatorSessionRoundAttemptState* move_to_persistent(ValidatorSessionDescription& desc,
                                                                     const ValidatorSessionRoundAttemptState* b) {
    if (desc.is_persistent(b)) {
      return b;
    }
    auto votes = VoteVector::move_to_persistent(desc, b->votes_);
    auto precommitted = CntVector<bool>::move_to_persistent(desc, b->precommitted_);
    auto vote_for = SentBlock::move_to_persistent(desc, b->vote_for_);

    auto r = lookup(desc, b->seqno_, votes, precommitted, vote_for, b->vote_for_inited_, b->hash_, false);
    if (r) {
      return r;
    }

    return new (desc, false) ValidatorSessionRoundAttemptState{desc,     b->seqno_,           votes,   precommitted,
                                                               vote_for, b->vote_for_inited_, b->hash_};
  }

  static const ValidatorSessionRoundAttemptState* create(ValidatorSessionDescription& desc, td::uint32 seqno,
                                                         const VoteVector* votes, const CntVector<bool>* precommitted,
                                                         const SentBlock* vote_for, bool vote_for_inited) {
    auto hash = create_hash(desc, seqno, get_vs_hash(desc, votes), get_vs_hash(desc, precommitted),
                            get_vs_hash(desc, vote_for), vote_for_inited);

    auto r = lookup(desc, seqno, votes, precommitted, vote_for, vote_for_inited, hash, true);
    if (r) {
      return r;
    }

    return new (desc, true)
        ValidatorSessionRoundAttemptState(desc, seqno, votes, precommitted, vote_for, vote_for_inited, hash);
  }
  static const ValidatorSessionRoundAttemptState* create(ValidatorSessionDescription& desc, td::uint32 seqno) {
    std::vector<bool> x;
    x.resize(desc.get_total_nodes(), false);
    auto p = CntVector<bool>::create(desc, std::move(x));

    return create(desc, seqno, nullptr, p, nullptr, false);
  }

  ValidatorSessionRoundAttemptState(ValidatorSessionDescription& desc, td::uint32 seqno, const VoteVector* votes,
                                    const CntVector<bool>* precommitted, const SentBlock* vote_for,
                                    bool vote_for_inited, HashType hash)
      : RootObject{sizeof(ValidatorSessionRoundAttemptState)}
      , seqno_(seqno)
      , votes_(votes)
      , precommitted_(precommitted)
      , vote_for_(vote_for)
      , vote_for_inited_(vote_for_inited)
      , hash_(std::move(hash)) {
    desc.update_hash(this, hash_);
  }
  auto get_hash(ValidatorSessionDescription& desc) const {
    return hash_;
  }
  auto get_seqno() const {
    return seqno_;
  }
  auto get_votes() const {
    return votes_;
  }
  auto get_precommits() const {
    return precommitted_;
  }
  const SentBlock* get_voted_block(ValidatorSessionDescription& desc, bool& f) const;
  const SentBlock* get_vote_for_block(ValidatorSessionDescription& desc, bool& f) const {
    f = vote_for_inited_;
    return vote_for_;
  }
  bool check_attempt_is_precommitted(ValidatorSessionDescription& desc) const;
  bool check_vote_received_from(td::uint32 src_idx) const;
  bool check_precommit_received_from(td::uint32 src_idx) const;

  bool operator<(const ValidatorSessionRoundAttemptState& right) const {
    return seqno_ < right.seqno_;
  }
  struct Compare {
    bool operator()(const ValidatorSessionRoundAttemptState* a, const ValidatorSessionRoundAttemptState* b) const {
      return *a < *b;
    }
  };

  static const ValidatorSessionRoundAttemptState* merge(ValidatorSessionDescription& desc,
                                                        const ValidatorSessionRoundAttemptState* left,
                                                        const ValidatorSessionRoundAttemptState* right);
  static const ValidatorSessionRoundAttemptState* action(ValidatorSessionDescription& desc,
                                                         const ValidatorSessionRoundAttemptState* state,
                                                         td::uint32 src_idx, td::uint32 att,
                                                         const ton_api::validatorSession_message_voteFor& act,
                                                         const ValidatorSessionRoundState* round);
  static const ValidatorSessionRoundAttemptState* action(ValidatorSessionDescription& desc,
                                                         const ValidatorSessionRoundAttemptState* state,
                                                         td::uint32 src_idx, td::uint32 att,
                                                         const ton_api::validatorSession_message_vote& act,
                                                         const ValidatorSessionRoundState* round);
  static const ValidatorSessionRoundAttemptState* action(ValidatorSessionDescription& desc,
                                                         const ValidatorSessionRoundAttemptState* state,
                                                         td::uint32 src_idx, td::uint32 att,
                                                         const ton_api::validatorSession_message_precommit& act,
                                                         const ValidatorSessionRoundState* round);
  static const ValidatorSessionRoundAttemptState* action(ValidatorSessionDescription& desc,
                                                         const ValidatorSessionRoundAttemptState* state,
                                                         td::uint32 src_idx, td::uint32 att,
                                                         const ton_api::validatorSession_message_empty& act,
                                                         const ValidatorSessionRoundState* round);
  static const ValidatorSessionRoundAttemptState* action(ValidatorSessionDescription& desc,
                                                         const ValidatorSessionRoundAttemptState* state,
                                                         td::uint32 src_idx, td::uint32 att,
                                                         const ton_api::validatorSession_round_Message* action,
                                                         const ValidatorSessionRoundState* round);
  template <class T>
  static const ValidatorSessionRoundAttemptState* action(ValidatorSessionDescription& desc,
                                                         const ValidatorSessionRoundAttemptState* state,
                                                         td::uint32 src_idx, td::uint32 att, const T& action,
                                                         const ValidatorSessionRoundState* round) {
    UNREACHABLE();
  }
  static const ValidatorSessionRoundAttemptState* try_vote(ValidatorSessionDescription& desc,
                                                           const ValidatorSessionRoundAttemptState* state,
                                                           td::uint32 src_idx, td::uint32 att,
                                                           const ValidatorSessionRoundState* round,
                                                           const ton_api::validatorSession_round_Message* cmp,
                                                           bool& made);
  static const ValidatorSessionRoundAttemptState* try_precommit(ValidatorSessionDescription& desc,
                                                                const ValidatorSessionRoundAttemptState* state,
                                                                td::uint32 src_idx, td::uint32 att,
                                                                const ValidatorSessionRoundState* round,
                                                                const ton_api::validatorSession_round_Message* cmp,
                                                                bool& made);
  static const ValidatorSessionRoundAttemptState* make_one(ValidatorSessionDescription& desc,
                                                           const ValidatorSessionRoundAttemptState* state,
                                                           td::uint32 src_idx, td::uint32 att,
                                                           const ValidatorSessionRoundState* round,
                                                           const ton_api::validatorSession_round_Message* cmp,
                                                           bool& made);
  tl_object_ptr<ton_api::validatorSession_round_Message> create_action(ValidatorSessionDescription& desc,
                                                                       const ValidatorSessionRoundState* round,
                                                                       td::uint32 src_idx, td::uint32 att) const;
  void dump(ValidatorSessionDescription& desc, td::StringBuilder& sb) const;

 private:
  const td::uint32 seqno_;
  const VoteVector* votes_;
  const CntVector<bool>* precommitted_;
  const SentBlock* vote_for_;
  const bool vote_for_inited_;
  const HashType hash_;
};

using AttemptVector =
    CntSortedVector<const ValidatorSessionRoundAttemptState*, ValidatorSessionRoundAttemptState::Compare>;


}  // namespace validatorsession

}  // namespace ton
