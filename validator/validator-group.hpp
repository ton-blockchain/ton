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

#include <list>

#include "interfaces/validator-manager.h"
#include "rldp/rldp.h"
#include "rldp2/rldp.h"
#include "validator-session/validator-session.h"

#include "collation-manager.hpp"

namespace ton {

namespace validator {

class ValidatorManager;

class ValidatorGroup : public td::actor::Actor {
 public:
  void generate_block_candidate(validatorsession::BlockSourceInfo source_info, td::Promise<GeneratedCandidate> promise);
  void generate_block_candidate_cont(validatorsession::BlockSourceInfo source_info,
                                     td::Promise<GeneratedCandidate> promise, td::CancellationToken cancellation_token);
  void validate_block_candidate(validatorsession::BlockSourceInfo source_info, BlockCandidate block,
                                td::Promise<std::pair<UnixTime, bool>> promise,
                                td::optional<BlockCandidate> optimistic_prev_block);
  void accept_block_candidate(validatorsession::BlockSourceInfo source_info, td::BufferSlice block, RootHash root_hash,
                              FileHash file_hash, std::vector<BlockSignature> signatures,
                              std::vector<BlockSignature> approve_signatures,
                              validatorsession::ValidatorSessionStats stats, td::Promise<td::Unit> promise);
  void skip_round(td::uint32 round);
  void accept_block_query(BlockIdExt block_id, td::Ref<BlockData> block, std::vector<BlockIdExt> prev,
                          td::Ref<BlockSignatureSet> sigs, td::Ref<BlockSignatureSet> approve_sigs,
                          int send_broadcast_mode, td::Promise<td::Unit> promise, bool is_retry = false);
  void get_approved_candidate(PublicKey source, RootHash root_hash, FileHash file_hash,
                              FileHash collated_data_file_hash, td::Promise<BlockCandidate> promise);
  BlockIdExt create_next_block_id(RootHash root_hash, FileHash file_hash) const;
  BlockId create_next_block_id_simple() const;

  void generate_block_optimistic(validatorsession::BlockSourceInfo source_info, td::BufferSlice prev_block,
                                 RootHash prev_root_hash, FileHash prev_file_hash,
                                 td::Promise<GeneratedCandidate> promise);
  void generated_block_optimistic(validatorsession::BlockSourceInfo source_info, td::Result<GeneratedCandidate> R);

  void start(std::vector<BlockIdExt> prev, BlockIdExt min_masterchain_block_id);
  void create_session();
  void destroy();
  void start_up() override {
    if (init_) {
      init_ = false;
      create_session();
    }
    td::actor::send_closure(collation_manager_, &CollationManager::validator_group_started, shard_);
  }
  void tear_down() override {
    td::actor::send_closure(collation_manager_, &CollationManager::validator_group_finished, shard_);
  }

  void get_validator_group_info_for_litequery(
      td::Promise<tl_object_ptr<lite_api::liteServer_nonfinal_validatorGroupInfo>> promise);

  void update_options(td::Ref<ValidatorManagerOptions> opts, bool apply_blocks) {
    opts_ = std::move(opts);
    monitoring_shard_ = apply_blocks;
  }

  ValidatorGroup(ShardIdFull shard, PublicKeyHash local_id, ValidatorSessionId session_id,
                 td::Ref<ValidatorSet> validator_set, BlockSeqno last_key_block_seqno,
                 validatorsession::ValidatorSessionOptions config, td::actor::ActorId<keyring::Keyring> keyring,
                 td::actor::ActorId<adnl::Adnl> adnl, td::actor::ActorId<rldp::Rldp> rldp,
                 td::actor::ActorId<rldp2::Rldp> rldp2, td::actor::ActorId<overlay::Overlays> overlays,
                 std::string db_root, td::actor::ActorId<ValidatorManager> validator_manager,
                 td::actor::ActorId<CollationManager> collation_manager, bool create_session,
                 bool allow_unsafe_self_blocks_resync, td::Ref<ValidatorManagerOptions> opts, bool monitoring_shard)
      : shard_(shard)
      , local_id_(std::move(local_id))
      , session_id_(session_id)
      , validator_set_(std::move(validator_set))
      , last_key_block_seqno_(last_key_block_seqno)
      , config_(std::move(config))
      , keyring_(keyring)
      , adnl_(adnl)
      , rldp_(rldp)
      , rldp2_(rldp2)
      , overlays_(overlays)
      , db_root_(std::move(db_root))
      , manager_(validator_manager)
      , collation_manager_(collation_manager)
      , init_(create_session)
      , allow_unsafe_self_blocks_resync_(allow_unsafe_self_blocks_resync)
      , opts_(std::move(opts))
      , monitoring_shard_(monitoring_shard) {
  }

 private:
  std::unique_ptr<validatorsession::ValidatorSession::Callback> make_validator_session_callback();
  void destroy_cont();

  struct PostponedAccept {
    RootHash root_hash;
    FileHash file_hash;
    td::BufferSlice block;
    td::Ref<BlockSignatureSet> sigs;
    td::Ref<BlockSignatureSet> approve_sigs;
    validatorsession::ValidatorSessionStats stats;
    td::Promise<td::Unit> promise;
  };

  std::list<PostponedAccept> postponed_accept_;

  ShardIdFull shard_;
  PublicKeyHash local_id_;
  PublicKey local_id_full_;
  ValidatorSessionId session_id_;

  std::vector<BlockIdExt> prev_block_ids_;
  BlockIdExt min_masterchain_block_id_;

  td::Ref<ValidatorSet> validator_set_;
  BlockSeqno last_key_block_seqno_;
  validatorsession::ValidatorSessionOptions config_;

  td::actor::ActorId<keyring::Keyring> keyring_;
  td::actor::ActorId<adnl::Adnl> adnl_;
  td::actor::ActorId<rldp::Rldp> rldp_;
  td::actor::ActorId<rldp2::Rldp> rldp2_;
  td::actor::ActorId<overlay::Overlays> overlays_;
  std::string db_root_;
  td::actor::ActorId<ValidatorManager> manager_;
  td::actor::ActorId<CollationManager> collation_manager_;
  td::actor::ActorOwn<validatorsession::ValidatorSession> session_;
  adnl::AdnlNodeIdShort local_adnl_id_;

  bool init_ = false;
  bool started_ = false;
  bool allow_unsafe_self_blocks_resync_;
  td::Ref<ValidatorManagerOptions> opts_;
  td::uint32 last_known_round_id_ = 0;
  bool monitoring_shard_ = true;
  bool destroying_ = false;

  struct CachedCollatedBlock {
    td::optional<GeneratedCandidate> result;
    std::vector<td::Promise<GeneratedCandidate>> promises;
  };
  std::shared_ptr<CachedCollatedBlock> cached_collated_block_;
  td::CancellationTokenSource cancellation_token_source_;

  void update_round_id(td::uint32 round);

  void generated_block_candidate(validatorsession::BlockSourceInfo source_info,
                                 std::shared_ptr<CachedCollatedBlock> cache, td::Result<GeneratedCandidate> R);

  using CacheKey = std::tuple<td::Bits256, BlockIdExt, FileHash, FileHash>;
  std::map<CacheKey, UnixTime> approved_candidates_cache_;

  void update_approve_cache(CacheKey key, UnixTime value);

  static CacheKey block_to_cache_key(const BlockCandidate& block) {
    return std::make_tuple(block.pubkey.as_bits256(), block.id, sha256_bits256(block.data), block.collated_file_hash);
  }

  void get_validator_group_info_for_litequery_cont(
      td::uint32 expected_round, std::vector<tl_object_ptr<lite_api::liteServer_nonfinal_candidateInfo>> candidates,
      td::Promise<tl_object_ptr<lite_api::liteServer_nonfinal_validatorGroupInfo>> promise);

  std::set<std::tuple<td::Bits256, BlockIdExt, FileHash>> available_block_candidates_;  // source, id, collated hash

  void add_available_block_candidate(td::Bits256 source, BlockIdExt id, FileHash collated_data_hash) {
    available_block_candidates_.emplace(source, id, collated_data_hash);
  }

  std::set<BlockIdExt> sent_candidate_broadcasts_;
  std::map<BlockIdExt, adnl::AdnlNodeIdShort> block_collator_node_id_;

  void send_block_candidate_broadcast(BlockIdExt id, td::BufferSlice data);

  struct OptimisticGeneration {
    td::uint32 round = 0;
    BlockIdExt prev;
    td::optional<GeneratedCandidate> result;
    td::CancellationTokenSource cancellation_token_source;
    std::vector<td::Promise<GeneratedCandidate>> promises;

    ~OptimisticGeneration() {
      for (auto& promise : promises) {
        promise.set_error(td::Status::Error(ErrorCode::cancelled, "Cancelled"));
      }
    }
  };
  std::unique_ptr<OptimisticGeneration> optimistic_generation_;
};

}  // namespace validator

}  // namespace ton
