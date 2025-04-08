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

#include <vector>
#include <deque>
#include <functional>

#include "td/actor/actor.h"

#include "ton/ton-types.h"

#include "adnl/adnl.h"
#include "dht/dht.h"
#include "overlay/overlays.h"

#include "interfaces/block-handle.h"
#include "interfaces/validator-set.h"
#include "interfaces/block.h"
#include "interfaces/proof.h"
#include "interfaces/shard.h"
#include "catchain/catchain-types.h"
#include "interfaces/out-msg-queue-proof.h"
#include "interfaces/external-message.h"

namespace ton {

namespace validator {

class ActionToken {
 public:
  virtual ~ActionToken() = default;
};

struct PerfTimerStats {
  std::string name;
  std::deque<std::pair<double, double>> stats; // <Time::now(), duration>
};

struct CollatorOptions : public td::CntObject {
  bool deferring_enabled = true;

  // Defer messages from account after Xth message in block (excluding first messages from transactions)
  td::uint32 defer_messages_after = 10;
  // Defer all messages if out msg queue size is greater than X (excluding first messages from transactions)
  td::uint64 defer_out_queue_size_limit = 2048;

  // See Collator::process_dispatch_queue
  td::uint32 dispatch_phase_2_max_total = 150;
  td::uint32 dispatch_phase_3_max_total = 150;
  td::uint32 dispatch_phase_2_max_per_initiator = 20;
  td::optional<td::uint32> dispatch_phase_3_max_per_initiator;  // Default - depends on out msg queue size

  // Don't defer messages from these accounts
  std::set<std::pair<WorkchainId, StdSmcAddress>> whitelist;
  // Prioritize these accounts on each phase of process_dispatch_queue
  std::set<std::pair<WorkchainId, StdSmcAddress>> prioritylist;
};

struct ValidatorManagerOptions : public td::CntObject {
 public:
  enum class ShardCheckMode { m_monitor, m_validate };

  virtual BlockIdExt zero_block_id() const = 0;
  virtual BlockIdExt init_block_id() const = 0;
  virtual bool need_monitor(ShardIdFull shard, const td::Ref<MasterchainState>& state) const = 0;
  virtual bool allow_blockchain_init() const = 0;
  virtual double sync_blocks_before() const = 0;
  virtual double block_ttl() const = 0;
  virtual double state_ttl() const = 0;
  virtual double max_mempool_num() const = 0;
  virtual double archive_ttl() const = 0;
  virtual double key_proof_ttl() const = 0;
  virtual bool initial_sync_disabled() const = 0;
  virtual bool is_hardfork(BlockIdExt block_id) const = 0;
  virtual td::uint32 get_vertical_seqno(BlockSeqno seqno) const = 0;
  virtual td::uint32 get_maximal_vertical_seqno() const = 0;
  virtual td::uint32 get_last_fork_masterchain_seqno() const = 0;
  virtual std::vector<BlockIdExt> get_hardforks() const = 0;
  virtual td::uint32 key_block_utime_step() const {
    return 86400;
  }
  virtual bool check_unsafe_resync_allowed(CatchainSeqno seqno) const = 0;
  virtual td::uint32 check_unsafe_catchain_rotate(BlockSeqno seqno, CatchainSeqno cc_seqno) const = 0;
  virtual bool need_db_truncate() const = 0;
  virtual BlockSeqno get_truncate_seqno() const = 0;
  virtual BlockSeqno sync_upto() const = 0;
  virtual std::string get_session_logs_file() const = 0;
  virtual td::uint32 get_celldb_compress_depth() const = 0;
  virtual bool get_celldb_in_memory() const = 0;
  virtual bool get_celldb_v2() const = 0;
  virtual size_t get_max_open_archive_files() const = 0;
  virtual double get_archive_preload_period() const = 0;
  virtual bool get_disable_rocksdb_stats() const = 0;
  virtual bool nonfinal_ls_queries_enabled() const = 0;
  virtual td::optional<td::uint64> get_celldb_cache_size() const = 0;
  virtual bool get_celldb_direct_io() const = 0;
  virtual bool get_celldb_preload_all() const = 0;
  virtual bool get_celldb_disable_bloom_filter() const = 0;
  virtual td::optional<double> get_catchain_max_block_delay() const = 0;
  virtual td::optional<double> get_catchain_max_block_delay_slow() const = 0;
  virtual bool get_state_serializer_enabled() const = 0;
  virtual td::Ref<CollatorOptions> get_collator_options() const = 0;
  virtual bool get_fast_state_serializer_enabled() const = 0;
  virtual double get_catchain_broadcast_speed_multiplier() const = 0;

  virtual void set_zero_block_id(BlockIdExt block_id) = 0;
  virtual void set_init_block_id(BlockIdExt block_id) = 0;
  virtual void set_shard_check_function(std::function<bool(ShardIdFull)> check_shard) = 0;
  virtual void set_allow_blockchain_init(bool value) = 0;
  virtual void set_sync_blocks_before(double value) = 0;
  virtual void set_block_ttl(double value) = 0;
  virtual void set_state_ttl(double value) = 0;
  virtual void set_max_mempool_num(double value) = 0;
  virtual void set_archive_ttl(double value) = 0;
  virtual void set_key_proof_ttl(double value) = 0;
  virtual void set_initial_sync_disabled(bool value) = 0;
  virtual void set_hardforks(std::vector<BlockIdExt> hardforks) = 0;
  virtual void add_unsafe_resync_catchain(CatchainSeqno seqno) = 0;
  virtual void add_unsafe_catchain_rotate(BlockSeqno seqno, CatchainSeqno cc_seqno, td::uint32 value) = 0;
  virtual void truncate_db(BlockSeqno seqno) = 0;
  virtual void set_sync_upto(BlockSeqno seqno) = 0;
  virtual void set_session_logs_file(std::string f) = 0;
  virtual void set_celldb_compress_depth(td::uint32 value) = 0;
  virtual void set_max_open_archive_files(size_t value) = 0;
  virtual void set_archive_preload_period(double value) = 0;
  virtual void set_disable_rocksdb_stats(bool value) = 0;
  virtual void set_nonfinal_ls_queries_enabled(bool value) = 0;
  virtual void set_celldb_cache_size(td::uint64 value) = 0;
  virtual void set_celldb_direct_io(bool value) = 0;
  virtual void set_celldb_preload_all(bool value) = 0;
  virtual void set_celldb_in_memory(bool value) = 0;
  virtual void set_celldb_v2(bool value) = 0;
  virtual void set_celldb_disable_bloom_filter(bool value) = 0;
  virtual void set_catchain_max_block_delay(double value) = 0;
  virtual void set_catchain_max_block_delay_slow(double value) = 0;
  virtual void set_state_serializer_enabled(bool value) = 0;
  virtual void set_collator_options(td::Ref<CollatorOptions> value) = 0;
  virtual void set_fast_state_serializer_enabled(bool value) = 0;
  virtual void set_catchain_broadcast_speed_multiplier(double value) = 0;

  static td::Ref<ValidatorManagerOptions> create(
      BlockIdExt zero_block_id, BlockIdExt init_block_id,

      std::function<bool(ShardIdFull)> check_shard = [](ShardIdFull) { return true; },
      bool allow_blockchain_init = false, double sync_blocks_before = 3600, double block_ttl = 86400,
      double state_ttl = 86400, double archive_ttl = 86400 * 7, double key_proof_ttl = 86400 * 3650,
      double max_mempool_num = 999999, bool initial_sync_disabled = false);
};

class ValidatorManagerInterface : public td::actor::Actor {
 public:
  class Callback {
   public:
    virtual ~Callback() = default;

    virtual void initial_read_complete(BlockHandle top_masterchain_blocks) = 0;
    virtual void on_new_masterchain_block(td::Ref<ton::validator::MasterchainState> state,
                                          std::set<ShardIdFull> shards_to_monitor) = 0;

    virtual void send_ihr_message(AccountIdPrefixFull dst, td::BufferSlice data) = 0;
    virtual void send_ext_message(AccountIdPrefixFull dst, td::BufferSlice data) = 0;
    virtual void send_shard_block_info(BlockIdExt block_id, CatchainSeqno cc_seqno, td::BufferSlice data) = 0;
    virtual void send_block_candidate(BlockIdExt block_id, CatchainSeqno cc_seqno, td::uint32 validator_set_hash,
                                      td::BufferSlice data) = 0;
    virtual void send_broadcast(BlockBroadcast broadcast, int mode) = 0;
    virtual void download_block(BlockIdExt block_id, td::uint32 priority, td::Timestamp timeout,
                                td::Promise<ReceivedBlock> promise) = 0;
    virtual void download_zero_state(BlockIdExt block_id, td::uint32 priority, td::Timestamp timeout,
                                     td::Promise<td::BufferSlice> promise) = 0;
    virtual void download_persistent_state(BlockIdExt block_id, BlockIdExt masterchain_block_id, td::uint32 priority,
                                           td::Timestamp timeout, td::Promise<td::BufferSlice> promise) = 0;
    virtual void download_block_proof(BlockIdExt block_id, td::uint32 priority, td::Timestamp timeout,
                                      td::Promise<td::BufferSlice> promise) = 0;
    virtual void download_block_proof_link(BlockIdExt block_id, td::uint32 priority, td::Timestamp timeout,
                                           td::Promise<td::BufferSlice> promise) = 0;
    virtual void get_next_key_blocks(BlockIdExt block_id, td::Timestamp timeout,
                                     td::Promise<std::vector<BlockIdExt>> promise) = 0;
    virtual void download_archive(BlockSeqno masterchain_seqno, ShardIdFull shard_prefix, std::string tmp_dir,
                                  td::Timestamp timeout, td::Promise<std::string> promise) = 0;
    virtual void download_out_msg_queue_proof(ShardIdFull dst_shard, std::vector<BlockIdExt> blocks,
                                              block::ImportedMsgQueueLimits limits, td::Timestamp timeout,
                                              td::Promise<std::vector<td::Ref<OutMsgQueueProof>>> promise) = 0;

    virtual void new_key_block(BlockHandle handle) = 0;
    virtual void send_validator_telemetry(PublicKeyHash key, tl_object_ptr<ton_api::validator_telemetry> telemetry) = 0;
  };

  virtual ~ValidatorManagerInterface() = default;
  virtual void install_callback(std::unique_ptr<Callback> new_callback, td::Promise<td::Unit> promise) = 0;
  virtual void add_permanent_key(PublicKeyHash key, td::Promise<td::Unit> promise) = 0;
  virtual void add_temp_key(PublicKeyHash key, td::Promise<td::Unit> promise) = 0;
  virtual void del_permanent_key(PublicKeyHash key, td::Promise<td::Unit> promise) = 0;
  virtual void del_temp_key(PublicKeyHash key, td::Promise<td::Unit> promise) = 0;

  virtual void validate_block_is_next_proof(BlockIdExt prev_block_id, BlockIdExt next_block_id, td::BufferSlice proof,
                                            td::Promise<td::Unit> promise) = 0;
  virtual void validate_block_proof(BlockIdExt block_id, td::BufferSlice proof, td::Promise<td::Unit> promise) = 0;
  virtual void validate_block_proof_link(BlockIdExt block_id, td::BufferSlice proof, td::Promise<td::Unit> promise) = 0;
  virtual void validate_block_proof_rel(BlockIdExt block_id, BlockIdExt rel_block_id, td::BufferSlice proof,
                                        td::Promise<td::Unit> promise) = 0;
  virtual void validate_block(ReceivedBlock block, td::Promise<BlockHandle> promise) = 0;
  virtual void prevalidate_block(BlockBroadcast broadcast, td::Promise<td::Unit> promise) = 0;

  //virtual void create_validate_block(BlockId block, td::BufferSlice data, td::Promise<Block> promise) = 0;
  virtual void sync_complete(td::Promise<td::Unit> promise) = 0;

  virtual void get_top_masterchain_state(td::Promise<td::Ref<MasterchainState>> promise) = 0;
  virtual void get_top_masterchain_block(td::Promise<BlockIdExt> promise) = 0;
  virtual void get_top_masterchain_state_block(
      td::Promise<std::pair<td::Ref<MasterchainState>, BlockIdExt>> promise) = 0;
  virtual void get_last_liteserver_state_block(
      td::Promise<std::pair<td::Ref<MasterchainState>, BlockIdExt>> promise) = 0;

  virtual void get_block_data(BlockHandle handle, td::Promise<td::BufferSlice> promise) = 0;
  virtual void check_zero_state_exists(BlockIdExt block_id, td::Promise<bool> promise) = 0;
  virtual void get_zero_state(BlockIdExt block_id, td::Promise<td::BufferSlice> promise) = 0;
  virtual void get_persistent_state_size(BlockIdExt block_id, BlockIdExt masterchain_block_id,
                                         td::Promise<td::uint64> promise) = 0;
  virtual void get_persistent_state(BlockIdExt block_id, BlockIdExt masterchain_block_id,
                                    td::Promise<td::BufferSlice> promise) = 0;
  virtual void get_persistent_state_slice(BlockIdExt block_id, BlockIdExt masterchain_block_id, td::int64 offset,
                                          td::int64 max_length, td::Promise<td::BufferSlice> promise) = 0;
  virtual void get_previous_persistent_state_files(
      BlockSeqno cur_mc_seqno, td::Promise<std::vector<std::pair<std::string, ShardIdFull>>> promise) = 0;
  virtual void get_block_proof(BlockHandle handle, td::Promise<td::BufferSlice> promise) = 0;
  virtual void get_block_proof_link(BlockHandle handle, td::Promise<td::BufferSlice> promise) = 0;
  virtual void get_block_handle(BlockIdExt block_id, bool force, td::Promise<BlockHandle> promise) = 0;
  virtual void get_key_block_proof(BlockIdExt block_id, td::Promise<td::BufferSlice> promise) = 0;
  virtual void get_key_block_proof_link(BlockIdExt block_id, td::Promise<td::BufferSlice> promise) = 0;
  virtual void get_next_key_blocks(BlockIdExt block_id, td::uint32 cnt,
                                   td::Promise<std::vector<BlockIdExt>> promise) = 0;
  virtual void get_next_block(BlockIdExt block_id, td::Promise<BlockHandle> promise) = 0;
  virtual void write_handle(BlockHandle handle, td::Promise<td::Unit> promise) = 0;

  virtual void new_external_message(td::BufferSlice data, int priority) = 0;
  virtual void check_external_message(td::BufferSlice data, td::Promise<td::Ref<ExtMessage>> promise) = 0;
  virtual void new_ihr_message(td::BufferSlice data) = 0;
  virtual void new_shard_block(BlockIdExt block_id, CatchainSeqno cc_seqno, td::BufferSlice data) = 0;
  virtual void new_block_candidate(BlockIdExt block_id, td::BufferSlice data) = 0;

  virtual void add_ext_server_id(adnl::AdnlNodeIdShort id) = 0;
  virtual void add_ext_server_port(td::uint16 port) = 0;

  virtual void get_download_token(size_t download_size, td::uint32 priority, td::Timestamp timeout,
                                  td::Promise<std::unique_ptr<ActionToken>> promise) = 0;

  virtual void get_block_data_from_db(ConstBlockHandle handle, td::Promise<td::Ref<BlockData>> promise) = 0;
  virtual void get_block_data_from_db_short(BlockIdExt block_id, td::Promise<td::Ref<BlockData>> promise) = 0;
  virtual void get_block_candidate_from_db(PublicKey source, BlockIdExt id, FileHash collated_data_file_hash,
                                           td::Promise<BlockCandidate> promise) = 0;
  virtual void get_candidate_data_by_block_id_from_db(BlockIdExt id, td::Promise<td::BufferSlice> promise) = 0;
  virtual void get_shard_state_from_db(ConstBlockHandle handle, td::Promise<td::Ref<ShardState>> promise) = 0;
  virtual void get_shard_state_from_db_short(BlockIdExt block_id, td::Promise<td::Ref<ShardState>> promise) = 0;
  virtual void get_block_proof_from_db(ConstBlockHandle handle, td::Promise<td::Ref<Proof>> promise) = 0;
  virtual void get_block_proof_from_db_short(BlockIdExt id, td::Promise<td::Ref<Proof>> promise) = 0;
  virtual void get_block_proof_link_from_db(ConstBlockHandle handle, td::Promise<td::Ref<ProofLink>> promise) = 0;
  virtual void get_block_proof_link_from_db_short(BlockIdExt id, td::Promise<td::Ref<ProofLink>> promise) = 0;

  virtual void get_block_by_lt_from_db(AccountIdPrefixFull account, LogicalTime lt,
                                       td::Promise<ConstBlockHandle> promise) = 0;
  virtual void get_block_by_unix_time_from_db(AccountIdPrefixFull account, UnixTime ts,
                                              td::Promise<ConstBlockHandle> promise) = 0;
  virtual void get_block_by_seqno_from_db(AccountIdPrefixFull account, BlockSeqno seqno,
                                          td::Promise<ConstBlockHandle> promise) = 0;

  virtual void wait_block_state(BlockHandle handle, td::uint32 priority, td::Timestamp timeout,
                                td::Promise<td::Ref<ShardState>> promise) = 0;
  virtual void wait_block_state_short(BlockIdExt block_id, td::uint32 priority, td::Timestamp timeout,
                                      td::Promise<td::Ref<ShardState>> promise) = 0;

  virtual void get_archive_id(BlockSeqno masterchain_seqno, ShardIdFull shard_prefix,
                              td::Promise<td::uint64> promise) = 0;
  virtual void get_archive_slice(td::uint64 archive_id, td::uint64 offset, td::uint32 limit,
                                 td::Promise<td::BufferSlice> promise) = 0;

  virtual void run_ext_query(td::BufferSlice data, td::Promise<td::BufferSlice> promise) = 0;
  virtual void prepare_stats(td::Promise<std::vector<std::pair<std::string, std::string>>> promise) = 0;
  virtual void prepare_actor_stats(td::Promise<std::string> promise) = 0;

  virtual void prepare_perf_timer_stats(td::Promise<std::vector<PerfTimerStats>> promise) = 0;
  virtual void add_perf_timer_stat(std::string name, double duration) = 0;
  virtual void get_out_msg_queue_size(BlockIdExt block_id, td::Promise<td::uint64> promise) = 0;

  virtual void update_options(td::Ref<ValidatorManagerOptions> opts) = 0;

  virtual void register_stats_provider(
      td::uint64 idx, std::string prefix,
      std::function<void(td::Promise<std::vector<std::pair<std::string, std::string>>>)> callback) {
  }
  virtual void unregister_stats_provider(td::uint64 idx) {
  }
};

}  // namespace validator

}  // namespace ton
