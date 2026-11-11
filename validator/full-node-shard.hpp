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

#include "auto/tl/ton_api.h"
#include "td/actor/PromiseFuture.h"
#include "td/utils/port/Poll.h"

#include "full-node-shard.h"

namespace ton {

namespace validator {

namespace fullnode {

struct Neighbour {
  adnl::AdnlNodeIdShort adnl_id;
  td::uint32 version_major = 0;
  td::uint32 version_minor = 0;
  td::uint32 flags = 0;
  double roundtrip = 0;
  double roundtrip_relax_at = 0;
  double roundtrip_weight = 0;
  double unreliability = 0;

  explicit Neighbour(adnl::AdnlNodeIdShort adnl_id) : adnl_id(std::move(adnl_id)) {
  }
  void update_proto_version(ton_api::tonNode_capabilities &q);
  void query_success(double t);
  void query_failed();
  void update_roundtrip(double t);

  std::pair<td::uint32, td::uint32> version() const {
    return {version_major, version_minor};
  }

  static Neighbour zero;
};

class FullNodeShardImpl : public FullNodeShard {
 public:
  WorkchainId get_workchain() const override {
    return shard_.workchain;
  }
  ShardId get_shard() const override {
    return shard_.shard;
  }
  ShardIdFull get_shard_full() const override {
    return shard_;
  }

  static constexpr td::uint32 max_neighbours() {
    return 16;
  }
  static constexpr double stop_unreliability() {
    return 5.0;
  }
  static constexpr double fail_unreliability() {
    return 10.0;
  }

  void create_overlay();
  void update_adnl_id(adnl::AdnlNodeIdShort adnl_id, td::Promise<td::Unit> promise) override;
  void set_active(bool active) override;

  void set_config(FullNodeConfig config) override {
    opts_.config_ = config;
  }
  void set_params(bool active, bool enable_plumtree_broadcast) override;

  void receive_query(adnl::AdnlNodeIdShort src, td::BufferSlice query, td::Promise<td::BufferSlice> promise);
  void receive_message(adnl::AdnlNodeIdShort src, td::BufferSlice data);

  void process_broadcast(PublicKeyHash src, ton_api::tonNode_blockBroadcast &query);
  void process_broadcast(PublicKeyHash src, ton_api::tonNode_blockBroadcastCompressed &query);
  void process_broadcast(PublicKeyHash src, ton_api::tonNode_blockBroadcastCompressedV2 &query);
  void process_broadcast(PublicKeyHash src, ton_api::tonNode_blockFinalityBroadcast &query);
  void process_block_broadcast(PublicKeyHash src, ton_api::tonNode_Broadcast &query);
  void obtain_state_for_decompression(PublicKeyHash src, ton_api::tonNode_blockBroadcastCompressedV2 query);
  void process_block_broadcast_with_state(PublicKeyHash src, ton_api::tonNode_blockBroadcastCompressedV2 query,
                                          td::Ref<ShardState> state);

  void process_broadcast(PublicKeyHash src, ton_api::tonNode_externalMessageBroadcast &query);
  void process_broadcast(PublicKeyHash src, ton_api::tonNode_newShardBlockBroadcast &query);
  void process_broadcast(PublicKeyHash src, ton_api::tonNode_outMsgQueueProofBroadcast &query) {
    LOG(ERROR) << "Ignore outMsgQueueProofBroadcast";
  }

  void process_broadcast(PublicKeyHash src, ton_api::tonNode_newBlockCandidateBroadcast &query);
  void process_broadcast(PublicKeyHash src, ton_api::tonNode_newBlockCandidateBroadcastCompressed &query);
  void process_broadcast(PublicKeyHash src, ton_api::tonNode_newBlockCandidateBroadcastCompressedV2 &query);
  void process_block_candidate_broadcast(PublicKeyHash src, ton_api::tonNode_Broadcast &query);

  void receive_broadcast(PublicKeyHash src, td::BufferSlice query);
  void check_broadcast(PublicKeyHash src, td::BufferSlice query, td::Promise<td::Unit> promise);
  void process_external_message_broadcast(ton_api::tonNode_externalMessageBroadcast &message,
                                          td::Promise<td::Unit> promise);
  void get_stats_extra(td::Promise<std::string> promise);
  void remove_neighbour(adnl::AdnlNodeIdShort id);

  void send_external_message(td::BufferSlice data) override;
  void send_shard_block_info(BlockIdExt block_id, CatchainSeqno cc_seqno, td::BufferSlice data) override;
  void send_block_candidate(BlockIdExt block_id, CatchainSeqno cc_seqno, td::uint32 validator_set_hash,
                            td::BufferSlice data) override;
  void send_broadcast(BlockBroadcast broadcast) override;
  void send_block_finality_broadcast(BlockFinalityBroadcast finality) override;

  void start_up() override;
  void tear_down() override;
  void alarm() override;

  void update_validators(std::vector<PublicKeyHash> public_key_hashes, PublicKeyHash local_hash) override;

  void sign_overlay_certificate(PublicKeyHash signed_key, td::uint32 expiry_at, td::uint32 max_size,
                                td::Promise<td::BufferSlice> promise) override;
  void import_overlay_certificate(PublicKeyHash signed_key, std::shared_ptr<ton::overlay::Certificate> cert,
                                  td::Promise<td::Unit> promise) override;

  td::actor::Task<QuerySender> get_query_sender() override;

  void sign_new_certificate(PublicKeyHash sign_by);
  void signed_new_certificate(overlay::Certificate cert, PublicKeyHash local_id);
  PublicKeyHash choose_outbound_source(td::uint32 payload_size, bool is_fec) const;
  bool has_valid_certificate_for_source(const PublicKeyHash &source,
                                        const std::shared_ptr<ton::overlay::Certificate> &cert, td::uint32 payload_size,
                                        bool is_fec) const;
  PublicKeyHash full_node_adnl_source() const;

  void ping_neighbours();
  void reload_neighbours();
  void got_neighbours(std::vector<adnl::AdnlNodeIdShort> res);
  void update_neighbour_stats(adnl::AdnlNodeIdShort adnl_id, double t, bool success);
  void got_neighbour_capabilities(adnl::AdnlNodeIdShort adnl_id, double t, td::BufferSlice data);
  const Neighbour &choose_neighbour(td::uint32 required_version_major = 0, td::uint32 required_version_minor = 0) const;

  FullNodeShardImpl(ShardIdFull shard, PublicKeyHash local_id, adnl::AdnlNodeIdShort adnl_id,
                    FileHash zero_state_file_hash, FullNodeOptions opts, td::actor::ActorId<keyring::Keyring> keyring,
                    td::actor::ActorId<adnl::Adnl> adnl, td::actor::ActorId<rldp2::Rldp> rldp2,
                    td::actor::ActorId<quic::QuicSender> quic, td::actor::ActorId<overlay::Overlays> overlays,
                    td::actor::ActorId<ValidatorManagerInterface> validator_manager,
                    td::actor::ActorId<FullNode> full_node, bool active, bool enable_plumtree_broadcast);

 private:
  bool use_new_download() const {
    return false;
  }

  ShardIdFull shard_;

  PublicKeyHash local_id_;
  adnl::AdnlNodeIdShort adnl_id_;
  FileHash zero_state_file_hash_;

  td::actor::ActorId<keyring::Keyring> keyring_;
  td::actor::ActorId<adnl::Adnl> adnl_;
  td::actor::ActorId<rldp2::Rldp> rldp2_;
  td::actor::ActorId<quic::QuicSender> quic_;
  td::actor::ActorId<overlay::Overlays> overlays_;
  td::actor::ActorId<ValidatorManagerInterface> validator_manager_;
  td::actor::ActorId<FullNode> full_node_;

  overlay::OverlayIdFull overlay_id_full_;
  overlay::OverlayIdShort overlay_id_;
  PublicKeyHash sign_cert_by_ = PublicKeyHash::zero();
  td::Timestamp update_certificate_at_;

  std::shared_ptr<ton::overlay::Certificate> cert_;
  std::shared_ptr<ton::overlay::Certificate> adnl_source_cert_;
  overlay::OverlayPrivacyRules rules_;

  std::map<adnl::AdnlNodeIdShort, Neighbour> neighbours_;
  td::Timestamp reload_neighbours_at_;
  td::Timestamp ping_neighbours_at_;
  adnl::AdnlNodeIdShort last_pinged_neighbour_ = adnl::AdnlNodeIdShort::zero();

  bool active_;
  bool enable_plumtree_broadcast_;
  bool is_original_sender_ = false;

  FullNodeOptions opts_;

  std::set<td::Bits256> my_ext_msg_broadcasts_;
  std::set<td::Bits256> processed_ext_msg_broadcasts_;
  td::Timestamp cleanup_processed_ext_msg_at_;
};

}  // namespace fullnode

}  // namespace validator

}  // namespace ton
