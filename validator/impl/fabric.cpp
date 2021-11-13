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
#include "fabric.h"
#include "collator-impl.h"
#include "validator/db/rootdb.hpp"
#include "validator/block-handle.hpp"
#include "apply-block.hpp"
#include "accept-block.hpp"
#include "shard.hpp"
#include "block.hpp"
#include "proof.hpp"
#include "signature-set.hpp"
#include "external-message.hpp"
#include "ihr-message.hpp"
#include "validate-query.hpp"
#include "check-proof.hpp"
#include "top-shard-descr.hpp"
#include "ton/ton-io.hpp"
#include "liteserver.hpp"
#include "validator/fabric.h"

namespace ton {

namespace validator {

td::actor::ActorOwn<Db> create_db_actor(td::actor::ActorId<ValidatorManager> manager, std::string db_root_) {
  return td::actor::create_actor<RootDb>("db", manager, db_root_);
}

td::actor::ActorOwn<LiteServerCache> create_liteserver_cache_actor(td::actor::ActorId<ValidatorManager> manager,
                                                                   std::string db_root) {
  return td::actor::create_actor<LiteServerCache>("cache");
}

td::Result<td::Ref<BlockData>> create_block(BlockIdExt block_id, td::BufferSlice data) {
  auto res = BlockQ::create(block_id, std::move(data));
  if (res.is_error()) {
    return res.move_as_error();
  } else {
    return td::Ref<BlockData>{res.move_as_ok()};
  }
}

td::Result<td::Ref<BlockData>> create_block(ReceivedBlock data) {
  return create_block(data.id, std::move(data.data));
}

td::Result<td::Ref<Proof>> create_proof(BlockIdExt masterchain_block_id, td::BufferSlice proof) {
  return Ref<ProofQ>{true, masterchain_block_id, std::move(proof)};
}

td::Result<td::Ref<ProofLink>> create_proof_link(BlockIdExt block_id, td::BufferSlice proof_link) {
  return Ref<ProofLinkQ>{true, block_id, std::move(proof_link)};
}

td::Result<td::Ref<BlockSignatureSet>> create_signature_set(td::BufferSlice sig_set) {
  return BlockSignatureSetQ::fetch(std::move(sig_set));
}

td::Result<td::Ref<ShardState>> create_shard_state(BlockIdExt block_id, td::BufferSlice data) {
  auto res = ShardStateQ::fetch(block_id, std::move(data));
  if (res.is_error()) {
    return res.move_as_error();
  } else {
    return td::Ref<ShardState>{res.move_as_ok()};
  }
}

td::Result<td::Ref<ShardState>> create_shard_state(BlockIdExt block_id, td::Ref<vm::DataCell> root_cell) {
  auto res = ShardStateQ::fetch(block_id, {}, std::move(root_cell));
  if (res.is_error()) {
    return res.move_as_error();
  } else {
    return td::Ref<ShardState>{res.move_as_ok()};
  }
}

td::Result<BlockHandle> create_block_handle(td::BufferSlice data) {
  return ton::validator::BlockHandleImpl::create(data.as_slice());
}

td::Result<BlockHandle> create_block_handle(td::Slice data) {
  return ton::validator::BlockHandleImpl::create(data);
}

td::Result<ConstBlockHandle> create_temp_block_handle(td::BufferSlice data) {
  return ton::validator::BlockHandleImpl::create(std::move(data));
}

BlockHandle create_empty_block_handle(BlockIdExt id) {
  return ton::validator::BlockHandleImpl::create_empty(id);
}

td::Ref<BlockSignatureSet> create_signature_set(std::vector<BlockSignature> sig_set) {
  return td::Ref<BlockSignatureSetQ>{true, std::move(sig_set)};
}

td::Result<td::Ref<ExtMessage>> create_ext_message(td::BufferSlice data) {
  TRY_RESULT(res, ExtMessageQ::create_ext_message(std::move(data)));
  return std::move(res);
}

void run_check_external_message(td::BufferSlice data, td::actor::ActorId<ValidatorManager> manager, td::Promise<td::Unit> promise) {
  ExtMessageQ::run_message(std::move(data), std::move(manager), std::move(promise));
}

td::Result<td::Ref<IhrMessage>> create_ihr_message(td::BufferSlice data) {
  TRY_RESULT(res, IhrMessageQ::create_ihr_message(std::move(data)));
  return std::move(res);
}

void run_accept_block_query(BlockIdExt id, td::Ref<BlockData> data, std::vector<BlockIdExt> prev,
                            td::Ref<ValidatorSet> validator_set, td::Ref<BlockSignatureSet> signatures,
                            td::Ref<BlockSignatureSet> approve_signatures, bool send_broadcast,
                            td::actor::ActorId<ValidatorManager> manager, td::Promise<td::Unit> promise) {
  td::actor::create_actor<AcceptBlockQuery>("accept", id, std::move(data), prev, std::move(validator_set),
                                            std::move(signatures), std::move(approve_signatures), send_broadcast,
                                            manager, std::move(promise))
      .release();
}

void run_fake_accept_block_query(BlockIdExt id, td::Ref<BlockData> data, std::vector<BlockIdExt> prev,
                                 td::Ref<ValidatorSet> validator_set, td::actor::ActorId<ValidatorManager> manager,
                                 td::Promise<td::Unit> promise) {
  td::actor::create_actor<AcceptBlockQuery>("fakeaccept", AcceptBlockQuery::IsFake(), id, std::move(data),
                                            std::move(prev), std::move(validator_set), std::move(manager),
                                            std::move(promise))
      .release();
}

void run_hardfork_accept_block_query(BlockIdExt id, td::Ref<BlockData> data,
                                     td::actor::ActorId<ValidatorManager> manager, td::Promise<td::Unit> promise) {
  td::actor::create_actor<AcceptBlockQuery>("fork/accept", AcceptBlockQuery::ForceFork(), id, std::move(data),
                                            std::move(manager), std::move(promise))
      .release();
}

void run_apply_block_query(BlockIdExt id, td::Ref<BlockData> block, BlockIdExt masterchain_block_id,
                           td::actor::ActorId<ValidatorManager> manager, td::Timestamp timeout,
                           td::Promise<td::Unit> promise) {
  td::actor::create_actor<ApplyBlock>(PSTRING() << "apply " << id, id, std::move(block), masterchain_block_id, manager,
                                      timeout, std::move(promise))
      .release();
}

void run_check_proof_query(BlockIdExt id, td::Ref<Proof> proof, td::actor::ActorId<ValidatorManager> manager,
                           td::Timestamp timeout, td::Promise<BlockHandle> promise, bool skip_check_signatures) {
  td::actor::create_actor<CheckProof>("checkproof", id, std::move(proof), manager, timeout, std::move(promise),
                                      skip_check_signatures)
      .release();
}

void run_check_proof_query(BlockIdExt id, td::Ref<Proof> proof, td::actor::ActorId<ValidatorManager> manager,
                           td::Timestamp timeout, td::Promise<BlockHandle> promise,
                           td::Ref<ProofLink> rel_key_block_proof, bool skip_check_signatures) {
  td::actor::create_actor<CheckProof>("checkproof/key", id, std::move(proof), manager, timeout, std::move(promise),
                                      skip_check_signatures, std::move(rel_key_block_proof))
      .release();
}

void run_check_proof_query(BlockIdExt id, td::Ref<Proof> proof, td::actor::ActorId<ValidatorManager> manager,
                           td::Timestamp timeout, td::Promise<BlockHandle> promise,
                           td::Ref<MasterchainState> rel_mc_state, bool skip_check_signatures) {
  td::actor::create_actor<CheckProof>("checkproof/st", id, std::move(proof), manager, timeout, std::move(promise),
                                      skip_check_signatures, std::move(rel_mc_state))
      .release();
}

void run_check_proof_link_query(BlockIdExt id, td::Ref<ProofLink> proof, td::actor::ActorId<ValidatorManager> manager,
                                td::Timestamp timeout, td::Promise<BlockHandle> promise) {
  td::actor::create_actor<CheckProof>("checkprooflink", id, std::move(proof), manager, timeout, std::move(promise))
      .release();
}

void run_validate_query(ShardIdFull shard, UnixTime min_ts, BlockIdExt min_masterchain_block_id,
                        std::vector<BlockIdExt> prev, BlockCandidate candidate, td::Ref<ValidatorSet> validator_set,
                        td::actor::ActorId<ValidatorManager> manager, td::Timestamp timeout,
                        td::Promise<ValidateCandidateResult> promise, bool is_fake) {
  BlockSeqno seqno = 0;
  for (auto& p : prev) {
    if (p.seqno() > seqno) {
      seqno = p.seqno();
    }
  }
  td::actor::create_actor<ValidateQuery>(
      PSTRING() << (is_fake ? "fakevalidate" : "validateblock") << shard.to_str() << ":" << (seqno + 1), shard, min_ts,
      min_masterchain_block_id, std::move(prev), std::move(candidate), std::move(validator_set), std::move(manager),
      timeout, std::move(promise), is_fake)
      .release();
}

void run_collate_query(ShardIdFull shard, td::uint32 min_ts, const BlockIdExt& min_masterchain_block_id,
                       std::vector<BlockIdExt> prev, Ed25519_PublicKey collator_id, td::Ref<ValidatorSet> validator_set,
                       td::actor::ActorId<ValidatorManager> manager, td::Timestamp timeout,
                       td::Promise<BlockCandidate> promise) {
  BlockSeqno seqno = 0;
  for (auto& p : prev) {
    if (p.seqno() > seqno) {
      seqno = p.seqno();
    }
  }
  td::actor::create_actor<Collator>(PSTRING() << "collate" << shard.to_str() << ":" << (seqno + 1), shard, false,
                                    min_ts, min_masterchain_block_id, std::move(prev), std::move(validator_set),
                                    collator_id, std::move(manager), timeout, std::move(promise))
      .release();
}

void run_collate_hardfork(ShardIdFull shard, const BlockIdExt& min_masterchain_block_id, std::vector<BlockIdExt> prev,
                          td::actor::ActorId<ValidatorManager> manager, td::Timestamp timeout,
                          td::Promise<BlockCandidate> promise) {
  BlockSeqno seqno = 0;
  for (auto& p : prev) {
    if (p.seqno() > seqno) {
      seqno = p.seqno();
    }
  }
  td::actor::create_actor<Collator>(PSTRING() << "collate" << shard.to_str() << ":" << (seqno + 1), shard, true, 0,
                                    min_masterchain_block_id, std::move(prev), td::Ref<ValidatorSet>{},
                                    Ed25519_PublicKey{Bits256::zero()}, std::move(manager), timeout, std::move(promise))
      .release();
}

void run_liteserver_query(td::BufferSlice data, td::actor::ActorId<ValidatorManager> manager,
                          td::actor::ActorId<LiteServerCache> cache, td::Promise<td::BufferSlice> promise) {
  LiteQuery::run_query(std::move(data), std::move(manager), std::move(promise));
}

void run_fetch_account_state(WorkchainId wc, StdSmcAddress  addr, td::actor::ActorId<ValidatorManager> manager,
                             td::Promise<std::tuple<td::Ref<vm::CellSlice>,UnixTime,LogicalTime,std::unique_ptr<block::ConfigInfo>>> promise) {
  LiteQuery::fetch_account_state(wc, addr, std::move(manager), std::move(promise));
}

void run_validate_shard_block_description(td::BufferSlice data, BlockHandle masterchain_block,
                                          td::Ref<MasterchainState> masterchain_state,
                                          td::actor::ActorId<ValidatorManager> manager, td::Timestamp timeout,
                                          td::Promise<td::Ref<ShardTopBlockDescription>> promise, bool is_fake) {
  auto id = masterchain_block->id();
  td::actor::create_actor<ValidateShardTopBlockDescr>("topshardfetch", std::move(data), id,
                                                      std::move(masterchain_block), std::move(masterchain_state),
                                                      manager, timeout, is_fake, std::move(promise))
      .release();
}

}  // namespace validator

}  // namespace ton
