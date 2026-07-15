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

#include "full-node-shard.h"
#include "full-node.h"
//#include "ton-node-slave.h"
#include <cstddef>
#include <map>
#include <queue>
#include <set>
#include <token-manager.h>

#include "interfaces/proof.h"
#include "interfaces/shard.h"
#include "td/utils/LRUCache.h"

#include "full-node-custom-overlays.hpp"
#include "full-node-fast-sync-overlays.hpp"
#include "full-node-queries.hpp"
#include "full-node-shard.h"
#include "full-node.h"
#include "rate-limiter.h"

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
  td::actor::Task<> send_ext_message(AccountIdPrefixFull dst, td::BufferSlice data);
  void send_shard_block_info(BlockIdExt block_id, CatchainSeqno cc_seqno, td::BufferSlice data);
  void send_block_candidate(BlockIdExt block_id, CatchainSeqno cc_seqno, td::uint32 validator_set_hash,
                            td::BufferSlice data, int mode);
  void send_block_finality_broadcast(BlockFinalityBroadcast finality, int mode);

  td::actor::Task<QuerySender> get_query_sender(ShardIdFull shard_id, bool historical = false);

  td::actor::Task<ReceivedBlock> download_block(BlockIdExt id, td::uint32 priority, td::Timestamp timeout);
  td::actor::Task<td::BufferSlice> download_zero_state(BlockIdExt id, td::uint32 priority, td::Timestamp timeout);
  td::actor::Task<td::BufferSlice> download_persistent_state(BlockIdExt id, BlockIdExt masterchain_block_id,
                                                             PersistentStateType type, td::uint32 priority,
                                                             td::Timestamp timeout);
  td::actor::Task<td::BufferSlice> download_block_proof(BlockIdExt block_id, td::uint32 priority,
                                                        td::Timestamp timeout);
  td::actor::Task<td::BufferSlice> download_block_proof_link(BlockIdExt block_id, td::uint32 priority,
                                                             td::Timestamp timeout);
  td::actor::Task<std::vector<BlockIdExt>> get_next_key_blocks(BlockIdExt block_id, td::Timestamp timeout);
  td::actor::Task<std::string> download_archive(BlockSeqno masterchain_seqno, ShardIdFull shard_prefix,
                                                std::string tmp_dir, td::Timestamp timeout);

  td::actor::Task<> get_next_blocks_loop();

  void got_key_block_config(td::Ref<ConfigHolder> config);
  void new_key_block(BlockHandle handle);

  void process_block_finality_broadcast(BlockFinalityBroadcast finality, BroadcastSource source,
                                        bool send_to_custom) override;
  void process_block_candidate_broadcast(BlockIdExt block_id, CatchainSeqno cc_seqno, td::uint32 validator_set_hash,
                                         td::BufferSlice data, BroadcastSource source, bool send_to_custom) override;
  void process_shard_block_info_broadcast(BlockIdExt block_id, CatchainSeqno cc_seqno, td::BufferSlice data,
                                          bool send_to_custom) override;
  void get_out_msg_queue_query_token(td::Promise<std::unique_ptr<ActionToken>> promise) override;

  void set_validator_telemetry_filename(std::string value) override;
  void set_plumtree_stats_filename(std::string value) override;
  void alarm() override;

  void import_fast_sync_member_certificate(adnl::AdnlNodeIdShort local_id,
                                           overlay::OverlayMemberCertificate cert) override {
    VLOG(full_node, DEBUG) << "Importing fast sync overlay certificate for " << local_id << " issued by "
                           << cert.issued_by().compute_short_id() << " expires in "
                           << (double)cert.expire_at() - td::Clocks::system();
    fast_sync_overlays_.add_member_certificate(local_id, std::move(cert));
  }

  td::actor::Task<td::BufferSlice> handle_query(td::BufferSlice query, adnl::AdnlNodeIdShort src,
                                                QuerySource source) override;

  void start_up() override;

  FullNodeImpl(PublicKeyHash local_id, adnl::AdnlNodeIdShort adnl_id, FileHash zero_state_file_hash,
               FullNodeOptions opts, td::actor::ActorId<keyring::Keyring> keyring, td::actor::ActorId<adnl::Adnl> adnl,
               td::actor::ActorId<rldp2::Rldp> rldp2, td::actor::ActorId<quic::QuicSender> quic,
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

  td::actor::ActorId<FullNodeShard> get_shard_overlay_actor(AccountIdPrefixFull dst);
  td::actor::ActorId<FullNodeShard> get_shard_overlay_actor(ShardIdFull shard, bool historical = false);
  std::map<ShardIdFull, ShardInfo> shards_;
  int wc_monitor_min_split_ = 0;

  td::actor::ActorId<keyring::Keyring> keyring_;
  td::actor::ActorId<adnl::Adnl> adnl_;
  td::actor::ActorId<rldp2::Rldp> rldp2_;
  td::actor::ActorId<quic::QuicSender> quic_;
  td::actor::ActorId<dht::Dht> dht_;
  td::actor::ActorId<overlay::Overlays> overlays_;
  td::actor::ActorId<ValidatorManagerInterface> validator_manager_;

  td::actor::ActorId<adnl::AdnlExtClient> client_;
  QuerySender client_query_sender_;

  std::string db_root_;

  PublicKeyHash sign_cert_by_;
  std::vector<PublicKeyHash> all_validators_;
  std::map<PublicKeyHash, adnl::AdnlNodeIdShort> current_validators_;

  std::set<PublicKeyHash> local_keys_;
  std::map<adnl::AdnlNodeIdShort, int> local_collator_nodes_;

  td::Promise<td::Unit> started_promise_;
  FullNodeOptions opts_;

  BlockHandle handle_;
  td::Promise<> sync_promise_;
  td::Timestamp sync_completed_at_;

  FullNodeFastSyncOverlays fast_sync_overlays_;

  struct CustomOverlayInfo {
    CustomOverlayParams params_;
    std::map<adnl::AdnlNodeIdShort, td::actor::ActorOwn<FullNodeCustomOverlay>> actors_;  // our local id -> actor
  };
  std::map<std::string, CustomOverlayInfo> custom_overlays_;
  td::LRUCache<BlockIdExt, td::Unit> custom_overlays_sent_broadcasts_{10000};
  td::LRUCache<BlockIdExt, td::Unit> custom_overlays_sent_finality_{10000};
  td::LRUCache<BlockIdExt, td::Unit> custom_overlays_sent_shard_block_desc_{10000};

  void update_private_overlays();
  void update_custom_overlay(CustomOverlayInfo& overlay);
  void send_block_finality_broadcast_to_custom_overlays(const BlockFinalityBroadcast& finality);
  void send_block_candidate_broadcast_to_custom_overlays(const BlockIdExt& block_id, CatchainSeqno cc_seqno,
                                                         td::uint32 validator_set_hash, const td::BufferSlice& data);
  void send_shard_block_info_to_custom_overlays(BlockIdExt block_id, CatchainSeqno cc_seqno,
                                                const td::BufferSlice& data);

  std::string validator_telemetry_filename_;
  PublicKeyHash validator_telemetry_collector_key_ = PublicKeyHash::zero();

  void update_validator_telemetry_collector();
  void update_plumtree_stats_collector();

  // Fractions of the Plumtree stats epoch: +15..+30 minutes at the one-hour
  static constexpr double PLUMTREE_STATS_EXCHANGE_FROM = 1.0 / 4;
  static constexpr double PLUMTREE_STATS_EXCHANGE_TO = 1.0 / 2;
  static constexpr std::size_t PLUMTREE_STATS_EXCHANGE_OVERLAYS_LIMIT = 4;
  std::string plumtree_stats_filename_;
  PublicKeyHash plumtree_stats_collector_key_ = PublicKeyHash::zero();
  td::int64 plumtree_stats_exchange_epoch_ = -1;
  td::Timestamp plumtree_stats_exchange_at_ = td::Timestamp::never();

  void schedule_plumtree_stats_exchange();
  void start_plumtree_stats_exchange();
  td::actor::ActorOwn<TokenManager> out_msg_queue_query_token_manager_ =
      td::actor::create_actor<TokenManager>("tokens", /* max_tokens = */ 1);

  // Separate handlers for separate rate limiters
  FullNodeQueryHandler query_handler_public_;
  FullNodeQueryHandler query_handler_fast_sync_;
  FullNodeQueryHandler query_handler_custom_;

  static std::shared_ptr<RateLimiter<>> make_rate_limiter(const FullNodeOptions::RateLimiterParams& params);
};

}  // namespace fullnode

}  // namespace validator

}  // namespace ton
