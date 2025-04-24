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

#include "full-node.h"
#include "full-node-shard.h"
//#include "ton-node-slave.h"
#include "interfaces/proof.h"
#include "interfaces/shard.h"
#include "full-node-private-overlay.hpp"

#include <map>
#include <set>
#include <queue>
#include <token-manager.h>

namespace ton {

namespace validator {

namespace fullnode {

class FullNodeImpl : public FullNode {
 public:
  void update_dht_node(td::actor::ActorId<dht::Dht> dht) override {
    dht_ = dht;
  }

  void add_permanent_key(PublicKeyHash key, td::Promise<td::Unit> promise) override;
  void del_permanent_key(PublicKeyHash key, td::Promise<td::Unit> promise) override;

  void sign_shard_overlay_certificate(ShardIdFull shard_id, PublicKeyHash signed_key, td::uint32 expiry_at,
                                      td::uint32 max_size, td::Promise<td::BufferSlice> promise) override;
  void import_shard_overlay_certificate(ShardIdFull shard_id, PublicKeyHash signed_key,
                                        std::shared_ptr<ton::overlay::Certificate> cert,
                                        td::Promise<td::Unit> promise) override;

  void update_adnl_id(adnl::AdnlNodeIdShort adnl_id, td::Promise<td::Unit> promise) override;
  void set_config(FullNodeConfig config) override;

  void add_custom_overlay(CustomOverlayParams params, td::Promise<td::Unit> promise) override;
  void del_custom_overlay(std::string name, td::Promise<td::Unit> promise) override;

  void on_new_masterchain_block(td::Ref<MasterchainState> state, std::set<ShardIdFull> shards_to_monitor);

  void sync_completed();

  void initial_read_complete(BlockHandle top_block);
  void send_ihr_message(AccountIdPrefixFull dst, td::BufferSlice data);
  void send_ext_message(AccountIdPrefixFull dst, td::BufferSlice data);
  void send_shard_block_info(BlockIdExt block_id, CatchainSeqno cc_seqnp, td::BufferSlice data);
  void send_block_candidate(BlockIdExt block_id, CatchainSeqno cc_seqno, td::uint32 validator_set_hash,
                            td::BufferSlice data);
  void send_broadcast(BlockBroadcast broadcast, int mode);
  void download_block(BlockIdExt id, td::uint32 priority, td::Timestamp timeout, td::Promise<ReceivedBlock> promise);
  void download_zero_state(BlockIdExt id, td::uint32 priority, td::Timestamp timeout,
                           td::Promise<td::BufferSlice> promise);
  void download_persistent_state(BlockIdExt id, BlockIdExt masterchain_block_id, td::uint32 priority,
                                 td::Timestamp timeout, td::Promise<td::BufferSlice> promise);
  void download_block_proof(BlockIdExt block_id, td::uint32 priority, td::Timestamp timeout,
                            td::Promise<td::BufferSlice> promise);
  void download_block_proof_link(BlockIdExt block_id, td::uint32 priority, td::Timestamp timeout,
                                 td::Promise<td::BufferSlice> promise);
  void get_next_key_blocks(BlockIdExt block_id, td::Timestamp timeout, td::Promise<std::vector<BlockIdExt>> promise);
  void download_archive(BlockSeqno masterchain_seqno, ShardIdFull shard_prefix, std::string tmp_dir,
                        td::Timestamp timeout, td::Promise<std::string> promise);
  void download_out_msg_queue_proof(ShardIdFull dst_shard, std::vector<BlockIdExt> blocks,
                                    block::ImportedMsgQueueLimits limits, td::Timestamp timeout,
                                    td::Promise<std::vector<td::Ref<OutMsgQueueProof>>> promise);

  void got_key_block_config(td::Ref<ConfigHolder> config);
  void new_key_block(BlockHandle handle);
  void send_validator_telemetry(PublicKeyHash key, tl_object_ptr<ton_api::validator_telemetry> telemetry);

  void process_block_broadcast(BlockBroadcast broadcast) override;
  void process_block_candidate_broadcast(BlockIdExt block_id, CatchainSeqno cc_seqno, td::uint32 validator_set_hash,
                                         td::BufferSlice data) override;
  void get_out_msg_queue_query_token(td::Promise<std::unique_ptr<ActionToken>> promise) override;

  void set_validator_telemetry_filename(std::string value) override;

  void start_up() override;

  FullNodeImpl(PublicKeyHash local_id, adnl::AdnlNodeIdShort adnl_id, FileHash zero_state_file_hash,
               FullNodeOptions opts, td::actor::ActorId<keyring::Keyring> keyring, td::actor::ActorId<adnl::Adnl> adnl,
               td::actor::ActorId<rldp::Rldp> rldp, td::actor::ActorId<rldp2::Rldp> rldp2,
               td::actor::ActorId<dht::Dht> dht, td::actor::ActorId<overlay::Overlays> overlays,
               td::actor::ActorId<ValidatorManagerInterface> validator_manager,
               td::actor::ActorId<adnl::AdnlExtClient> client, std::string db_root,
               td::Promise<td::Unit> started_promise);

 private:
  struct ShardInfo {
    td::actor::ActorOwn<FullNodeShard> actor;
    bool active = false;
    td::Timestamp delete_at = td::Timestamp::never();
  };

  void update_shard_actor(ShardIdFull shard, bool active);

  PublicKeyHash local_id_;
  adnl::AdnlNodeIdShort adnl_id_;
  FileHash zero_state_file_hash_;

  td::actor::ActorId<FullNodeShard> get_shard(AccountIdPrefixFull dst);
  td::actor::ActorId<FullNodeShard> get_shard(ShardIdFull shard);
  std::map<ShardIdFull, ShardInfo> shards_;
  int wc_monitor_min_split_ = 0;

  td::actor::ActorId<keyring::Keyring> keyring_;
  td::actor::ActorId<adnl::Adnl> adnl_;
  td::actor::ActorId<rldp::Rldp> rldp_;
  td::actor::ActorId<rldp2::Rldp> rldp2_;
  td::actor::ActorId<dht::Dht> dht_;
  td::actor::ActorId<overlay::Overlays> overlays_;
  td::actor::ActorId<ValidatorManagerInterface> validator_manager_;
  td::actor::ActorId<adnl::AdnlExtClient> client_;

  std::string db_root_;

  PublicKeyHash sign_cert_by_;
  std::vector<PublicKeyHash> all_validators_;
  std::map<PublicKeyHash, adnl::AdnlNodeIdShort> current_validators_;

  std::set<PublicKeyHash> local_keys_;

  td::Promise<td::Unit> started_promise_;
  FullNodeOptions opts_;

  std::map<PublicKeyHash, td::actor::ActorOwn<FullNodePrivateBlockOverlay>> private_block_overlays_;
  bool broadcast_block_candidates_in_public_overlay_ = false;

  struct CustomOverlayInfo {
    CustomOverlayParams params_;
    std::map<adnl::AdnlNodeIdShort, td::actor::ActorOwn<FullNodeCustomOverlay>> actors_;  // our local id -> actor
  };
  std::map<std::string, CustomOverlayInfo> custom_overlays_;
  std::set<BlockIdExt> custom_overlays_sent_broadcasts_;
  std::queue<BlockIdExt> custom_overlays_sent_broadcasts_lru_;

  void update_private_overlays();
  void create_private_block_overlay(PublicKeyHash key);
  void update_custom_overlay(CustomOverlayInfo& overlay);
  void send_block_broadcast_to_custom_overlays(const BlockBroadcast& broadcast);
  void send_block_candidate_broadcast_to_custom_overlays(const BlockIdExt& block_id, CatchainSeqno cc_seqno,
                                                         td::uint32 validator_set_hash, const td::BufferSlice& data);

  std::string validator_telemetry_filename_;
  PublicKeyHash validator_telemetry_collector_key_ = PublicKeyHash::zero();

  void update_validator_telemetry_collector();

  td::actor::ActorOwn<TokenManager> out_msg_queue_query_token_manager_ =
      td::actor::create_actor<TokenManager>("tokens", /* max_tokens = */ 1);
};

}  // namespace fullnode

}  // namespace validator

}  // namespace ton
