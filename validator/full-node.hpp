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
#include "full-node-private-overlay-v2.hpp"

#include <map>
#include <set>
#include <queue>

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
  void add_collator_adnl_id(adnl::AdnlNodeIdShort id) override;
  void del_collator_adnl_id(adnl::AdnlNodeIdShort id) override;

  void sign_shard_overlay_certificate(ShardIdFull shard_id, PublicKeyHash signed_key,
                                      td::uint32 expiry_at, td::uint32 max_size,
                                      td::Promise<td::BufferSlice> promise) override;
  void import_shard_overlay_certificate(ShardIdFull shard_id, PublicKeyHash signed_key,
                                        std::shared_ptr<ton::overlay::Certificate> cert,
                                        td::Promise<td::Unit> promise) override;

  void update_adnl_id(adnl::AdnlNodeIdShort adnl_id, td::Promise<td::Unit> promise) override;
  void set_config(FullNodeConfig config) override;

  void add_custom_overlay(CustomOverlayParams params, td::Promise<td::Unit> promise) override;
  void del_custom_overlay(std::string name, td::Promise<td::Unit> promise) override;

  void update_shard_configuration(td::Ref<MasterchainState> state, std::set<ShardIdFull> shards_to_monitor,
                                  std::set<ShardIdFull> temporary_shards);

  void sync_completed();

  void initial_read_complete(BlockHandle top_block);
  void send_ihr_message(AccountIdPrefixFull dst, td::BufferSlice data);
  void send_ext_message(AccountIdPrefixFull dst, td::BufferSlice data);
  void send_shard_block_info(BlockIdExt block_id, CatchainSeqno cc_seqnp, td::BufferSlice data);
  void send_block_candidate(BlockIdExt block_id, CatchainSeqno cc_seqno, td::uint32 validator_set_hash,
                            td::BufferSlice data);
  void send_broadcast(BlockBroadcast broadcast, bool custom_overlays_only);
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
  void download_archive(BlockSeqno masterchain_seqno, std::string tmp_dir, td::Timestamp timeout,
                        td::Promise<std::string> promise);
  void download_out_msg_queue_proof(ShardIdFull dst_shard, std::vector<BlockIdExt> blocks,
                                    block::ImportedMsgQueueLimits limits, td::Timestamp timeout,
                                    td::Promise<std::vector<td::Ref<OutMsgQueueProof>>> promise);

  void got_key_block_state(td::Ref<ShardState> state);
  void new_key_block(BlockHandle handle);

  void process_block_broadcast(BlockBroadcast broadcast) override;
  void process_block_candidate_broadcast(BlockIdExt block_id, CatchainSeqno cc_seqno, td::uint32 validator_set_hash,
                                         td::BufferSlice data) override;

  void start_up() override;

  FullNodeImpl(PublicKeyHash local_id, adnl::AdnlNodeIdShort adnl_id, FileHash zero_state_file_hash,
               FullNodeConfig config, td::actor::ActorId<keyring::Keyring> keyring, td::actor::ActorId<adnl::Adnl> adnl,
               td::actor::ActorId<rldp::Rldp> rldp, td::actor::ActorId<rldp2::Rldp> rldp2,
               td::actor::ActorId<dht::Dht> dht, td::actor::ActorId<overlay::Overlays> overlays,
               td::actor::ActorId<ValidatorManagerInterface> validator_manager,
               td::actor::ActorId<adnl::AdnlExtClient> client, std::string db_root,
               td::Promise<td::Unit> started_promise);

 private:
  struct ShardInfo {
    bool exists = false;
    td::actor::ActorOwn<FullNodeShard> actor;
    FullNodeShardMode mode = FullNodeShardMode::inactive;
    td::Timestamp delete_at = td::Timestamp::never();
  };

  void add_shard_actor(ShardIdFull shard, FullNodeShardMode mode);

  PublicKeyHash local_id_;
  adnl::AdnlNodeIdShort adnl_id_;
  FileHash zero_state_file_hash_;

  td::actor::ActorId<FullNodeShard> get_shard(AccountIdPrefixFull dst);
  td::actor::ActorId<FullNodeShard> get_shard(ShardIdFull shard);
  std::map<ShardIdFull, ShardInfo> shards_;

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
  std::map<adnl::AdnlNodeIdShort, int> local_collator_nodes_;

  td::Promise<td::Unit> started_promise_;
  FullNodeConfig config_;

  // TODO: Decide what to do with old private overlays. Maybe use old or new depending on some flag in config.
  /*
  std::map<PublicKeyHash, td::actor::ActorOwn<FullNodePrivateOverlay>> private_block_overlays_;
  bool private_block_overlays_enable_compression_ = false;
  void set_private_block_overlays_enable_compression(bool value);
  void create_private_block_overlay(PublicKeyHash key);
  */
  bool broadcast_block_candidates_in_public_overlay_ = false;

  struct CustomOverlayInfo {
    CustomOverlayParams params_;
    std::map<adnl::AdnlNodeIdShort, td::actor::ActorOwn<FullNodeCustomOverlay>> actors_;  // our local id -> actor
  };
  std::map<std::string, CustomOverlayInfo> custom_overlays_;
  std::set<BlockIdExt> custom_overlays_sent_broadcasts_;
  std::queue<BlockIdExt> custom_overlays_sent_broadcasts_lru_;

  void update_private_overlays();
  // void set_private_block_overlays_enable_compression(bool value);
  // void create_private_block_overlay(PublicKeyHash key);
  void update_custom_overlay(CustomOverlayInfo& overlay);
  void send_block_broadcast_to_custom_overlays(const BlockBroadcast& broadcast);
  void send_block_candidate_broadcast_to_custom_overlays(const BlockIdExt& block_id, CatchainSeqno cc_seqno,
                                                         td::uint32 validator_set_hash, const td::BufferSlice& data);
  FullNodePrivateBlockOverlays private_block_overlays_;
};

}  // namespace fullnode

}  // namespace validator

}  // namespace ton
