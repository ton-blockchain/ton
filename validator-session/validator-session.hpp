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

#include <set>
#include <map>
#include <list>
#include <atomic>

#include "validator-session.h"
#include "validator-session-state.h"

#include "keys/encryptor.h"

#include "catchain/catchain.h"

namespace ton {

namespace validatorsession {

class ValidatorSessionImpl : public ValidatorSession {
 private:
  class BlockExtra : public catchain::CatChainBlock::Extra {
   public:
    const ValidatorSessionState *get_ref() const {
      return state_;
    }
    BlockExtra(const ValidatorSessionState *state) : state_(std::move(state)) {
    }

   private:
    const ValidatorSessionState *state_;
  };

  bool requested_new_block_ = false;
  bool requested_new_block_now_ = false;
  const ValidatorSessionState *real_state_ = nullptr;
  const ValidatorSessionState *virtual_state_ = nullptr;

  td::uint32 cur_round_ = 0, first_block_round_ = 0;
  td::Timestamp round_started_at_ = td::Timestamp::never();
  td::Timestamp round_debug_at_ = td::Timestamp::never();
  std::set<ValidatorSessionCandidateId> pending_approve_;
  std::map<ValidatorSessionCandidateId, td::BufferSlice> pending_reject_;
  std::set<ValidatorSessionCandidateId> rejected_;
  std::map<ValidatorSessionCandidateId, std::pair<UnixTime, td::BufferSlice>> approved_;

  std::set<ValidatorSessionCandidateId> active_requests_;

  bool pending_generate_ = false;
  bool generated_ = false;
  bool sent_generated_ = false;
  ValidatorSessionCandidateId generated_block_;

  bool pending_sign_ = false;
  bool signed_ = false;
  ValidatorSessionCandidateId signed_block_;
  td::BufferSlice signature_;

  std::map<ValidatorSessionCandidateId, tl_object_ptr<ton_api::validatorSession_candidate>> blocks_;
  // src_round_candidate_[src_id][round] -> candidate id
  std::vector<std::map<td::uint32, ValidatorSessionCandidateId>> src_round_candidate_;

  catchain::CatChainSessionId unique_hash_;

  std::unique_ptr<Callback> callback_;
  std::string db_root_;
  std::string db_suffix_;

  td::actor::ActorId<keyring::Keyring> keyring_;
  td::actor::ActorId<adnl::Adnl> adnl_;
  td::actor::ActorId<rldp::Rldp> rldp_;
  td::actor::ActorId<overlay::Overlays> overlay_manager_;
  td::actor::ActorOwn<catchain::CatChain> catchain_;
  std::unique_ptr<ValidatorSessionDescription> description_;

  double catchain_max_block_delay_ = 0.4;
  double catchain_max_block_delay_slow_ = 1.0;

  void on_new_round(td::uint32 round);
  void on_catchain_started();
  void check_vote_for_slot(td::uint32 att);
  void check_generate_slot();
  void check_sign_slot();
  void check_approve();
  void check_action(td::uint32 att);
  void check_all();

  std::unique_ptr<catchain::CatChain::Callback> make_catchain_callback() {
    class cb : public catchain::CatChain::Callback {
     public:
      void process_blocks(std::vector<catchain::CatChainBlock *> blocks) override {
        td::actor::send_closure(id_, &ValidatorSessionImpl::process_blocks, std::move(blocks));
      }
      void finished_processing() override {
        td::actor::send_closure(id_, &ValidatorSessionImpl::finished_processing);
      }
      void preprocess_block(catchain::CatChainBlock *block) override {
        td::actor::send_closure(id_, &ValidatorSessionImpl::preprocess_block, block);
      }
      void process_broadcast(const PublicKeyHash &src, td::BufferSlice data) override {
        td::actor::send_closure(id_, &ValidatorSessionImpl::process_broadcast, src, std::move(data),
                                td::optional<ValidatorSessionCandidateId>(), true);
      }
      void process_message(const PublicKeyHash &src, td::BufferSlice data) override {
        td::actor::send_closure(id_, &ValidatorSessionImpl::process_message, src, std::move(data));
      }
      void process_query(const PublicKeyHash &src, td::BufferSlice data,
                         td::Promise<td::BufferSlice> promise) override {
        td::actor::send_closure(id_, &ValidatorSessionImpl::process_query, src, std::move(data), std::move(promise));
      }
      void started() override {
        td::actor::send_closure(id_, &ValidatorSessionImpl::on_catchain_started);
      }

      cb(td::actor::ActorId<ValidatorSessionImpl> id) : id_(id) {
      }

     private:
      td::actor::ActorId<ValidatorSessionImpl> id_;
    };

    return std::make_unique<cb>(actor_id(this));
  }

  auto &description() {
    return *description_.get();
  }

  td::uint32 local_idx() {
    return description_->get_self_idx();
  }
  ton::PublicKeyHash local_id() {
    return description_->get_source_id(description_->get_self_idx());
  }

  void request_new_block(bool now);
  double get_current_max_block_delay() const;
  void get_broadcast_p2p(PublicKeyHash node, ValidatorSessionFileHash file_hash,
                         ValidatorSessionCollatedDataFileHash collated_data_file_hash, PublicKeyHash src,
                         td::uint32 round, ValidatorSessionRootHash root_hash, td::Promise<td::BufferSlice> promise,
                         td::Timestamp timeout);

  bool started_ = false;
  bool catchain_started_ = false;
  bool allow_unsafe_self_blocks_resync_;
  bool compress_block_candidates_ = false;

  ValidatorSessionStats cur_stats_;
  bool stats_inited_ = false;
  std::map<std::pair<td::uint32, ValidatorSessionCandidateId>, std::vector<td::uint32>>
      stats_pending_approve_;  // round, candidate_id -> approvers
  std::map<std::pair<td::uint32, ValidatorSessionCandidateId>, std::vector<td::uint32>>
      stats_pending_sign_;  // round, candidate_id -> signers
  void stats_init();
  void stats_add_round();
  ValidatorSessionStats::Producer *stats_get_candidate_stat(
      td::uint32 round, PublicKeyHash src,
      ValidatorSessionCandidateId candidate_id = ValidatorSessionCandidateId::zero());
  ValidatorSessionStats::Producer *stats_get_candidate_stat_by_id(td::uint32 round,
                                                                  ValidatorSessionCandidateId candidate_id);
  void stats_process_action(td::uint32 node_id, ton_api::validatorSession_round_Message &action);

 public:
  ValidatorSessionImpl(catchain::CatChainSessionId session_id, ValidatorSessionOptions opts, PublicKeyHash local_id,
                       std::vector<ValidatorSessionNode> nodes, std::unique_ptr<Callback> callback,
                       td::actor::ActorId<keyring::Keyring> keyring, td::actor::ActorId<adnl::Adnl> adnl,
                       td::actor::ActorId<rldp::Rldp> rldp, td::actor::ActorId<overlay::Overlays> overlays,
                       std::string db_root, std::string db_suffix, bool allow_unsafe_self_blocks_resync);
  void start_up() override;
  void alarm() override;

  void start() override;
  void destroy() override;
  void get_current_stats(td::Promise<ValidatorSessionStats> promise) override;
  void get_end_stats(td::Promise<EndValidatorGroupStats> promise) override;
  void get_validator_group_info_for_litequery(
      td::uint32 cur_round,
      td::Promise<std::vector<tl_object_ptr<lite_api::liteServer_nonfinal_candidateInfo>>> promise) override;

  void set_catchain_max_block_delay(double delay, double delay_slow) override {
    catchain_max_block_delay_ = delay;
    catchain_max_block_delay_slow_ = delay_slow;
  }

  void process_blocks(std::vector<catchain::CatChainBlock *> blocks);
  void finished_processing();
  void preprocess_block(catchain::CatChainBlock *block);
  bool ensure_candidate_unique(td::uint32 src_idx, td::uint32 round, ValidatorSessionCandidateId block_id);
  void process_broadcast(PublicKeyHash src, td::BufferSlice data, td::optional<ValidatorSessionCandidateId> expected_id,
                         bool is_overlay_broadcast);
  void process_message(PublicKeyHash src, td::BufferSlice data);
  void process_query(PublicKeyHash src, td::BufferSlice data, td::Promise<td::BufferSlice> promise);

  void try_approve_block(const SentBlock *block);

  void candidate_decision_fail(td::uint32 round, ValidatorSessionCandidateId hash, std::string result, td::uint32 src,
                               td::BufferSlice proof, double validation_time, bool validation_cached);
  void candidate_decision_ok(td::uint32 round, ValidatorSessionCandidateId hash, RootHash root_hash, FileHash file_hash,
                             td::uint32 src, td::uint32 ok_from, double validation_time, bool validation_cached);
  void candidate_approved_signed(td::uint32 round, ValidatorSessionCandidateId hash, td::uint32 ok_from,
                                 td::BufferSlice signature);

  void generated_block(td::uint32 round, ValidatorSessionRootHash root_hash, td::BufferSlice data,
                       td::BufferSlice collated, double collation_time, bool collation_cached);
  void signed_block(td::uint32 round, ValidatorSessionCandidateId hash, td::BufferSlice signature);

  void end_request(td::uint32 round, ValidatorSessionCandidateId block_id) {
    if (cur_round_ == round) {
      active_requests_.erase(block_id);
    }
  }

  PrintId print_id() const override {
    return PrintId{unique_hash_, description_->get_source_id(description_->get_self_idx())};
  }

 private:
  static const size_t MAX_REJECT_REASON_SIZE = 1024;
  static const td::int32 MAX_FUTURE_ROUND_BLOCK = 100;
  static const td::int32 MAX_PAST_ROUND_BLOCK = 20;
  constexpr static const double REQUEST_BROADCAST_P2P_DELAY = 2.0;
  static const td::uint32 MAX_CANDIDATE_EXTRA_SIZE = 1024;
};

}  // namespace validatorsession

}  // namespace ton

namespace td {

inline td::StringBuilder &operator<<(td::StringBuilder &sb,
                                     const ton::validatorsession::ValidatorSessionImpl *session) {
  sb << session->print_id();
  return sb;
}

}  // namespace td
