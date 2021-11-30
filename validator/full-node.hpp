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

#include <map>
#include <set>

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

  void sign_shard_overlay_certificate(ShardIdFull shard_id, PublicKeyHash signed_key,
                                      td::uint32 expiry_at, td::uint32 max_size,
                                      td::Promise<td::BufferSlice> promise) override;
  void import_shard_overlay_certificate(ShardIdFull shard_id, PublicKeyHash signed_key,
                                        std::shared_ptr<ton::overlay::Certificate> cert,
                                        td::Promise<td::Unit> promise) override;


  void update_adnl_id(adnl::AdnlNodeIdShort adnl_id, td::Promise<td::Unit> promise) override;

  void add_shard(ShardIdFull shard);
  void del_shard(ShardIdFull shard);

  void sync_completed();

  void initial_read_complete(BlockHandle top_block);
  void send_ihr_message(AccountIdPrefixFull dst, td::BufferSlice data);
  void send_ext_message(AccountIdPrefixFull dst, td::BufferSlice data);
  void send_shard_block_info(BlockIdExt block_id, CatchainSeqno cc_seqnp, td::BufferSlice data);
  void send_broadcast(BlockBroadcast broadcast);
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

  void got_key_block_proof(td::Ref<ProofLink> proof);
  void got_zero_block_state(td::Ref<ShardState> state);
  void new_key_block(BlockHandle handle);

  void start_up() override;

  FullNodeImpl(PublicKeyHash local_id, adnl::AdnlNodeIdShort adnl_id, FileHash zero_state_file_hash,
               td::actor::ActorId<keyring::Keyring> keyring, td::actor::ActorId<adnl::Adnl> adnl,
               td::actor::ActorId<rldp::Rldp> rldp, td::actor::ActorId<dht::Dht> dht,
               td::actor::ActorId<overlay::Overlays> overlays,
               td::actor::ActorId<ValidatorManagerInterface> validator_manager,
               td::actor::ActorId<adnl::AdnlExtClient> client, std::string db_root);

 private:
  PublicKeyHash local_id_;
  adnl::AdnlNodeIdShort adnl_id_;
  FileHash zero_state_file_hash_;

  td::actor::ActorId<FullNodeShard> get_shard(AccountIdPrefixFull dst);
  td::actor::ActorId<FullNodeShard> get_shard(ShardIdFull dst);

  std::map<ShardIdFull, td::actor::ActorOwn<FullNodeShard>> shards_;

  td::actor::ActorId<keyring::Keyring> keyring_;
  td::actor::ActorId<adnl::Adnl> adnl_;
  td::actor::ActorId<rldp::Rldp> rldp_;
  td::actor::ActorId<dht::Dht> dht_;
  td::actor::ActorId<overlay::Overlays> overlays_;
  td::actor::ActorId<ValidatorManagerInterface> validator_manager_;
  td::actor::ActorId<adnl::AdnlExtClient> client_;

  std::string db_root_;

  PublicKeyHash sign_cert_by_;
  std::vector<PublicKeyHash> all_validators_;

  std::set<PublicKeyHash> local_keys_;
};

}  // namespace fullnode

}  // namespace validator

}  // namespace ton
