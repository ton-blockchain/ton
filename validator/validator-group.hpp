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

#include "interfaces/validator-manager.h"

#include "validator-session/validator-session.h"

#include "rldp/rldp.h"

namespace ton {

namespace validator {

class ValidatorManager;

class ValidatorGroup : public td::actor::Actor {
 public:
  void generate_block_candidate(td::uint32 round_id, td::Promise<BlockCandidate> promise);
  void validate_block_candidate(td::uint32 round_id, BlockCandidate block, td::Promise<td::uint32> promise);
  void accept_block_candidate(td::uint32 round_id, PublicKeyHash src, td::BufferSlice block, RootHash root_hash,
                              FileHash file_hash, std::vector<BlockSignature> signatures,
                              std::vector<BlockSignature> approve_signatures,
                              validatorsession::ValidatorSessionStats stats, td::Promise<td::Unit> promise);
  void skip_round(td::uint32 round);
  void retry_accept_block_query(BlockIdExt block_id, td::Ref<BlockData> block, std::vector<BlockIdExt> prev,
                                td::Ref<BlockSignatureSet> sigs, td::Ref<BlockSignatureSet> approve_sigs,
                                td::Promise<td::Unit> promise);
  void get_approved_candidate(PublicKey source, RootHash root_hash, FileHash file_hash,
                              FileHash collated_data_file_hash, td::Promise<BlockCandidate> promise);
  BlockIdExt create_next_block_id(RootHash root_hash, FileHash file_hash) const;

  void start(std::vector<BlockIdExt> prev, BlockIdExt min_masterchain_block_id, UnixTime min_ts);
  void create_session();
  void destroy();
  void start_up() override {
    if (init_) {
      init_ = false;
      create_session();
    }
  }

  ValidatorGroup(ShardIdFull shard, PublicKeyHash local_id, ValidatorSessionId session_id,
                 td::Ref<ValidatorSet> validator_set, validatorsession::ValidatorSessionOptions config,
                 td::actor::ActorId<keyring::Keyring> keyring, td::actor::ActorId<adnl::Adnl> adnl,
                 td::actor::ActorId<rldp::Rldp> rldp, td::actor::ActorId<overlay::Overlays> overlays,
                 std::string db_root, td::actor::ActorId<ValidatorManager> validator_manager, bool create_session,
                 bool allow_unsafe_self_blocks_resync)
      : shard_(shard)
      , local_id_(std::move(local_id))
      , session_id_(session_id)
      , validator_set_(std::move(validator_set))
      , config_(std::move(config))
      , keyring_(keyring)
      , adnl_(adnl)
      , rldp_(rldp)
      , overlays_(overlays)
      , db_root_(std::move(db_root))
      , manager_(validator_manager)
      , init_(create_session)
      , allow_unsafe_self_blocks_resync_(allow_unsafe_self_blocks_resync) {
  }

 private:
  std::unique_ptr<validatorsession::ValidatorSession::Callback> make_validator_session_callback();

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
  UnixTime min_ts_;

  td::Ref<ValidatorSet> validator_set_;
  validatorsession::ValidatorSessionOptions config_;

  td::actor::ActorId<keyring::Keyring> keyring_;
  td::actor::ActorId<adnl::Adnl> adnl_;
  td::actor::ActorId<rldp::Rldp> rldp_;
  td::actor::ActorId<overlay::Overlays> overlays_;
  std::string db_root_;
  td::actor::ActorId<ValidatorManager> manager_;
  td::actor::ActorOwn<validatorsession::ValidatorSession> session_;

  bool init_ = false;
  bool started_ = false;
  bool allow_unsafe_self_blocks_resync_;
  td::uint32 last_known_round_id_ = 0;

  struct CachedCollatedBlock {
    td::optional<BlockCandidate> result;
    std::vector<td::Promise<BlockCandidate>> promises;
  };
  std::shared_ptr<CachedCollatedBlock> cached_collated_block_;

  void generated_block_candidate(std::shared_ptr<CachedCollatedBlock> cache, td::Result<BlockCandidate> R);
};

}  // namespace validator

}  // namespace ton
