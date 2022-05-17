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
#include "validator/interfaces/block-handle.h"
#include "adnl/adnl-ext-client.h"

namespace ton {

namespace validator {

namespace fullnode {

class FullNodeShard : public td::actor::Actor {
 public:
  virtual ~FullNodeShard() = default;
  virtual WorkchainId get_workchain() const = 0;
  virtual ShardId get_shard() const = 0;
  virtual ShardIdFull get_shard_full() const = 0;

  virtual void update_adnl_id(adnl::AdnlNodeIdShort adnl_id, td::Promise<td::Unit> promise) = 0;

  virtual void send_ihr_message(td::BufferSlice data) = 0;
  virtual void send_external_message(td::BufferSlice data) = 0;
  virtual void send_shard_block_info(BlockIdExt block_id, CatchainSeqno cc_seqno, td::BufferSlice data) = 0;
  virtual void send_broadcast(BlockBroadcast broadcast) = 0;

  virtual void sign_overlay_certificate(PublicKeyHash signed_key, td::uint32 expiry_at, td::uint32 max_size, td::Promise<td::BufferSlice> promise) = 0;
  virtual void import_overlay_certificate(PublicKeyHash signed_key, std::shared_ptr<ton::overlay::Certificate> cert, td::Promise<td::Unit> promise) = 0;


  virtual void download_block(BlockIdExt id, td::uint32 priority, td::Timestamp timeout,
                              td::Promise<ReceivedBlock> promise) = 0;
  virtual void download_zero_state(BlockIdExt id, td::uint32 priority, td::Timestamp timeout,
                                   td::Promise<td::BufferSlice> promise) = 0;
  virtual void download_persistent_state(BlockIdExt id, BlockIdExt masterchain_block_id, td::uint32 priority,
                                         td::Timestamp timeout, td::Promise<td::BufferSlice> promise) = 0;

  virtual void download_block_proof(BlockIdExt block_id, td::uint32 priority, td::Timestamp timeout,
                                    td::Promise<td::BufferSlice> promise) = 0;
  virtual void download_block_proof_link(BlockIdExt block_id, td::uint32 priority, td::Timestamp timeout,
                                         td::Promise<td::BufferSlice> promise) = 0;
  virtual void get_next_key_blocks(BlockIdExt block_id, td::Timestamp timeout,
                                   td::Promise<std::vector<BlockIdExt>> promise) = 0;
  virtual void download_archive(BlockSeqno masterchain_seqno, std::string tmp_dir, td::Timestamp timeout,
                                td::Promise<std::string> promise) = 0;

  virtual void set_handle(BlockHandle handle, td::Promise<td::Unit> promise) = 0;

  virtual void update_validators(std::vector<PublicKeyHash> public_key_hashes, PublicKeyHash local_hash) = 0;

  static td::actor::ActorOwn<FullNodeShard> create(
      ShardIdFull shard, PublicKeyHash local_id, adnl::AdnlNodeIdShort adnl_id, FileHash zero_state_file_hash,
      td::actor::ActorId<keyring::Keyring> keyring, td::actor::ActorId<adnl::Adnl> adnl,
      td::actor::ActorId<rldp::Rldp> rldp, td::actor::ActorId<overlay::Overlays> overlays,
      td::actor::ActorId<ValidatorManagerInterface> validator_manager, td::actor::ActorId<adnl::AdnlExtClient> client);
};

}  // namespace fullnode

}  // namespace validator

}  // namespace ton
