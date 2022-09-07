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

#include "ton/ton-types.h"
#include "validator/interfaces/block-handle.h"
#include "validator/interfaces/validator-manager.h"

namespace ton {

namespace validator {

class Db : public td::actor::Actor {
 public:
  virtual ~Db() = default;

  virtual void store_block_data(BlockHandle handle, td::Ref<BlockData> data, td::Promise<td::Unit> promise) = 0;
  virtual void get_block_data(ConstBlockHandle handle, td::Promise<td::Ref<BlockData>> promise) = 0;

  virtual void store_block_signatures(BlockHandle handle, td::Ref<BlockSignatureSet> data,
                                      td::Promise<td::Unit> promise) = 0;
  virtual void get_block_signatures(ConstBlockHandle handle, td::Promise<td::Ref<BlockSignatureSet>> promise) = 0;

  virtual void store_block_proof(BlockHandle handle, td::Ref<Proof> proof, td::Promise<td::Unit> promise) = 0;
  virtual void get_block_proof(ConstBlockHandle handle, td::Promise<td::Ref<Proof>> promise) = 0;

  virtual void store_block_proof_link(BlockHandle handle, td::Ref<ProofLink> proof, td::Promise<td::Unit> promise) = 0;
  virtual void get_block_proof_link(ConstBlockHandle handle, td::Promise<td::Ref<ProofLink>> promise) = 0;

  virtual void store_block_candidate(BlockCandidate candidate, td::Promise<td::Unit> promise) = 0;
  virtual void get_block_candidate(ton::PublicKey source, BlockIdExt id, FileHash collated_data_file_hash,
                                   td::Promise<BlockCandidate> promise) = 0;

  virtual void store_block_state(BlockHandle handle, td::Ref<ShardState> state,
                                 td::Promise<td::Ref<ShardState>> promise) = 0;
  virtual void get_block_state(ConstBlockHandle handle, td::Promise<td::Ref<ShardState>> promise) = 0;
  virtual void get_cell_db_reader(td::Promise<std::shared_ptr<vm::CellDbReader>> promise) = 0;

  virtual void store_persistent_state_file(BlockIdExt block_id, BlockIdExt masterchain_block_id, td::BufferSlice state,
                                           td::Promise<td::Unit> promise) = 0;
  virtual void store_persistent_state_file_gen(BlockIdExt block_id, BlockIdExt masterchain_block_id,
                                               std::function<td::Status(td::FileFd&)> write_data,
                                               td::Promise<td::Unit> promise) = 0;
  virtual void get_persistent_state_file(BlockIdExt block_id, BlockIdExt masterchain_block_id,
                                         td::Promise<td::BufferSlice> promise) = 0;
  virtual void get_persistent_state_file_slice(BlockIdExt block_id, BlockIdExt masterchain_block_id, td::int64 offset,
                                               td::int64 max_length, td::Promise<td::BufferSlice> promise) = 0;
  virtual void check_persistent_state_file_exists(BlockIdExt block_id, BlockIdExt masterchain_block_id,
                                                  td::Promise<bool> promise) = 0;
  virtual void store_zero_state_file(BlockIdExt block_id, td::BufferSlice state, td::Promise<td::Unit> promise) = 0;
  virtual void get_zero_state_file(BlockIdExt block_id, td::Promise<td::BufferSlice> promise) = 0;
  virtual void check_zero_state_file_exists(BlockIdExt block_id, td::Promise<bool> promise) = 0;

  virtual void try_get_static_file(FileHash file_hash, td::Promise<td::BufferSlice> promise) = 0;

  //virtual void store_zero_state(ZeroStateIdExt id, td::Ref<ShardState> state, td::Promise<td::Unit> promise) = 0;
  //virtual void get_zero_state(ZeroStateIdExt id, td::Promise<td::Ref<ShardState>> promise) = 0;

  virtual void store_block_handle(BlockHandle handle, td::Promise<td::Unit> promise) = 0;
  virtual void get_block_handle(BlockIdExt id, td::Promise<BlockHandle> promise) = 0;

  virtual void apply_block(BlockHandle handle, td::Promise<td::Unit> promise) = 0;
  virtual void get_block_by_lt(AccountIdPrefixFull account, LogicalTime lt, td::Promise<ConstBlockHandle> promise) = 0;
  virtual void get_block_by_unix_time(AccountIdPrefixFull account, UnixTime ts,
                                      td::Promise<ConstBlockHandle> promise) = 0;
  virtual void get_block_by_seqno(AccountIdPrefixFull account, BlockSeqno seqno,
                                  td::Promise<ConstBlockHandle> promise) = 0;

  virtual void update_init_masterchain_block(BlockIdExt block, td::Promise<td::Unit> promise) = 0;
  virtual void get_init_masterchain_block(td::Promise<BlockIdExt> promise) = 0;

  virtual void update_gc_masterchain_block(BlockIdExt block, td::Promise<td::Unit> promise) = 0;
  virtual void get_gc_masterchain_block(td::Promise<BlockIdExt> promise) = 0;

  virtual void update_shard_client_state(BlockIdExt masterchain_block_id, td::Promise<td::Unit> promise) = 0;
  virtual void get_shard_client_state(td::Promise<BlockIdExt> promise) = 0;

  virtual void update_destroyed_validator_sessions(std::vector<ValidatorSessionId> sessions,
                                                   td::Promise<td::Unit> promise) = 0;
  virtual void get_destroyed_validator_sessions(td::Promise<std::vector<ValidatorSessionId>> promise) = 0;

  virtual void update_async_serializer_state(AsyncSerializerState state, td::Promise<td::Unit> promise) = 0;
  virtual void get_async_serializer_state(td::Promise<AsyncSerializerState> promise) = 0;

  virtual void update_hardforks(std::vector<BlockIdExt> blocks, td::Promise<td::Unit> promise) = 0;
  virtual void get_hardforks(td::Promise<std::vector<BlockIdExt>> promise) = 0;

  virtual void archive(BlockHandle handle, td::Promise<td::Unit> promise) = 0;

  virtual void prepare_stats(td::Promise<std::vector<std::pair<std::string, std::string>>> promise) = 0;

  virtual void truncate(BlockSeqno seqno, ConstBlockHandle handle, td::Promise<td::Unit> promise) = 0;

  virtual void add_key_block_proof(td::Ref<Proof> proof, td::Promise<td::Unit> promise) = 0;
  virtual void add_key_block_proof_link(td::Ref<ProofLink> proof_link, td::Promise<td::Unit> promise) = 0;
  virtual void get_key_block_proof(BlockIdExt block_id, td::Promise<td::Ref<Proof>> promise) = 0;
  virtual void get_key_block_proof_link(BlockIdExt block_id, td::Promise<td::Ref<ProofLink>> promise) = 0;
  virtual void check_key_block_proof_exists(BlockIdExt block_id, td::Promise<bool> promise) = 0;
  virtual void check_key_block_proof_link_exists(BlockIdExt block_id, td::Promise<bool> promise) = 0;

  virtual void get_archive_id(BlockSeqno masterchain_seqno, td::Promise<td::uint64> promise) = 0;
  virtual void get_archive_slice(td::uint64 archive_id, td::uint64 offset, td::uint32 limit,
                                 td::Promise<td::BufferSlice> promise) = 0;
  virtual void set_async_mode(bool mode, td::Promise<td::Unit> promise) = 0;

  virtual void run_gc(UnixTime ts, UnixTime archive_ttl) = 0;
};

}  // namespace validator

}  // namespace ton
