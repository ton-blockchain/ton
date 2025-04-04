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
#include "validator-group.hpp"
#include "manager-init.h"
#include "manager-disk.h"
#include "queue-size-counter.hpp"

#include <map>
#include <set>

namespace ton {

namespace validator {

class WaitBlockState;
class WaitZeroState;
class WaitShardState;
class WaitBlockDataDisk;

class ValidatorManagerImpl : public ValidatorManager {
 private:
  std::vector<td::Ref<ExtMessage>> ext_messages_;
  std::vector<td::Ref<IhrMessage>> ihr_messages_;
  struct Compare {
    bool operator()(const td::Ref<ShardTopBlockDescription> &l, const td::Ref<ShardTopBlockDescription> &r) const {
      return l->block_id() < r->block_id();
    }
  };
  std::set<td::Ref<ShardTopBlockDescription>, Compare> shard_blocks_, out_shard_blocks_;
  std::vector<td::BufferSlice> shard_blocks_raw_;

  struct WaitBlockStateList {
    std::vector<std::pair<td::Timestamp, td::Promise<td::Ref<ShardState>>>> waiting_;
    td::actor::ActorId<WaitBlockState> actor_;
  };
  std::map<BlockIdExt, WaitBlockStateList> wait_state_;
  struct WaitBlockDataList {
    std::vector<std::pair<td::Timestamp, td::Promise<td::Ref<BlockData>>>> waiting_;
    td::actor::ActorId<WaitBlockDataDisk> actor_;
  };
  std::map<BlockIdExt, WaitBlockDataList> wait_block_data_;

  std::map<BlockIdExt, std::weak_ptr<BlockHandleInterface>> handles_;

  std::unique_ptr<Callback> callback_;
  td::actor::ActorOwn<Db> db_;
  BlockSeqno last_masterchain_seqno_ = 0;
  bool started_ = false;
  td::Ref<MasterchainState> last_masterchain_state_;
  //BlockHandle last_masterchain_block_;

 public:
  void install_callback(std::unique_ptr<Callback> new_callback, td::Promise<td::Unit> promise) override {
    callback_ = std::move(new_callback);
    promise.set_value(td::Unit());
  }
  void add_permanent_key(PublicKeyHash key, td::Promise<td::Unit> promise) override {
    UNREACHABLE();
  }
  void add_temp_key(PublicKeyHash key, td::Promise<td::Unit> promise) override {
    UNREACHABLE();
  }
  void del_permanent_key(PublicKeyHash key, td::Promise<td::Unit> promise) override {
    UNREACHABLE();
  }
  void del_temp_key(PublicKeyHash key, td::Promise<td::Unit> promise) override {
    UNREACHABLE();
  }

  void validate_block_is_next_proof(BlockIdExt prev_block_id, BlockIdExt next_block_id, td::BufferSlice proof,
                                    td::Promise<td::Unit> promise) override;
  void validate_block_proof(BlockIdExt block_id, td::BufferSlice proof, td::Promise<td::Unit> promise) override;
  void validate_block_proof_link(BlockIdExt block_id, td::BufferSlice proof, td::Promise<td::Unit> promise) override;
  void validate_block_proof_rel(BlockIdExt block_id, BlockIdExt rel_block_id, td::BufferSlice proof,
                                td::Promise<td::Unit> promise) override {
    UNREACHABLE();
  }
  void validate_block(ReceivedBlock block, td::Promise<BlockHandle> promise) override;
  void prevalidate_block(BlockBroadcast broadcast, td::Promise<td::Unit> promise) override;

  //void create_validate_block(BlockId block, td::BufferSlice data, td::Promise<Block> promise) = 0;
  void sync_complete(td::Promise<td::Unit> promise) override;

  void get_next_block(BlockIdExt block_id, td::Promise<BlockHandle> promise) override;
  void get_next_key_blocks(BlockIdExt block_id, td::uint32 cnt, td::Promise<std::vector<BlockIdExt>> promise) override {
    UNREACHABLE();
  }
  void get_block_data(BlockHandle handle, td::Promise<td::BufferSlice> promise) override;
  void check_zero_state_exists(BlockIdExt block_id, td::Promise<bool> promise) override;
  void get_zero_state(BlockIdExt block_id, td::Promise<td::BufferSlice> promise) override;
  void get_persistent_state_size(BlockIdExt block_id, BlockIdExt masterchain_block_id,
                                 td::Promise<td::uint64> promise) override;
  void get_persistent_state(BlockIdExt block_id, BlockIdExt masterchain_block_id,
                            td::Promise<td::BufferSlice> promise) override;
  void get_persistent_state_slice(BlockIdExt block_id, BlockIdExt masterchain_block_id, td::int64 offset,
                                  td::int64 max_length, td::Promise<td::BufferSlice> promise) override {
    UNREACHABLE();
  }
  void get_previous_persistent_state_files(
      BlockSeqno cur_mc_seqno, td::Promise<std::vector<std::pair<std::string, ShardIdFull>>> promise) override {
    UNREACHABLE();
  }
  void get_block_proof(BlockHandle handle, td::Promise<td::BufferSlice> promise) override;
  void get_block_proof_link(BlockHandle block_id, td::Promise<td::BufferSlice> promise) override {
    UNREACHABLE();
  }
  void get_key_block_proof(BlockIdExt block_id, td::Promise<td::BufferSlice> promise) override;
  void get_key_block_proof_link(BlockIdExt block_id, td::Promise<td::BufferSlice> promise) override;
  //void get_block_description(BlockIdExt block_id, td::Promise<BlockDescription> promise) override;

  void new_external_message(td::BufferSlice data, int priority) override;
  void check_external_message(td::BufferSlice data, td::Promise<td::Ref<ExtMessage>> promise) override {
    UNREACHABLE();
  }
  void new_ihr_message(td::BufferSlice data) override;
  void new_shard_block(BlockIdExt block_id, CatchainSeqno cc_seqno, td::BufferSlice data) override;
  void new_block_candidate(BlockIdExt block_id, td::BufferSlice data) override {
  }

  void add_ext_server_id(adnl::AdnlNodeIdShort id) override {
    UNREACHABLE();
  }
  void add_ext_server_port(td::uint16 port) override {
    UNREACHABLE();
  }

  void get_block_handle(BlockIdExt id, bool force, td::Promise<BlockHandle> promise) override;

  void set_block_state(BlockHandle handle, td::Ref<ShardState> state,
                       td::Promise<td::Ref<ShardState>> promise) override;
  void get_cell_db_reader(td::Promise<std::shared_ptr<vm::CellDbReader>> promise) override;
  void store_persistent_state_file(BlockIdExt block_id, BlockIdExt masterchain_block_id, td::BufferSlice state,
                                   td::Promise<td::Unit> promise) override;
  void store_persistent_state_file_gen(BlockIdExt block_id, BlockIdExt masterchain_block_id,
                                       std::function<td::Status(td::FileFd&)> write_data,
                                       td::Promise<td::Unit> promise) override;
  void store_zero_state_file(BlockIdExt block_id, td::BufferSlice state, td::Promise<td::Unit> promise) override;
  void wait_block_state(BlockHandle handle, td::uint32 priority, td::Timestamp timeout,
                        td::Promise<td::Ref<ShardState>> promise) override;
  void wait_block_state_short(BlockIdExt block_id, td::uint32 priority, td::Timestamp timeout,
                              td::Promise<td::Ref<ShardState>> promise) override;

  void set_block_data(BlockHandle handle, td::Ref<BlockData> data, td::Promise<td::Unit> promise) override;
  void wait_block_data(BlockHandle handle, td::uint32 priority, td::Timestamp,
                       td::Promise<td::Ref<BlockData>> promise) override;
  void wait_block_data_short(BlockIdExt block_id, td::uint32 priority, td::Timestamp,
                             td::Promise<td::Ref<BlockData>> promise) override;

  void set_block_proof(BlockHandle handle, td::Ref<Proof> proof, td::Promise<td::Unit> promise) override;
  void wait_block_proof(BlockHandle handle, td::Timestamp timeout, td::Promise<td::Ref<Proof>> promise) override;
  void wait_block_proof_short(BlockIdExt id, td::Timestamp timeout, td::Promise<td::Ref<Proof>> promise) override;

  void set_block_proof_link(BlockHandle handle, td::Ref<ProofLink> proof, td::Promise<td::Unit> promise) override;
  void wait_block_proof_link(BlockHandle handle, td::Timestamp timeout,
                             td::Promise<td::Ref<ProofLink>> promise) override;
  void wait_block_proof_link_short(BlockIdExt id, td::Timestamp timeout,
                                   td::Promise<td::Ref<ProofLink>> promise) override;

  void set_block_signatures(BlockHandle handle, td::Ref<BlockSignatureSet> signatures,
                            td::Promise<td::Unit> promise) override;
  void wait_block_signatures(BlockHandle handle, td::Timestamp timeout,
                             td::Promise<td::Ref<BlockSignatureSet>> promise) override;
  void wait_block_signatures_short(BlockIdExt id, td::Timestamp timeout,
                                   td::Promise<td::Ref<BlockSignatureSet>> promise) override;

  void set_block_candidate(BlockIdExt id, BlockCandidate candidate, CatchainSeqno cc_seqno,
                           td::uint32 validator_set_hash, td::Promise<td::Unit> promise) override;
  void send_block_candidate_broadcast(BlockIdExt id, CatchainSeqno cc_seqno, td::uint32 validator_set_hash,
                                      td::BufferSlice data) override;

  void wait_block_state_merge(BlockIdExt left_id, BlockIdExt right_id, td::uint32 priority, td::Timestamp timeout,
                              td::Promise<td::Ref<ShardState>> promise) override;
  void wait_prev_block_state(BlockHandle handle, td::uint32 priority, td::Timestamp timeout,
                             td::Promise<td::Ref<ShardState>> promise) override;

  void wait_block_message_queue(BlockHandle handle, td::uint32 priority, td::Timestamp timeout,
                                td::Promise<td::Ref<MessageQueue>> promise) override;
  void wait_block_message_queue_short(BlockIdExt id, td::uint32 priority, td::Timestamp timeout,
                                      td::Promise<td::Ref<MessageQueue>> promise) override;
  void get_external_messages(ShardIdFull shard,
                             td::Promise<std::vector<std::pair<td::Ref<ExtMessage>, int>>> promise) override;
  void get_ihr_messages(ShardIdFull shard, td::Promise<std::vector<td::Ref<IhrMessage>>> promise) override;
  void get_shard_blocks(BlockIdExt masterchain_block_id,
                        td::Promise<std::vector<td::Ref<ShardTopBlockDescription>>> promise) override;
  void complete_external_messages(std::vector<ExtMessage::Hash> to_delay,
                                  std::vector<ExtMessage::Hash> to_delete) override;
  void complete_ihr_messages(std::vector<IhrMessage::Hash> to_delay, std::vector<IhrMessage::Hash> to_delete) override;

  //void set_first_block(ZeroStateIdExt state, BlockIdExt block, td::Promise<td::Unit> promise) override;
  void set_next_block(BlockIdExt prev, BlockIdExt next, td::Promise<td::Unit> promise) override;

  void get_block_data_from_db(ConstBlockHandle handle, td::Promise<td::Ref<BlockData>> promise) override;
  void get_block_data_from_db_short(BlockIdExt block_id, td::Promise<td::Ref<BlockData>> promise) override;
  void get_shard_state_from_db(ConstBlockHandle handle, td::Promise<td::Ref<ShardState>> promise) override;
  void get_shard_state_from_db_short(BlockIdExt block_id, td::Promise<td::Ref<ShardState>> promise) override;
  void get_block_candidate_from_db(PublicKey source, BlockIdExt id, FileHash collated_data_file_hash,
                                   td::Promise<BlockCandidate> promise) override;
  void get_candidate_data_by_block_id_from_db(BlockIdExt id, td::Promise<td::BufferSlice> promise) override;
  void get_block_proof_from_db(ConstBlockHandle handle, td::Promise<td::Ref<Proof>> promise) override;
  void get_block_proof_from_db_short(BlockIdExt id, td::Promise<td::Ref<Proof>> promise) override;
  void get_block_proof_link_from_db(ConstBlockHandle handle, td::Promise<td::Ref<ProofLink>> promise) override;
  void get_block_proof_link_from_db_short(BlockIdExt id, td::Promise<td::Ref<ProofLink>> promise) override;

  void get_block_by_lt_from_db(AccountIdPrefixFull account, LogicalTime lt,
                               td::Promise<ConstBlockHandle> promise) override;
  void get_block_by_unix_time_from_db(AccountIdPrefixFull account, UnixTime ts,
                                      td::Promise<ConstBlockHandle> promise) override;
  void get_block_by_seqno_from_db(AccountIdPrefixFull account, BlockSeqno seqno,
                                  td::Promise<ConstBlockHandle> promise) override;

  // get block handle declared in parent class
  void write_handle(BlockHandle handle, td::Promise<td::Unit> promise) override;

  void new_block(BlockHandle handle, td::Ref<ShardState> state, td::Promise<td::Unit> promise) override;
  void new_block_cont(BlockHandle handle, td::Ref<ShardState> state, td::Promise<td::Unit> promise);
  void get_top_masterchain_state(td::Promise<td::Ref<MasterchainState>> promise) override;
  void get_top_masterchain_block(td::Promise<BlockIdExt> promise) override;
  void get_top_masterchain_state_block(td::Promise<std::pair<td::Ref<MasterchainState>, BlockIdExt>> promise) override;
  void get_last_liteserver_state_block(td::Promise<std::pair<td::Ref<MasterchainState>, BlockIdExt>> promise) override;

  void send_get_block_request(BlockIdExt id, td::uint32 priority, td::Promise<ReceivedBlock> promise) override;
  void send_get_zero_state_request(BlockIdExt id, td::uint32 priority, td::Promise<td::BufferSlice> promise) override;
  void send_get_persistent_state_request(BlockIdExt id, BlockIdExt masterchain_block_id, td::uint32 priority,
                                         td::Promise<td::BufferSlice> promise) override;
  void send_get_block_proof_request(BlockIdExt block_id, td::uint32 priority,
                                    td::Promise<td::BufferSlice> promise) override {
    UNREACHABLE();
  }
  void send_get_block_proof_link_request(BlockIdExt block_id, td::uint32 priority,
                                         td::Promise<td::BufferSlice> promise) override {
    UNREACHABLE();
  }
  void send_get_next_key_blocks_request(BlockIdExt block_id, td::uint32 priority,
                                        td::Promise<std::vector<BlockIdExt>> promise) override {
    UNREACHABLE();
  }
  void send_external_message(td::Ref<ExtMessage> message) override {
    new_external_message(message->serialize(), 0);
  }
  void send_ihr_message(td::Ref<IhrMessage> message) override {
    new_ihr_message(message->serialize());
  }
  void send_top_shard_block_description(td::Ref<ShardTopBlockDescription> desc) override;
  void send_block_broadcast(BlockBroadcast broadcast, int mode) override {
  }
  void send_validator_telemetry(PublicKeyHash key, tl_object_ptr<ton_api::validator_telemetry> telemetry) override {
  }
  void send_get_out_msg_queue_proof_request(ShardIdFull dst_shard, std::vector<BlockIdExt> blocks,
                                            block::ImportedMsgQueueLimits limits,
                                            td::Promise<std::vector<td::Ref<OutMsgQueueProof>>> promise) override {
    UNREACHABLE();
  }
  void send_download_archive_request(BlockSeqno mc_seqno, ShardIdFull shard_prefix, std::string tmp_dir,
                                     td::Timestamp timeout, td::Promise<std::string> promise) override {
    UNREACHABLE();
  }

  void update_shard_client_state(BlockIdExt masterchain_block_id, td::Promise<td::Unit> promise) override;
  void get_shard_client_state(bool from_db, td::Promise<BlockIdExt> promise) override;

  void update_async_serializer_state(AsyncSerializerState state, td::Promise<td::Unit> promise) override {
    UNREACHABLE();
  }
  void get_async_serializer_state(td::Promise<AsyncSerializerState> promise) override {
    UNREACHABLE();
  }

  void try_get_static_file(FileHash file_hash, td::Promise<td::BufferSlice> promise) override;

  void get_download_token(size_t download_size, td::uint32 priority, td::Timestamp timeout,
                          td::Promise<std::unique_ptr<ActionToken>> promise) override {
    promise.set_error(td::Status::Error(ErrorCode::error, "download disabled"));
  }

  void get_archive_id(BlockSeqno masterchain_seqno, ShardIdFull shard_prefix,
                      td::Promise<td::uint64> promise) override {
    UNREACHABLE();
  }
  void get_archive_slice(td::uint64 archive_id, td::uint64 offset, td::uint32 limit,
                         td::Promise<td::BufferSlice> promise) override {
    UNREACHABLE();
  }

  void add_shard_block_description(td::Ref<ShardTopBlockDescription> desc);

  void register_block_handle(BlockHandle handle, td::Promise<BlockHandle> promise);

  void finished_wait_state(BlockIdExt id, td::Result<td::Ref<ShardState>> R);
  void finished_wait_data(BlockIdExt id, td::Result<td::Ref<BlockData>> R);

  void start_up() override;
  void started(ValidatorManagerInitResult result);

  void write_fake(BlockCandidate candidate, std::vector<BlockIdExt> prev, BlockIdExt last,
                  td::Ref<ValidatorSet> val_set);
  void validate_fake(BlockCandidate candidate, std::vector<BlockIdExt> prev, BlockIdExt last,
                     td::Ref<ValidatorSet> val_set);
  void complete_fake(BlockIdExt candidate_id);

  void check_is_hardfork(BlockIdExt block_id, td::Promise<bool> promise) override {
    CHECK(block_id.is_masterchain());
    promise.set_result(opts_->is_hardfork(block_id));
  }
  void get_vertical_seqno(BlockSeqno seqno, td::Promise<td::uint32> promise) override {
    promise.set_result(opts_->get_vertical_seqno(seqno));
  }
  void run_ext_query(td::BufferSlice data, td::Promise<td::BufferSlice> promise) override {
    UNREACHABLE();
  }

  ValidatorManagerImpl(PublicKeyHash local_id, td::Ref<ValidatorManagerOptions> opts, ShardIdFull shard_id,
                       BlockIdExt shard_to_block_id, std::string db_root)
      : local_id_(local_id)
      , opts_(std::move(opts))
      , db_root_(db_root)
      , shard_to_generate_(shard_id)
      , block_to_generate_(shard_to_block_id) {
  }

 public:
  void update_gc_block_handle(BlockHandle handle, td::Promise<td::Unit> promise) override {
    promise.set_value(td::Unit());
  }
  void allow_block_state_gc(BlockIdExt block_id, td::Promise<bool> promise) override {
    promise.set_result(false);
  }
  void archive(BlockHandle handle, td::Promise<td::Unit> promise) override {
    td::actor::send_closure(db_, &Db::archive, std::move(handle), std::move(promise));
  }
  void update_last_known_key_block(BlockHandle handle, bool send_request) override {
  }
  void update_shard_client_block_handle(BlockHandle handle, td::Ref<MasterchainState> state,
                                        td::Promise<td::Unit> promise) override {
  }

  void prepare_stats(td::Promise<std::vector<std::pair<std::string, std::string>>> promise) override {
    UNREACHABLE();
  }

  void prepare_actor_stats(td::Promise<std::string> promise) override {
    UNREACHABLE();
  }

  void prepare_perf_timer_stats(td::Promise<std::vector<PerfTimerStats>> promise) override {
    UNREACHABLE();
  }

  void add_perf_timer_stat(std::string name, double duration) override {
  }

  void truncate(BlockSeqno seqno, ConstBlockHandle handle, td::Promise<td::Unit> promise) override {
    UNREACHABLE();
  }
  void wait_shard_client_state(BlockSeqno seqno, td::Timestamp timeout, td::Promise<td::Unit> promise) override {
    UNREACHABLE();
  }
  void log_validator_session_stats(BlockIdExt block_id, validatorsession::ValidatorSessionStats stats) override {
    UNREACHABLE();
  }
  void log_new_validator_group_stats(validatorsession::NewValidatorGroupStats stats) override {
    UNREACHABLE();
  }
  void log_end_validator_group_stats(validatorsession::EndValidatorGroupStats stats) override {
    UNREACHABLE();
  }
  void get_out_msg_queue_size(BlockIdExt block_id, td::Promise<td::uint64> promise) override {
    if (queue_size_counter_.empty()) {
      queue_size_counter_ = td::actor::create_actor<QueueSizeCounter>("queuesizecounter", td::Ref<MasterchainState>{},
                                                                      opts_, actor_id(this));
    }
    td::actor::send_closure(queue_size_counter_, &QueueSizeCounter::get_queue_size, block_id, std::move(promise));
  }
  void get_block_handle_for_litequery(BlockIdExt block_id, td::Promise<ConstBlockHandle> promise) override {
    get_block_handle(block_id, false, promise.wrap([](BlockHandle &&handle) -> ConstBlockHandle { return handle; }));
  }
  void get_block_data_for_litequery(BlockIdExt block_id, td::Promise<td::Ref<BlockData>> promise) override {
    get_block_data_from_db_short(block_id, std::move(promise));
  }
  void get_block_state_for_litequery(BlockIdExt block_id, td::Promise<td::Ref<ShardState>> promise) override {
    get_shard_state_from_db_short(block_id, std::move(promise));
  }
  void get_block_by_lt_for_litequery(AccountIdPrefixFull account, LogicalTime lt,
                                             td::Promise<ConstBlockHandle> promise) override {
    get_block_by_lt_from_db(account, lt, std::move(promise));
  }
  void get_block_by_unix_time_for_litequery(AccountIdPrefixFull account, UnixTime ts,
                                                    td::Promise<ConstBlockHandle> promise) override {
    get_block_by_unix_time_from_db(account, ts, std::move(promise));
  }
  void get_block_by_seqno_for_litequery(AccountIdPrefixFull account, BlockSeqno seqno,
                                                td::Promise<ConstBlockHandle> promise) override {
    get_block_by_seqno_from_db(account, seqno, std::move(promise));
  }
  void get_block_candidate_for_litequery(PublicKey source, BlockIdExt block_id, FileHash collated_data_hash,
                                         td::Promise<BlockCandidate> promise) override {
    promise.set_result(td::Status::Error("not implemented"));
  }
  void get_validator_groups_info_for_litequery(
      td::optional<ShardIdFull> shard,
      td::Promise<tl_object_ptr<lite_api::liteServer_nonfinal_validatorGroups>> promise) override {
    promise.set_result(td::Status::Error("not implemented"));
  }
  void add_persistent_state_description(td::Ref<PersistentStateDescription> desc) override {
  }

  void update_options(td::Ref<ValidatorManagerOptions> opts) override {
    opts_ = std::move(opts);
  }

 private:
  PublicKeyHash local_id_;

 private:
  td::Ref<ValidatorManagerOptions> opts_;

 private:
  BlockIdExt last_masterchain_block_id_;
  BlockHandle last_masterchain_block_handle_;

  std::string db_root_;
  ShardIdFull shard_to_generate_;
  BlockIdExt block_to_generate_;

  int pending_new_shard_block_descr_{0};
  std::vector<td::Promise<std::vector<td::Ref<ShardTopBlockDescription>>>> waiting_new_shard_block_descr_;
  td::actor::ActorOwn<QueueSizeCounter> queue_size_counter_;

  void update_shards();
  void update_shard_blocks();
  void dec_pending_new_blocks();
  ValidatorSessionId get_validator_set_id(ShardIdFull shard, td::Ref<ValidatorSet> val_set);
};

}  // namespace validator

}  // namespace ton
