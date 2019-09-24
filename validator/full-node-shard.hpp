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

    Copyright 2017-2019 Telegram Systems LLP
*/
#pragma once

#include "full-node-shard.h"

namespace ton {

namespace validator {

namespace fullnode {

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
  static constexpr td::uint32 proto_version() {
    return 1;
  }
  static constexpr td::uint64 proto_capabilities() {
    return 0;
  }

  void create_overlay();
  void update_adnl_id(adnl::AdnlNodeIdShort adnl_id, td::Promise<td::Unit> promise) override;

  //td::Result<Block> fetch_block(td::BufferSlice data);
  void prevalidate_block(BlockIdExt block_id, td::BufferSlice data, td::BufferSlice proof,
                         td::Promise<ReceivedBlock> promise);
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
  void process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_downloadBlockProof &query,
                     td::Promise<td::BufferSlice> promise);
  void process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_downloadBlockProofLink &query,
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
  // void process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_prepareNextKeyBlockProof &query,
  //                   td::Promise<td::BufferSlice> promise);
  void receive_query(adnl::AdnlNodeIdShort src, td::BufferSlice query, td::Promise<td::BufferSlice> promise);

  void process_broadcast(PublicKeyHash src, ton_api::tonNode_blockBroadcast &query);
  void process_broadcast(PublicKeyHash src, ton_api::tonNode_ihrMessageBroadcast &query);
  void process_broadcast(PublicKeyHash src, ton_api::tonNode_externalMessageBroadcast &query);
  void process_broadcast(PublicKeyHash src, ton_api::tonNode_newShardBlockBroadcast &query);
  void receive_broadcast(PublicKeyHash src, td::BufferSlice query);

  void send_ihr_message(td::BufferSlice data) override;
  void send_external_message(td::BufferSlice data) override;
  void send_shard_block_info(BlockIdExt block_id, CatchainSeqno cc_seqno, td::BufferSlice data) override;
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

  void set_handle(BlockHandle handle, td::Promise<td::Unit> promise) override;

  void start_up() override;
  void alarm() override;

  void update_validators(std::vector<PublicKeyHash> public_key_hashes, PublicKeyHash local_hash) override;
  void sign_new_certificate(PublicKeyHash sign_by);
  void signed_new_certificate(ton::overlay::Certificate cert);

  FullNodeShardImpl(ShardIdFull shard, PublicKeyHash local_id, adnl::AdnlNodeIdShort adnl_id,
                    FileHash zero_state_file_hash, td::actor::ActorId<keyring::Keyring> keyring,
                    td::actor::ActorId<adnl::Adnl> adnl, td::actor::ActorId<rldp::Rldp> rldp,
                    td::actor::ActorId<overlay::Overlays> overlays,
                    td::actor::ActorId<ValidatorManagerInterface> validator_manager,
                    td::actor::ActorId<adnl::AdnlExtClient> client);

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
  td::actor::ActorId<overlay::Overlays> overlays_;
  td::actor::ActorId<ValidatorManagerInterface> validator_manager_;
  td::actor::ActorId<adnl::AdnlExtClient> client_;

  td::uint32 attempt_ = 0;

  overlay::OverlayIdFull overlay_id_full_;
  overlay::OverlayIdShort overlay_id_;
  PublicKeyHash sign_cert_by_ = PublicKeyHash::zero();
  td::Timestamp update_certificate_at_;
  td::Timestamp sync_completed_at_;

  std::shared_ptr<ton::overlay::Certificate> cert_;
  overlay::OverlayPrivacyRules rules_;
};

}  // namespace fullnode

}  // namespace validator

}  // namespace ton
