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

#include "full-node.h"

namespace ton::validator::fullnode {

class FullNodeFastSyncOverlay : public td::actor::Actor {
 public:
  void process_broadcast(PublicKeyHash src, ton_api::tonNode_blockBroadcast& query);
  void process_broadcast(PublicKeyHash src, ton_api::tonNode_blockBroadcastCompressed& query);
  void process_block_broadcast(PublicKeyHash src, ton_api::tonNode_Broadcast& query);

  void process_broadcast(PublicKeyHash src, ton_api::tonNode_newShardBlockBroadcast& query);

  void process_broadcast(PublicKeyHash src, ton_api::tonNode_newBlockCandidateBroadcast& query);
  void process_broadcast(PublicKeyHash src, ton_api::tonNode_newBlockCandidateBroadcastCompressed& query);
  void process_block_candidate_broadcast(PublicKeyHash src, ton_api::tonNode_Broadcast& query);

  template <class T>
  void process_broadcast(PublicKeyHash, T&) {
    VLOG(FULL_NODE_WARNING) << "dropping unknown broadcast";
  }
  void receive_broadcast(PublicKeyHash src, td::BufferSlice query);

  void send_shard_block_info(BlockIdExt block_id, CatchainSeqno cc_seqno, td::BufferSlice data);
  void send_broadcast(BlockBroadcast broadcast);
  void send_block_candidate(BlockIdExt block_id, CatchainSeqno cc_seqno, td::uint32 validator_set_hash,
                            td::BufferSlice data);

  void start_up() override;
  void tear_down() override;

  void set_validators(std::vector<PublicKeyHash> root_public_keys,
                      std::vector<adnl::AdnlNodeIdShort> current_validators_adnl);
  void set_member_certificate(overlay::OverlayMemberCertificate member_certificate);
  void set_receive_broadcasts(bool value);

  FullNodeFastSyncOverlay(adnl::AdnlNodeIdShort local_id, ShardIdFull shard, FileHash zero_state_file_hash,
                          std::vector<PublicKeyHash> root_public_keys,
                          std::vector<adnl::AdnlNodeIdShort> current_validators_adnl,
                          overlay::OverlayMemberCertificate member_certificate, bool receive_broadcasts,
                          td::actor::ActorId<keyring::Keyring> keyring, td::actor::ActorId<adnl::Adnl> adnl,
                          td::actor::ActorId<overlay::Overlays> overlays,
                          td::actor::ActorId<ValidatorManagerInterface> validator_manager,
                          td::actor::ActorId<FullNode> full_node)
      : local_id_(local_id)
      , shard_(shard)
      , root_public_keys_(std::move(root_public_keys))
      , current_validators_adnl_(std::move(current_validators_adnl))
      , member_certificate_(std::move(member_certificate))
      , receive_broadcasts_(receive_broadcasts)
      , zero_state_file_hash_(zero_state_file_hash)
      , keyring_(keyring)
      , adnl_(adnl)
      , overlays_(overlays)
      , validator_manager_(validator_manager)
      , full_node_(full_node) {
  }

 private:
  adnl::AdnlNodeIdShort local_id_;
  ShardIdFull shard_;
  std::vector<PublicKeyHash> root_public_keys_;
  std::vector<adnl::AdnlNodeIdShort> current_validators_adnl_;
  overlay::OverlayMemberCertificate member_certificate_;
  bool receive_broadcasts_;
  FileHash zero_state_file_hash_;

  td::actor::ActorId<keyring::Keyring> keyring_;
  td::actor::ActorId<adnl::Adnl> adnl_;
  td::actor::ActorId<overlay::Overlays> overlays_;
  td::actor::ActorId<ValidatorManagerInterface> validator_manager_;
  td::actor::ActorId<FullNode> full_node_;

  bool inited_ = false;
  overlay::OverlayIdFull overlay_id_full_;
  overlay::OverlayIdShort overlay_id_;
  UnixTime created_at_ = (UnixTime)td::Clocks::system();

  void try_init();
  void init();
  void get_stats_extra(td::Promise<std::string> promise);
};

class FullNodeFastSyncOverlays {
 public:
  td::actor::ActorId<FullNodeFastSyncOverlay> choose_overlay(ShardIdFull shard);
  void update_overlays(td::Ref<MasterchainState> state, std::set<adnl::AdnlNodeIdShort> my_adnl_ids,
                       std::set<ShardIdFull> monitoring_shards, const FileHash& zero_state_file_hash,
                       const td::actor::ActorId<keyring::Keyring>& keyring, const td::actor::ActorId<adnl::Adnl>& adnl,
                       const td::actor::ActorId<overlay::Overlays>& overlays,
                       const td::actor::ActorId<ValidatorManagerInterface>& validator_manager,
                       const td::actor::ActorId<FullNode>& full_node);
  void add_member_certificate(adnl::AdnlNodeIdShort local_id, overlay::OverlayMemberCertificate member_certificate);

 private:
  struct Overlays {
    std::map<ShardIdFull, td::actor::ActorOwn<FullNodeFastSyncOverlay>> overlays_;
    overlay::OverlayMemberCertificate current_certificate_;
    bool is_validator_{false};
  };

  std::map<adnl::AdnlNodeIdShort, Overlays> id_to_overlays_;  // local_id -> overlays
  std::map<adnl::AdnlNodeIdShort, std::vector<overlay::OverlayMemberCertificate>> member_certificates_;

  td::optional<BlockSeqno> last_key_block_seqno_;
  std::vector<PublicKeyHash> root_public_keys_;
  std::vector<adnl::AdnlNodeIdShort> current_validators_adnl_;
};

}  // namespace ton::validator::fullnode
