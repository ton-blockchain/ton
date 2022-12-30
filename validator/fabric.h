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

#include "interfaces/validator-manager.h"
#include "interfaces/db.h"

namespace ton {

namespace validator {

td::actor::ActorOwn<Db> create_db_actor(td::actor::ActorId<ValidatorManager> manager, std::string db_root_);
td::actor::ActorOwn<LiteServerCache> create_liteserver_cache_actor(td::actor::ActorId<ValidatorManager> manager,
                                                                   std::string db_root);

td::Result<td::Ref<BlockData>> create_block(BlockIdExt block_id, td::BufferSlice data);
td::Result<td::Ref<BlockData>> create_block(ReceivedBlock data);
td::Result<td::Ref<Proof>> create_proof(BlockIdExt masterchain_block_id, td::BufferSlice proof);
td::Result<td::Ref<ProofLink>> create_proof_link(BlockIdExt block_id, td::BufferSlice proof);
td::Result<td::Ref<BlockSignatureSet>> create_signature_set(td::BufferSlice sig_set);
td::Result<td::Ref<ShardState>> create_shard_state(BlockIdExt block_id, td::BufferSlice data);
td::Result<td::Ref<ShardState>> create_shard_state(BlockIdExt block_id, td::Ref<vm::DataCell> root_cell);
td::Result<BlockHandle> create_block_handle(td::BufferSlice data);
td::Result<BlockHandle> create_block_handle(td::Slice data);
td::Result<ConstBlockHandle> create_temp_block_handle(td::BufferSlice data);
BlockHandle create_empty_block_handle(BlockIdExt id);
td::Result<td::Ref<ExtMessage>> create_ext_message(td::BufferSlice data,
                                                   block::SizeLimitsConfig::ExtMsgLimits limits);
td::Result<td::Ref<IhrMessage>> create_ihr_message(td::BufferSlice data);
td::Result<std::vector<td::Ref<ShardTopBlockDescription>>> create_new_shard_block_descriptions(td::BufferSlice data);

td::Ref<BlockSignatureSet> create_signature_set(std::vector<BlockSignature> sig_set);

void run_check_external_message(td::BufferSlice data, block::SizeLimitsConfig::ExtMsgLimits limits,
                                td::actor::ActorId<ValidatorManager> manager, td::Promise<td::Ref<ExtMessage>> promise);

void run_accept_block_query(BlockIdExt id, td::Ref<BlockData> data, std::vector<BlockIdExt> prev,
                            td::Ref<ValidatorSet> validator_set, td::Ref<BlockSignatureSet> signatures,
                            td::Ref<BlockSignatureSet> approve_signatures, bool send_broadcast,
                            td::actor::ActorId<ValidatorManager> manager, td::Promise<td::Unit> promise);
void run_fake_accept_block_query(BlockIdExt id, td::Ref<BlockData> data, std::vector<BlockIdExt> prev,
                                 td::Ref<ValidatorSet> validator_set, td::actor::ActorId<ValidatorManager> manager,
                                 td::Promise<td::Unit> promise);
void run_hardfork_accept_block_query(BlockIdExt id, td::Ref<BlockData> data,
                                     td::actor::ActorId<ValidatorManager> manager, td::Promise<td::Unit> promise);
void run_apply_block_query(BlockIdExt id, td::Ref<BlockData> block, BlockIdExt masterchain_block_id,
                           td::actor::ActorId<ValidatorManager> manager, td::Timestamp timeout,
                           td::Promise<td::Unit> promise);
void run_check_proof_query(BlockIdExt id, td::Ref<Proof> proof, td::actor::ActorId<ValidatorManager> manager,
                           td::Timestamp timeout, td::Promise<BlockHandle> promise, bool skip_check_signatures = false);
void run_check_proof_query(BlockIdExt id, td::Ref<Proof> proof, td::actor::ActorId<ValidatorManager> manager,
                           td::Timestamp timeout, td::Promise<BlockHandle> promise,
                           td::Ref<MasterchainState> rel_masterchain_state, bool skip_check_signatures = false);
void run_check_proof_query(BlockIdExt id, td::Ref<Proof> proof, td::actor::ActorId<ValidatorManager> manager,
                           td::Timestamp timeout, td::Promise<BlockHandle> promise,
                           td::Ref<ProofLink> rel_key_block_proof, bool skip_check_signatures = false);
void run_check_proof_link_query(BlockIdExt id, td::Ref<ProofLink> proof, td::actor::ActorId<ValidatorManager> manager,
                                td::Timestamp timeout, td::Promise<BlockHandle> promise);
void run_validate_query(ShardIdFull shard, UnixTime min_ts, BlockIdExt min_masterchain_block_id,
                        std::vector<BlockIdExt> prev, BlockCandidate candidate, td::Ref<ValidatorSet> validator_set,
                        td::actor::ActorId<ValidatorManager> manager, td::Timestamp timeout,
                        td::Promise<ValidateCandidateResult> promise, bool is_fake = false);
void run_collate_query(ShardIdFull shard, td::uint32 min_ts, const BlockIdExt& min_masterchain_block_id,
                       std::vector<BlockIdExt> prev, Ed25519_PublicKey local_id, td::Ref<ValidatorSet> validator_set,
                       td::actor::ActorId<ValidatorManager> manager, td::Timestamp timeout,
                       td::Promise<BlockCandidate> promise);
void run_collate_hardfork(ShardIdFull shard, const BlockIdExt& min_masterchain_block_id, std::vector<BlockIdExt> prev,
                          td::actor::ActorId<ValidatorManager> manager, td::Timestamp timeout,
                          td::Promise<BlockCandidate> promise);
void run_liteserver_query(td::BufferSlice data, td::actor::ActorId<ValidatorManager> manager,
                          td::actor::ActorId<LiteServerCache> cache, td::Promise<td::BufferSlice> promise);
void run_fetch_account_state(WorkchainId wc, StdSmcAddress  addr, td::actor::ActorId<ValidatorManager> manager,
                             td::Promise<std::tuple<td::Ref<vm::CellSlice>,UnixTime,LogicalTime,std::unique_ptr<block::ConfigInfo>>> promise);
void run_validate_shard_block_description(td::BufferSlice data, BlockHandle masterchain_block,
                                          td::Ref<MasterchainState> masterchain_state,
                                          td::actor::ActorId<ValidatorManager> manager, td::Timestamp timeout,
                                          td::Promise<td::Ref<ShardTopBlockDescription>> promise, bool is_fake = false);

}  // namespace validator

}  // namespace ton
