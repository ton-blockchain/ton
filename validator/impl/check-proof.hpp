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

#include "td/actor/actor.h"
#include "interfaces/block-handle.h"
#include "interfaces/validator-manager.h"

namespace ton {

namespace validator {
using td::Ref;

/*
 *
 * check block proof
 * write proof
 * initialize prev, before_split, after_merge
 * initialize prev's next
 *
 */

class CheckProof : public td::actor::Actor {
 public:
  CheckProof(BlockIdExt id, td::Ref<Proof> proof, td::actor::ActorId<ValidatorManager> manager, td::Timestamp timeout,
             td::Promise<BlockHandle> promise, bool skip_check_signatures, td::Ref<ProofLink> prev_key_proof = {})
      : mode_(prev_key_proof.is_null() ? m_normal : m_relproof)
      , id_(id)
      , proof_(std::move(proof))
      , old_proof_(std::move(prev_key_proof))
      , manager_(manager)
      , timeout_(timeout)
      , promise_(std::move(promise))
      , skip_check_signatures_(skip_check_signatures)
      , perf_timer_("checkproof", 0.1, [manager](double duration) {
          send_closure(manager, &ValidatorManager::add_perf_timer_stat, "checkproof", duration);
        }) {
  }
  CheckProof(BlockIdExt id, td::Ref<Proof> proof, td::actor::ActorId<ValidatorManager> manager, td::Timestamp timeout,
             td::Promise<BlockHandle> promise, bool skip_check_signatures, td::Ref<MasterchainState> known_state)
      : mode_(m_relstate)
      , id_(id)
      , proof_(std::move(proof))
      , manager_(manager)
      , timeout_(timeout)
      , promise_(std::move(promise))
      , state_(std::move(known_state))
      , skip_check_signatures_(skip_check_signatures)
      , perf_timer_("checkproof", 0.1, [manager](double duration) {
          send_closure(manager, &ValidatorManager::add_perf_timer_stat, "checkproof", duration);
        }) {
  }
  CheckProof(BlockIdExt id, td::Ref<ProofLink> proof_link, td::actor::ActorId<ValidatorManager> manager,
             td::Timestamp timeout, td::Promise<BlockHandle> promise)
      : mode_(m_prooflink)
      , id_(id)
      , proof_(std::move(proof_link))
      , manager_(manager)
      , timeout_(timeout)
      , promise_(std::move(promise))
      , perf_timer_("checkproof", 0.1, [manager](double duration) {
          send_closure(manager, &ValidatorManager::add_perf_timer_stat, "checkproof", duration);
        }) {
  }

 private:
  static constexpr td::uint32 priority() {
    return 2;
  }

  void abort_query(td::Status reason);
  void finish_query();
  void alarm() override;

  void start_up() override;
  void got_block_handle(BlockHandle handle);
  void got_masterchain_state(td::Ref<MasterchainState> state);
  void process_masterchain_state();
  void check_signatures(Ref<ValidatorSet> vset);
  void got_block_handle_2(BlockHandle handle);

 private:
  enum { m_normal, m_relproof, m_relstate, m_prooflink } mode_{m_normal};
  BlockIdExt id_, key_id_;
  td::Ref<ProofLink> proof_, old_proof_;

  td::actor::ActorId<ValidatorManager> manager_;
  td::Timestamp timeout_;
  td::Promise<BlockHandle> promise_;

  BlockHandle handle_;
  td::Ref<MasterchainState> state_;
  td::Ref<ValidatorSet> vset_;
  Ref<vm::Cell> proof_root_, sig_root_, old_proof_root_;

  RootHash state_hash_, state_old_hash_;
  LogicalTime lt_;
  UnixTime created_at_;
  bool after_merge_, after_split_, before_split_, want_merge_, want_split_, is_key_block_;
  BlockIdExt mc_blkid_;
  std::vector<BlockIdExt> prev_;
  BlockSeqno prev_key_seqno_{~0U};
  CatchainSeqno catchain_seqno_{0};
  td::uint32 validator_hash_{0};
  td::uint32 sig_count_;
  ValidatorWeight sig_weight_;
  bool skip_check_signatures_{false};
  bool sig_ok_{false};

  td::PerfWarningTimer perf_timer_;

  static bool check_send_error(td::actor::ActorId<CheckProof> SelfId, td::Status error);
  template <typename T>
  static bool check_send_error(td::actor::ActorId<CheckProof> SelfId, td::Result<T>& res) {
    return res.is_error() && check_send_error(std::move(SelfId), res.move_as_error());
  }
  bool fatal_error(std::string err_msg, int err_code = -666);
  bool fatal_error(td::Status error);
  bool init_parse(bool is_aux = false);
  bool is_proof() const {
    return mode_ != m_prooflink;
  }
  bool is_masterchain() const {
    return id_.is_masterchain();
  }
};

}  // namespace validator

}  // namespace ton
