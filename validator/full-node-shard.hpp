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

#include "auto/tl/ton_api.h"
#include "full-node-shard.h"
#include "td/actor/PromiseFuture.h"
#include "td/utils/port/Poll.h"
#include <set>

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

  static constexpr td::uint32 download_next_priority() {
    return 1;
  }
  static constexpr td::uint32 proto_version_major() {
    return 3;
  }
  static constexpr td::uint32 proto_version_minor() {
    return 0;
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

  void try_get_next_block(td::Timestamp timestamp, td::Promise<ReceivedBlock> promise);
  void got_next_block(td::Result<BlockHandle> block);
  void get_next_block();

  template <class T>
  void process_query(adnl::AdnlNodeIdShort src, T &query, td::Promise<td::BufferSlice> promise) {
    promise.set_error(td::Status::Error(ErrorCode::error, "unknown query"));
  }
  void process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_getNextBlockDescription &query,
                     td::Promise<td::BufferSlice> promise);
  void process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_prepareBlockProof &query,
                     td::Promise<td::BufferSlice> promise);
  void process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_prepareKeyBlockProof &query,
                     td::Promise<td::BufferSlice> promise);
  void process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_downloadBlockProof &query,
                     td::Promise<td::BufferSlice> promise);
  void process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_downloadBlockProofLink &query,
                     td::Promise<td::BufferSlice> promise);
  void process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_downloadKeyBlockProof &query,
                     td::Promise<td::BufferSlice> promise);
  void process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_downloadKeyBlockProofLink &query,
                     td::Promise<td::BufferSlice> promise);
  void process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_prepareBlock &query,
                     td::Promise<td::BufferSlice> promise);
  void process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_downloadBlock &query,
                     td::Promise<td::BufferSlice> promise);
  void process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_downloadBlockFull &query,
                     td::Promise<td::BufferSlice> promise);
  void process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_downloadNextBlockFull &query,
                     td::Promise<td::BufferSlice> promise);
  void process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_prepareZeroState &query,
                     td::Promise<td::BufferSlice> promise);
  void process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_preparePersistentState &query,
                     td::Promise<td::BufferSlice> promise);
  void process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_getPersistentStateSize &query,
                     td::Promise<td::BufferSlice> promise);
  void process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_getNextKeyBlockIds &query,
                     td::Promise<td::BufferSlice> promise);
  void process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_downloadZeroState &query,
                     td::Promise<td::BufferSlice> promise);
  void process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_downloadPersistentState &query,
                     td::Promise<td::BufferSlice> promise);
  void process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_downloadPersistentStateSlice &query,
                     td::Promise<td::BufferSlice> promise);
  void process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_getCapabilities &query,
                     td::Promise<td::BufferSlice> promise);
  void process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_getArchiveInfo &query,
                     td::Promise<td::BufferSlice> promise);
  void process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_getShardArchiveInfo &query,
                     td::Promise<td::BufferSlice> promise);
  void process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_getArchiveSlice &query,
                     td::Promise<td::BufferSlice> promise);
  void process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_getOutMsgQueueProof &query,
                     td::Promise<td::BufferSlice> promise);
  void receive_query(adnl::AdnlNodeIdShort src, td::BufferSlice query, td::Promise<td::BufferSlice> promise);
  void receive_message(adnl::AdnlNodeIdShort src, td::BufferSlice data);

  void process_broadcast(PublicKeyHash src, ton_api::tonNode_blockBroadcast &query);
  void process_broadcast(PublicKeyHash src, ton_api::tonNode_blockBroadcastCompressed &query);
  void process_block_broadcast(PublicKeyHash src, ton_api::tonNode_Broadcast &query);

  void process_broadcast(PublicKeyHash src, ton_api::tonNode_ihrMessageBroadcast &query);
  void process_broadcast(PublicKeyHash src, ton_api::tonNode_externalMessageBroadcast &query);
  void process_broadcast(PublicKeyHash src, ton_api::tonNode_newShardBlockBroadcast &query);

  void process_broadcast(PublicKeyHash src, ton_api::tonNode_newBlockCandidateBroadcast &query);
  void process_broadcast(PublicKeyHash src, ton_api::tonNode_newBlockCandidateBroadcastCompressed &query);
  void process_block_candidate_broadcast(PublicKeyHash src, ton_api::tonNode_Broadcast &query);

  void receive_broadcast(PublicKeyHash src, td::BufferSlice query);
  void check_broadcast(PublicKeyHash src, td::BufferSlice query, td::Promise<td::Unit> promise);
  void get_stats_extra(td::Promise<std::string> promise);
  void remove_neighbour(adnl::AdnlNodeIdShort id);

  void send_ihr_message(td::BufferSlice data) override;
  void send_external_message(td::BufferSlice data) override;
  void send_shard_block_info(BlockIdExt block_id, CatchainSeqno cc_seqno, td::BufferSlice data) override;
  void send_block_candidate(BlockIdExt block_id, CatchainSeqno cc_seqno, td::uint32 validator_set_hash,
                            td::BufferSlice data) override;
  void send_broadcast(BlockBroadcast broadcast) override;

  void download_block(BlockIdExt id, td::uint32 priority, td::Timestamp timeout,
                      td::Promise<ReceivedBlock> promise) override;
  void download_zero_state(BlockIdExt id, td::uint32 priority, td::Timestamp timeout,
                           td::Promise<td::BufferSlice> promise) override;
  void download_persistent_state(BlockIdExt id, BlockIdExt masterchain_block_id, td::uint32 priority,
                                 td::Timestamp timeout, td::Promise<td::BufferSlice> promise) override;

  void download_block_proof(BlockIdExt block_id, td::uint32 priority, td::Timestamp timeout,
                            td::Promise<td::BufferSlice> promise) override;
  void download_block_proof_link(BlockIdExt block_id, td::uint32 priority, td::Timestamp timeout,
                                 td::Promise<td::BufferSlice> promise) override;
  void get_next_key_blocks(BlockIdExt block_id, td::Timestamp timeout,
                           td::Promise<std::vector<BlockIdExt>> promise) override;
  void download_archive(BlockSeqno masterchain_seqno, ShardIdFull shard_prefix, std::string tmp_dir,
                        td::Timestamp timeout, td::Promise<std::string> promise) override;
  void download_out_msg_queue_proof(ShardIdFull dst_shard, std::vector<BlockIdExt> blocks,
                                    block::ImportedMsgQueueLimits limits, td::Timestamp timeout,
                                    td::Promise<std::vector<td::Ref<OutMsgQueueProof>>> promise) override;

  void set_handle(BlockHandle handle, td::Promise<td::Unit> promise) override;

  void start_up() override;
  void tear_down() override;
  void alarm() override;

  void update_validators(std::vector<PublicKeyHash> public_key_hashes, PublicKeyHash local_hash) override;

  void sign_overlay_certificate(PublicKeyHash signed_key, td::uint32 expiry_at, td::uint32 max_size, td::Promise<td::BufferSlice> promise) override;
  void import_overlay_certificate(PublicKeyHash signed_key, std::shared_ptr<ton::overlay::Certificate> cert, td::Promise<td::Unit> promise) override;

  void sign_new_certificate(PublicKeyHash sign_by);
  void signed_new_certificate(ton::overlay::Certificate cert);

  void ping_neighbours();
  void reload_neighbours();
  void got_neighbours(std::vector<adnl::AdnlNodeIdShort> res);
  void update_neighbour_stats(adnl::AdnlNodeIdShort adnl_id, double t, bool success);
  void got_neighbour_capabilities(adnl::AdnlNodeIdShort adnl_id, double t, td::BufferSlice data);
  const Neighbour &choose_neighbour() const;

  template <typename T>
  td::Promise<T> create_neighbour_promise(const Neighbour &x, td::Promise<T> p) {
    return td::PromiseCreator::lambda([id = x.adnl_id, SelfId = actor_id(this), p = std::move(p),
                                       ts = td::Time::now()](td::Result<T> R) mutable {
      if (R.is_error() && R.error().code() != ErrorCode::notready && R.error().code() != ErrorCode::cancelled) {
        td::actor::send_closure(SelfId, &FullNodeShardImpl::update_neighbour_stats, id, td::Time::now() - ts, false);
      } else {
        td::actor::send_closure(SelfId, &FullNodeShardImpl::update_neighbour_stats, id, td::Time::now() - ts, true);
      }
      p.set_result(std::move(R));
    });
  }

  FullNodeShardImpl(ShardIdFull shard, PublicKeyHash local_id, adnl::AdnlNodeIdShort adnl_id,
                    FileHash zero_state_file_hash, FullNodeOptions opts, td::actor::ActorId<keyring::Keyring> keyring,
                    td::actor::ActorId<adnl::Adnl> adnl, td::actor::ActorId<rldp::Rldp> rldp,
                    td::actor::ActorId<rldp2::Rldp> rldp2, td::actor::ActorId<overlay::Overlays> overlays,
                    td::actor::ActorId<ValidatorManagerInterface> validator_manager,
                    td::actor::ActorId<adnl::AdnlExtClient> client, td::actor::ActorId<FullNode> full_node,
                    bool active);

 private:
  bool use_new_download() const {
    return false;
  }

  ShardIdFull shard_;
  BlockHandle handle_;
  td::Promise<td::Unit> promise_;

  PublicKeyHash local_id_;
  adnl::AdnlNodeIdShort adnl_id_;
  FileHash zero_state_file_hash_;

  td::actor::ActorId<keyring::Keyring> keyring_;
  td::actor::ActorId<adnl::Adnl> adnl_;
  td::actor::ActorId<rldp::Rldp> rldp_;
  td::actor::ActorId<rldp2::Rldp> rldp2_;
  td::actor::ActorId<overlay::Overlays> overlays_;
  td::actor::ActorId<ValidatorManagerInterface> validator_manager_;
  td::actor::ActorId<adnl::AdnlExtClient> client_;
  td::actor::ActorId<FullNode> full_node_;

  td::uint32 attempt_ = 0;

  overlay::OverlayIdFull overlay_id_full_;
  overlay::OverlayIdShort overlay_id_;
  PublicKeyHash sign_cert_by_ = PublicKeyHash::zero();
  td::Timestamp update_certificate_at_;
  td::Timestamp sync_completed_at_;

  std::shared_ptr<ton::overlay::Certificate> cert_;
  overlay::OverlayPrivacyRules rules_;

  std::map<adnl::AdnlNodeIdShort, Neighbour> neighbours_;
  td::Timestamp reload_neighbours_at_;
  td::Timestamp ping_neighbours_at_;
  adnl::AdnlNodeIdShort last_pinged_neighbour_ = adnl::AdnlNodeIdShort::zero();

  bool active_;

  FullNodeOptions opts_;

  std::set<td::Bits256> my_ext_msg_broadcasts_;
  std::set<td::Bits256> processed_ext_msg_broadcasts_;
  td::Timestamp cleanup_processed_ext_msg_at_;
};

}  // namespace fullnode

}  // namespace validator

}  // namespace ton
