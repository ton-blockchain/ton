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
#include "validator/fabric.h"
#include "block.hpp"
#include "external-message.hpp"
#include "proof.hpp"
#include "signature-set.hpp"
#include "shard.hpp"
#include "accept-block.hpp"
#include "fake-accept-block.hpp"
#include "check-proof.hpp"
#include "collate-query.hpp"
#include "validate-query.hpp"
#include "top-shard-description.hpp"

#include "validator/db/rootdb.hpp"
#include "validator/block-handle.hpp"
#include "validator/apply-block.hpp"

#include "td/utils/Random.h"

namespace ton {

namespace validator {

td::actor::ActorOwn<Db> create_db_actor(td::actor::ActorId<ValidatorManager> manager, std::string db_root_) {
  return td::actor::create_actor<RootDb>("db", manager, db_root_);
}

td::Result<td::Ref<BlockData>> create_block(BlockIdExt block_id, td::BufferSlice data) {
  return td::Ref<dummy0::Block>{true, block_id, std::move(data)};
}

td::Result<td::Ref<BlockData>> create_block(ReceivedBlock data) {
  return td::Ref<dummy0::Block>{true, data.id, std::move(data.data)};
}

td::Result<td::Ref<Proof>> create_proof(BlockIdExt masterchain_block_id, td::BufferSlice proof) {
  return td::Ref<dummy0::ProofImpl>{true, masterchain_block_id, std::move(proof)};
}

td::Result<td::Ref<ProofLink>> create_proof_link(td::BufferSlice proof) {
  return td::Ref<dummy0::ProofLinkImpl>{true, std::move(proof)};
}

td::Result<td::Ref<BlockSignatureSet>> create_signature_set(td::BufferSlice sig_set) {
  return dummy0::BlockSignatureSetImpl::fetch(std::move(sig_set));
}

td::Result<td::Ref<ShardState>> create_shard_state(BlockIdExt block_id, td::BufferSlice data) {
  return dummy0::ShardStateImpl::fetch(block_id, std::move(data));
}

td::Result<td::Ref<ShardState>> create_shard_state(BlockIdExt block_id, td::Ref<vm::DataCell> root_cell) {
  UNREACHABLE();
}

td::Result<BlockHandle> create_block_handle(td::BufferSlice data) {
  return ton::validator::BlockHandleImpl::create(std::move(data));
}

BlockHandle create_empty_block_handle(BlockIdExt id) {
  return ton::validator::BlockHandleImpl::create_empty(id);
}

//td::Ref<McShardHash> create_mc_shard(ShardIdFull id, ZeroStateIdExt zero_top_block) {
//  return td::Ref<dummy0::McShardHashImpl>{true, zero_top_block};
//}

td::Ref<BlockSignatureSet> create_signature_set(std::vector<BlockSignature> sig_set) {
  return td::Ref<dummy0::BlockSignatureSetImpl>{true, std::move(sig_set)};
}

td::Result<td::Ref<ExtMessage>> create_ext_message(td::BufferSlice data) {
  TRY_RESULT(B, fetch_tl_object<ton_api::test0_extMessage>(std::move(data), true));
  return td::Ref<dummy0::ExtMessageImpl>{true, std::move(B)};
}

void run_accept_block_query(BlockIdExt id, td::Ref<BlockData> data, std::vector<BlockIdExt> prev,
                            td::Ref<ValidatorSet> validator_set, td::Ref<BlockSignatureSet> signatures,
                            bool send_broadcast, td::actor::ActorId<ValidatorManager> manager,
                            td::Promise<td::Unit> promise) {
  td::actor::create_actor<dummy0::AcceptBlockQuery>(
      "accept", id, std::move(data), prev, validator_set->get_catchain_seqno(), validator_set->get_validator_set_hash(),
      std::move(signatures), send_broadcast, manager, std::move(promise))
      .release();
}

void run_fake_accept_block_query(BlockIdExt id, td::Ref<BlockData> data, std::vector<BlockIdExt> prev,
                                 td::Ref<ValidatorSet> validator_set, td::actor::ActorId<ValidatorManager> manager,
                                 td::Promise<td::Unit> promise) {
  td::actor::create_actor<FakeAcceptBlockQuery>("fakeaccept", id, std::move(data), std::move(prev), 0, 0,
                                                td::Ref<BlockSignatureSet>{}, std::move(manager), std::move(promise))
      .release();
}

void run_apply_block_query(BlockIdExt id, td::Ref<BlockData> block, td::actor::ActorId<ValidatorManager> manager,
                           td::Timestamp timeout, td::Promise<td::Unit> promise) {
  td::actor::create_actor<ApplyBlock>("apply", id, std::move(block), manager, timeout, std::move(promise)).release();
}

void run_check_proof_query(BlockIdExt id, td::Ref<Proof> proof, td::actor::ActorId<ValidatorManager> manager,
                           td::Timestamp timeout, td::Promise<BlockHandle> promise) {
  td::actor::create_actor<dummy0::CheckProof>("checkproof", id, std::move(proof), manager, timeout, std::move(promise))
      .release();
}

void run_check_proof_link_query(BlockIdExt id, td::Ref<ProofLink> proof, td::actor::ActorId<ValidatorManager> manager,
                                td::Timestamp timeout, td::Promise<BlockHandle> promise) {
  td::actor::create_actor<dummy0::CheckProofLink>("checkprooflink", id, std::move(proof), manager, timeout,
                                                  std::move(promise))
      .release();
}

void run_validate_query(ShardIdFull shard, UnixTime min_ts, BlockIdExt min_masterchain_block_id,
                        std::vector<BlockIdExt> prev, BlockCandidate candidate, td::Ref<ValidatorSet> validator_set,
                        td::actor::ActorId<ValidatorManager> manager, td::Timestamp timeout,
                        td::Promise<ValidateCandidateResult> promise) {
  td::actor::create_actor<dummy0::ValidateQuery>(
      "validateblock", shard, min_ts, min_masterchain_block_id, std::move(prev), std::move(candidate),
      validator_set->get_catchain_seqno(), validator_set->get_validator_set_hash(), manager, timeout,
      std::move(promise))
      .release();
}

void run_collate_query(ShardIdFull shard, td::uint32 min_ts, const BlockIdExt& min_masterchain_block_id,
                       std::vector<BlockIdExt> prev, td::Ref<ValidatorSet> validator_set,
                       td::actor::ActorId<ValidatorManager> manager, td::Timestamp timeout,
                       td::Promise<BlockCandidate> promise) {
  td::actor::create_actor<dummy0::CollateQuery>("collator", shard, min_ts, min_masterchain_block_id, std::move(prev),
                                                std::move(validator_set), manager, timeout, std::move(promise))
      .release();
}

void run_liteserver_query(td::BufferSlice data, td::actor::ActorId<ValidatorManager> manager,
                          td::Promise<td::BufferSlice> promise) {
  LOG(ERROR) << "answering";
  promise.set_value(serialize_tl_object(create_tl_object<ton_api::testInt>(td::Random::fast_uint32()), true));
}

void run_validate_shard_block_description(td::BufferSlice data, BlockHandle masterchain_block,
                                          td::Ref<MasterchainState> masterchain_state,
                                          td::actor::ActorId<ValidatorManager> manager, td::Timestamp timeout,
                                          td::Promise<td::Ref<ShardTopBlockDescription>> promise, bool is_fake) {
  td::actor::create_actor<dummy0::ValidateShardTopBlockDescription>(
      "topshardfetch", std::move(data), std::move(masterchain_block), std::move(masterchain_state), manager, timeout,
      std::move(promise))
      .release();
}

}  // namespace validator

}  // namespace ton
