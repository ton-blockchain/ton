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

#include <fstream>

#include "td/utils/DecTree.h"

#include "full-node.h"
#include "validator-telemetry.hpp"

namespace ton::validator::fullnode {

class FullNodeCustomOverlay : public td::actor::Actor {
 public:
  void process_broadcast(PublicKeyHash src, ton_api::tonNode_blockBroadcast &query);
  void process_broadcast(PublicKeyHash src, ton_api::tonNode_blockBroadcastCompressed &query);
  void process_broadcast(PublicKeyHash src, ton_api::tonNode_blockBroadcastCompressedV2 &query);
  void process_block_broadcast(PublicKeyHash src, ton_api::tonNode_Broadcast &query);
  void process_broadcast(PublicKeyHash src, ton_api::tonNode_blockFinalityBroadcast &query);

  void obtain_state_for_decompression(PublicKeyHash src, ton_api::tonNode_blockBroadcastCompressedV2 query);
  void process_block_broadcast_with_state(PublicKeyHash src, ton_api::tonNode_blockBroadcastCompressedV2 query,
                                          td::Ref<ShardState> state);

  void process_broadcast(PublicKeyHash src, ton_api::tonNode_externalMessageBroadcast &query);

  void process_broadcast(PublicKeyHash src, ton_api::tonNode_newBlockCandidateBroadcast &query);
  void process_broadcast(PublicKeyHash src, ton_api::tonNode_newBlockCandidateBroadcastCompressed &query);
  void process_broadcast(PublicKeyHash src, ton_api::tonNode_newBlockCandidateBroadcastCompressedV2 &query);
  void process_block_candidate_broadcast(PublicKeyHash src, ton_api::tonNode_Broadcast &query);
  void process_broadcast(PublicKeyHash src, ton_api::tonNode_newShardBlockBroadcast &query);

  template <class T>
  void process_broadcast(PublicKeyHash, T &) {
    VLOG(full_node, WARNING) << "dropping unknown broadcast";
  }
  void receive_broadcast(PublicKeyHash src, td::BufferSlice query);
  void receive_query(adnl::AdnlNodeIdShort src, td::BufferSlice query, td::Promise<td::BufferSlice> promise);

  void send_external_message(td::BufferSlice data);
  void send_broadcast(BlockBroadcast broadcast);
  void send_block_finality_broadcast(BlockFinalityBroadcast finality);
  void send_block_candidate(BlockIdExt block_id, CatchainSeqno cc_seqno, td::uint32 validator_set_hash,
                            td::BufferSlice data);
  void send_shard_block_info(BlockIdExt block_id, CatchainSeqno cc_seqno, td::BufferSlice data);

  void set_config(FullNodeConfig config) {
    opts_.config_ = std::move(config);
  }

  td::actor::Task<QuerySender> get_query_sender();

  void start_up() override;
  void tear_down() override;
  void alarm() override;

  FullNodeCustomOverlay(adnl::AdnlNodeIdShort local_id, CustomOverlayParams params, FileHash zero_state_file_hash,
                        FullNodeOptions opts, td::actor::ActorId<keyring::Keyring> keyring,
                        td::actor::ActorId<adnl::Adnl> adnl, td::actor::ActorId<adnl::AdnlSenderEx> adnl_sender,
                        td::actor::ActorId<overlay::Overlays> overlays,
                        td::actor::ActorId<ValidatorManagerInterface> validator_manager,
                        td::actor::ActorId<FullNode> full_node)
      : local_id_(local_id)
      , name_(std::move(params.name_))
      , nodes_(std::move(params.nodes_))
      , msg_senders_(std::move(params.msg_senders_))
      , block_senders_(std::move(params.block_senders_))
      , accept_queries_(std::move(params.accept_queries_))
      , send_queries_(params.send_queries_)
      , zero_state_file_hash_(zero_state_file_hash)
      , opts_(opts)
      , keyring_(keyring)
      , adnl_(adnl)
      , adnl_sender_(adnl_sender)
      , overlays_(overlays)
      , validator_manager_(validator_manager)
      , full_node_(full_node) {
  }

 private:
  adnl::AdnlNodeIdShort local_id_;
  std::string name_;
  std::vector<adnl::AdnlNodeIdShort> nodes_;
  std::map<adnl::AdnlNodeIdShort, int> msg_senders_;
  std::set<adnl::AdnlNodeIdShort> block_senders_;
  std::set<adnl::AdnlNodeIdShort> accept_queries_;
  bool send_queries_;
  FileHash zero_state_file_hash_;
  FullNodeOptions opts_;

  td::actor::ActorId<keyring::Keyring> keyring_;
  td::actor::ActorId<adnl::Adnl> adnl_;
  td::actor::ActorId<adnl::AdnlSenderEx> adnl_sender_;
  td::actor::ActorId<overlay::Overlays> overlays_;
  td::actor::ActorId<ValidatorManagerInterface> validator_manager_;
  td::actor::ActorId<FullNode> full_node_;

  bool inited_ = false;
  overlay::OverlayIdFull overlay_id_full_;
  overlay::OverlayIdShort overlay_id_;

  void try_init();
  void init();

  struct PeerInfo {
    std::pair<td::uint32, td::uint32> proto_version{0, 0};
    bool alive = false;
  };
  std::map<adnl::AdnlNodeIdShort, PeerInfo> peers_info_;
  td::DecTree<adnl::AdnlNodeIdShort, td::Unit> alive_peers_;
  adnl::AdnlNodeIdShort last_pinged_peer_ = adnl::AdnlNodeIdShort::zero();

  td::actor::Task<> ping_peer(adnl::AdnlNodeIdShort peer_id);
};

}  // namespace ton::validator::fullnode
