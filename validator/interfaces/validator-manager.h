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

#include "shard.h"
#include "block.h"
#include "proof.h"
#include "external-message.h"
#include "ihr-message.h"
#include "shard-block.h"
#include "message-queue.h"
#include "validator/validator.h"
#include "liteserver.h"
#include "crypto/vm/db/DynamicBagOfCellsDb.h"
#include "validator-session/validator-session-types.h"
#include "auto/tl/lite_api.h"

namespace ton {

namespace validator {

constexpr int VERBOSITY_NAME(VALIDATOR_WARNING) = verbosity_WARNING;
constexpr int VERBOSITY_NAME(VALIDATOR_NOTICE) = verbosity_INFO;
constexpr int VERBOSITY_NAME(VALIDATOR_INFO) = verbosity_DEBUG;
constexpr int VERBOSITY_NAME(VALIDATOR_DEBUG) = verbosity_DEBUG;
constexpr int VERBOSITY_NAME(VALIDATOR_EXTRA_DEBUG) = verbosity_DEBUG + 1;

struct CandidateReject {
  std::string reason;
  td::BufferSlice proof;
};

struct AsyncSerializerState {
  BlockIdExt last_block_id;
  BlockIdExt last_written_block_id;
  UnixTime last_written_block_ts;
};

struct CollationStats {
  td::uint32 bytes, gas, lt_delta;
  int cat_bytes, cat_gas, cat_lt_delta;
  std::string limits_log;
  td::uint32 ext_msgs_total = 0;
  td::uint32 ext_msgs_filtered = 0;
  td::uint32 ext_msgs_accepted = 0;
  td::uint32 ext_msgs_rejected = 0;
};

using ValidateCandidateResult = td::Variant<UnixTime, CandidateReject>;

class ValidatorManager : public ValidatorManagerInterface {
 public:
  virtual void init_last_masterchain_state(td::Ref<MasterchainState> state) {
  }
  virtual void set_block_state(BlockHandle handle, td::Ref<ShardState> state,
                               td::Promise<td::Ref<ShardState>> promise) = 0;
  virtual void get_cell_db_reader(td::Promise<std::shared_ptr<vm::CellDbReader>> promise) = 0;
  virtual void store_persistent_state_file(BlockIdExt block_id, BlockIdExt masterchain_block_id, td::BufferSlice state,
                                           td::Promise<td::Unit> promise) = 0;
  virtual void store_persistent_state_file_gen(BlockIdExt block_id, BlockIdExt masterchain_block_id,
                                               std::function<td::Status(td::FileFd&)> write_data,
                                               td::Promise<td::Unit> promise) = 0;
  virtual void store_zero_state_file(BlockIdExt block_id, td::BufferSlice state, td::Promise<td::Unit> promise) = 0;

  virtual void set_block_data(BlockHandle handle, td::Ref<BlockData> data, td::Promise<td::Unit> promise) = 0;
  virtual void wait_block_data(BlockHandle handle, td::uint32 priority, td::Timestamp,
                               td::Promise<td::Ref<BlockData>> promise) = 0;
  virtual void wait_block_data_short(BlockIdExt block_id, td::uint32 priority, td::Timestamp,
                                     td::Promise<td::Ref<BlockData>> promise) = 0;

  virtual void set_block_proof(BlockHandle handle, td::Ref<Proof> proof, td::Promise<td::Unit> promise) = 0;
  virtual void wait_block_proof(BlockHandle handle, td::Timestamp timeout, td::Promise<td::Ref<Proof>> promise) = 0;
  virtual void wait_block_proof_short(BlockIdExt id, td::Timestamp timeout, td::Promise<td::Ref<Proof>> promise) = 0;

  virtual void set_block_proof_link(BlockHandle handle, td::Ref<ProofLink> proof, td::Promise<td::Unit> promise) = 0;
  virtual void wait_block_proof_link(BlockHandle handle, td::Timestamp timeout,
                                     td::Promise<td::Ref<ProofLink>> promise) = 0;
  virtual void wait_block_proof_link_short(BlockIdExt id, td::Timestamp timeout,
                                           td::Promise<td::Ref<ProofLink>> promise) = 0;

  virtual void set_block_signatures(BlockHandle handle, td::Ref<BlockSignatureSet> signatures,
                                    td::Promise<td::Unit> promise) = 0;
  virtual void wait_block_signatures(BlockHandle handle, td::Timestamp timeout,
                                     td::Promise<td::Ref<BlockSignatureSet>> promise) = 0;
  virtual void wait_block_signatures_short(BlockIdExt id, td::Timestamp timeout,
                                           td::Promise<td::Ref<BlockSignatureSet>> promise) = 0;

  virtual void set_block_candidate(BlockIdExt id, BlockCandidate candidate, CatchainSeqno cc_seqno,
                                   td::uint32 validator_set_hash, td::Promise<td::Unit> promise) = 0;
  virtual void send_block_candidate_broadcast(BlockIdExt id, CatchainSeqno cc_seqno, td::uint32 validator_set_hash,
                                              td::BufferSlice data) = 0;

  virtual void wait_block_state_merge(BlockIdExt left_id, BlockIdExt right_id, td::uint32 priority,
                                      td::Timestamp timeout, td::Promise<td::Ref<ShardState>> promise) = 0;
  virtual void wait_prev_block_state(BlockHandle handle, td::uint32 priority, td::Timestamp timeout,
                                     td::Promise<td::Ref<ShardState>> promise) = 0;

  virtual void wait_block_message_queue(BlockHandle handle, td::uint32 priority, td::Timestamp timeout,
                                        td::Promise<td::Ref<MessageQueue>> promise) = 0;
  virtual void wait_block_message_queue_short(BlockIdExt id, td::uint32 priority, td::Timestamp timeout,
                                              td::Promise<td::Ref<MessageQueue>> promise) = 0;
  virtual void get_external_messages(ShardIdFull shard,
                                     td::Promise<std::vector<std::pair<td::Ref<ExtMessage>, int>>> promise) = 0;
  virtual void get_ihr_messages(ShardIdFull shard, td::Promise<std::vector<td::Ref<IhrMessage>>> promise) = 0;
  virtual void get_shard_blocks(BlockIdExt masterchain_block_id,
                                td::Promise<std::vector<td::Ref<ShardTopBlockDescription>>> promise) = 0;
  virtual void complete_external_messages(std::vector<ExtMessage::Hash> to_delay,
                                          std::vector<ExtMessage::Hash> to_delete) = 0;
  virtual void complete_ihr_messages(std::vector<IhrMessage::Hash> to_delay,
                                     std::vector<IhrMessage::Hash> to_delete) = 0;

  //virtual void set_first_block(ZeroStateIdExt state, BlockIdExt block, td::Promise<td::Unit> promise) = 0;
  virtual void set_next_block(BlockIdExt prev, BlockIdExt next, td::Promise<td::Unit> promise) = 0;

  virtual void new_block(BlockHandle handle, td::Ref<ShardState> state, td::Promise<td::Unit> promise) = 0;

  virtual void send_get_block_request(BlockIdExt id, td::uint32 priority, td::Promise<ReceivedBlock> promise) = 0;
  virtual void send_get_zero_state_request(BlockIdExt id, td::uint32 priority,
                                           td::Promise<td::BufferSlice> promise) = 0;
  virtual void send_get_persistent_state_request(BlockIdExt id, BlockIdExt masterchain_block_id, td::uint32 priority,
                                                 td::Promise<td::BufferSlice> promise) = 0;
  virtual void send_get_block_proof_request(BlockIdExt block_id, td::uint32 priority,
                                            td::Promise<td::BufferSlice> promise) = 0;
  virtual void send_get_block_proof_link_request(BlockIdExt block_id, td::uint32 priority,
                                                 td::Promise<td::BufferSlice> promise) = 0;
  virtual void send_get_next_key_blocks_request(BlockIdExt block_id, td::uint32 priority,
                                                td::Promise<std::vector<BlockIdExt>> promise) = 0;
  virtual void send_external_message(td::Ref<ExtMessage> message) = 0;
  virtual void send_ihr_message(td::Ref<IhrMessage> message) = 0;
  virtual void send_top_shard_block_description(td::Ref<ShardTopBlockDescription> desc) = 0;
  virtual void send_block_broadcast(BlockBroadcast broadcast, int mode) = 0;
  virtual void send_validator_telemetry(PublicKeyHash key, tl_object_ptr<ton_api::validator_telemetry> telemetry) = 0;
  virtual void send_get_out_msg_queue_proof_request(ShardIdFull dst_shard, std::vector<BlockIdExt> blocks,
                                                    block::ImportedMsgQueueLimits limits,
                                                    td::Promise<std::vector<td::Ref<OutMsgQueueProof>>> promise) = 0;
  virtual void send_download_archive_request(BlockSeqno mc_seqno, ShardIdFull shard_prefix, std::string tmp_dir,
                                             td::Timestamp timeout, td::Promise<std::string> promise) = 0;

  virtual void update_shard_client_state(BlockIdExt masterchain_block_id, td::Promise<td::Unit> promise) = 0;
  virtual void get_shard_client_state(bool from_db, td::Promise<BlockIdExt> promise) = 0;

  virtual void update_async_serializer_state(AsyncSerializerState state, td::Promise<td::Unit> promise) = 0;
  virtual void get_async_serializer_state(td::Promise<AsyncSerializerState> promise) = 0;

  virtual void try_get_static_file(FileHash file_hash, td::Promise<td::BufferSlice> promise) = 0;

  virtual void allow_block_state_gc(BlockIdExt block_id, td::Promise<bool> promise) = 0;

  virtual void archive(BlockHandle handle, td::Promise<td::Unit> promise) = 0;

  virtual void check_is_hardfork(BlockIdExt block_id, td::Promise<bool> promise) = 0;
  virtual void get_vertical_seqno(BlockSeqno seqno, td::Promise<td::uint32> promise) = 0;

  virtual void update_last_known_key_block(BlockHandle handle, bool send_request) = 0;
  virtual void update_gc_block_handle(BlockHandle handle, td::Promise<td::Unit> promise) = 0;

  virtual void update_shard_client_block_handle(BlockHandle handle, td::Ref<MasterchainState> state,
                                                td::Promise<td::Unit> promise) = 0;

  virtual void truncate(BlockSeqno seqno, ConstBlockHandle handle, td::Promise<td::Unit> promise) = 0;

  virtual void wait_shard_client_state(BlockSeqno seqno, td::Timestamp timeout, td::Promise<td::Unit> promise) = 0;

  virtual void log_validator_session_stats(BlockIdExt block_id, validatorsession::ValidatorSessionStats stats) = 0;
  virtual void log_new_validator_group_stats(validatorsession::NewValidatorGroupStats stats) = 0;
  virtual void log_end_validator_group_stats(validatorsession::EndValidatorGroupStats stats) = 0;

  virtual void get_block_handle_for_litequery(BlockIdExt block_id, td::Promise<ConstBlockHandle> promise) = 0;
  virtual void get_block_data_for_litequery(BlockIdExt block_id, td::Promise<td::Ref<BlockData>> promise) = 0;
  virtual void get_block_state_for_litequery(BlockIdExt block_id, td::Promise<td::Ref<ShardState>> promise) = 0;
  virtual void get_block_by_lt_for_litequery(AccountIdPrefixFull account, LogicalTime lt,
                                             td::Promise<ConstBlockHandle> promise) = 0;
  virtual void get_block_by_unix_time_for_litequery(AccountIdPrefixFull account, UnixTime ts,
                                                    td::Promise<ConstBlockHandle> promise) = 0;
  virtual void get_block_by_seqno_for_litequery(AccountIdPrefixFull account, BlockSeqno seqno,
                                                td::Promise<ConstBlockHandle> promise) = 0;
  virtual void get_block_candidate_for_litequery(PublicKey source, BlockIdExt block_id, FileHash collated_data_hash,
                                                 td::Promise<BlockCandidate> promise) = 0;
  virtual void get_validator_groups_info_for_litequery(
      td::optional<ShardIdFull> shard,
      td::Promise<tl_object_ptr<lite_api::liteServer_nonfinal_validatorGroups>> promise) = 0;

  virtual void add_lite_query_stats(int lite_query_id, bool success) {
  }

  virtual void record_collate_query_stats(BlockIdExt block_id, double work_time, double cpu_work_time,
                                          td::optional<CollationStats> stats) {
  }
  virtual void record_validate_query_stats(BlockIdExt block_id, double work_time, double cpu_work_time, bool success) {
  }

  virtual void add_persistent_state_description(td::Ref<PersistentStateDescription> desc) = 0;

  static bool is_persistent_state(UnixTime ts, UnixTime prev_ts) {
    return ts / (1 << 17) != prev_ts / (1 << 17);
  }
  static UnixTime persistent_state_ttl(UnixTime ts) {
    auto x = ts / (1 << 17);
    CHECK(x > 0);
    auto b = td::count_trailing_zeroes32(x);
    return ts + ((1 << 18) << b);
  }
};

}  // namespace validator

}  // namespace ton
