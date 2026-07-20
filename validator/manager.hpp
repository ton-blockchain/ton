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

#include <list>
#include <map>
#include <queue>
#include <set>

#include "common/refcnt.hpp"
#include "db/db-event-publisher.hpp"
#include "impl/ext-message-pool.hpp"
#include "interfaces/db.h"
#include "interfaces/validator-manager.h"
#include "metrics/prometheus-exporter.h"
#include "quic/quic-sender.h"
#include "rldp2/rldp.h"
#include "td/actor/ActorStats.h"
#include "td/actor/MultiPromise.h"
#include "td/actor/PromiseFuture.h"
#include "td/actor/coro_task.h"
#include "td/utils/LRUCache.h"
#include "td/utils/SharedSlice.h"
#include "td/utils/buffer.h"
#include "td/utils/port/Poll.h"
#include "td/utils/port/StdStreams.h"
#include "ton/ton-io.hpp"

#include "collator-scoreboard.hpp"
#include "manager-init.h"
#include "queue-size-counter.hpp"
#include "shard-block-retainer.hpp"
#include "shard-block-verifier.hpp"
#include "shard-client.hpp"
#include "state-serializer.hpp"
#include "storage-stat-cache.hpp"
#include "token-manager.h"
#include "validator-group.hpp"
#include "validator-registry-watcher.hpp"

namespace ton {

namespace validator {

class WaitBlockState;
class WaitZeroState;
class WaitShardState;
class WaitBlockData;
class AppliedExtMessageCleanupActor;

class BlockHandleLru : public td::ListNode {
 public:
  BlockHandle handle() const {
    return handle_;
  }
  BlockHandleLru(BlockHandle h) : handle_(std::move(h)) {
  }
  static inline BlockHandleLru *from_list_node(ListNode *node) {
    return static_cast<BlockHandleLru *>(node);
  }

 private:
  BlockHandle handle_;
};

#define BLOCK_SOURCE_LIST(F)       \
  F(unknown)                       \
  F(block_broadcast_public)        \
  F(block_broadcast_fast_sync)     \
  F(block_broadcast_custom)        \
  F(block_download)                \
  F(candidate_broadcast_public)    \
  F(candidate_broadcast_fast_sync) \
  F(candidate_broadcast_consensus) \
  F(candidate_broadcast_custom)    \
  F(candidate_finality_public)     \
  F(candidate_finality_fast_sync)  \
  F(candidate_finality_custom)     \
  F(candidate_stored)              \
  F(block_accepted)
TON_METRIC_DEFINE_LABEL(BlockSource, "source", BLOCK_SOURCE_LIST)
#undef BLOCK_SOURCE_LIST

struct BlockReceiveStats {
  static constexpr auto n_types = metrics::LabelDomainOf<BlockSource>::size;

  td::Timestamp received_at[n_types];
  bool applied = false;

  BlockSource get_earliest_type() const {
    auto result = BlockSource::unknown;
    td::Timestamp result_ts;
    for (size_t i = 0; i < n_types; i++) {
      if (received_at[i] && (!result_ts || received_at[i] < result_ts)) {
        result = static_cast<BlockSource>(i);
        result_ts = received_at[i];
      }
    }
    return result;
  }

  static BlockSource from(BroadcastSource source, bool is_candidate) {
    switch (source) {
      case BroadcastSource::public_overlay:
        return is_candidate ? BlockSource::candidate_broadcast_public : BlockSource::block_broadcast_public;
      case BroadcastSource::fast_sync_overlay:
        return is_candidate ? BlockSource::candidate_broadcast_fast_sync : BlockSource::block_broadcast_fast_sync;
      case BroadcastSource::custom_overlay:
        return is_candidate ? BlockSource::candidate_broadcast_custom : BlockSource::block_broadcast_custom;
      case BroadcastSource::consensus_overlay:
        return BlockSource::candidate_broadcast_consensus;
    }
    UNREACHABLE();
  }

  static BlockSource from_candidate_finality(BroadcastSource source) {
    switch (source) {
      case BroadcastSource::public_overlay:
        return BlockSource::candidate_finality_public;
      case BroadcastSource::fast_sync_overlay:
        return BlockSource::candidate_finality_fast_sync;
      case BroadcastSource::custom_overlay:
        return BlockSource::candidate_finality_custom;
      case BroadcastSource::consensus_overlay:
        // This happens when we get block finality directly from consensus
        return BlockSource::block_accepted;
    }
    UNREACHABLE();
  }
};

struct PendingBlockFinality {
  td::Ref<block::BlockSignatureSet> sig_set;
  td::Ref<vm::Cell> serialized;  // only for is_final
  BroadcastSource source;
};

struct WorkchainLabel {
  int id;

  bool operator==(const WorkchainLabel &) const = default;
};

constexpr metrics::LabelValues<WorkchainLabel, 2> ton_metric_label(WorkchainLabel) {
  return {
      .key = "workchain",
      .entries = {{
          {WorkchainLabel{0}, "0"},
          {WorkchainLabel{-1}, "-1"},
      }},
  };
}

class ValidatorManagerImpl : public ValidatorManager {
 private:
  // WAITERS
  //
  //  list of promises waiting for same object
  template <typename ResType>
  struct Waiter {
    td::Timestamp timeout;
    td::uint32 priority;
    td::Promise<ResType> promise;

    Waiter() {
    }
    Waiter(td::Timestamp timeout, td::uint32 priority, td::Promise<ResType> promise)
        : timeout(timeout), priority(priority), promise(std::move(promise)) {
    }
  };
  template <typename ActorT, typename ResType>
  struct WaitList {
    std::vector<Waiter<ResType>> waiting_;
    td::actor::ActorId<ActorT> actor_;

    WaitList() = default;

    std::pair<td::Timestamp, td::uint32> get_timeout() const {
      return get_timeout_impl(waiting_);
    }
    void check_timers() {
      check_timers_impl(waiting_);
    }

   protected:
    static std::pair<td::Timestamp, td::uint32> get_timeout_impl(const std::vector<Waiter<ResType>> &waiting) {
      td::Timestamp t = td::Timestamp::now();
      td::uint32 prio = 0;
      for (auto &v : waiting) {
        if (v.timeout.at() > t.at()) {
          t = v.timeout;
        }
        if (v.priority > prio) {
          prio = v.priority;
        }
      }
      return {td::Timestamp::at(t.at() + 10.0), prio};
    }
    static void check_timers_impl(std::vector<Waiter<ResType>> &waiting) {
      td::uint32 j = 0;
      auto f = waiting.begin();
      auto t = waiting.end();
      while (f < t) {
        if (f->timeout.is_in_past()) {
          f->promise.set_error(td::Status::Error(ErrorCode::timeout, "timeout"));
          t--;
          std::swap(*f, *t);
        } else {
          f++;
          j++;
        }
      }
      waiting.resize(j);
    }
  };
  template <typename ActorT, typename ResType>
  struct WaitListPreliminary : WaitList<ActorT, ResType> {
    std::vector<Waiter<ResType>> waiting_preliminary_;
    bool preliminary_done_ = false;
    ResType preliminary_result_;

    std::pair<td::Timestamp, td::uint32> get_timeout() const {
      auto t1 = WaitList<ActorT, ResType>::get_timeout_impl(this->waiting_);
      auto t2 = WaitList<ActorT, ResType>::get_timeout_impl(waiting_preliminary_);
      return {std::max(t1.first, t2.first), std::max(t1.second, t2.second)};
    }
    void check_timers() {
      WaitList<ActorT, ResType>::check_timers_impl(this->waiting_);
      WaitList<ActorT, ResType>::check_timers_impl(waiting_preliminary_);
    }
  };
  std::map<BlockIdExt, WaitListPreliminary<WaitBlockState, td::Ref<ShardState>>> wait_state_;
  std::map<BlockIdExt, WaitList<WaitBlockData, td::Ref<BlockData>>> wait_block_data_;

  struct CachedBlockState {
    td::Ref<ShardState> state_;
    td::Timestamp ttl_;
  };
  std::map<BlockIdExt, CachedBlockState> block_state_cache_;

  struct WaitBlockHandle {
    std::vector<td::Promise<BlockHandle>> waiting_;
    bool force_ = false;
  };
  std::map<BlockIdExt, WaitBlockHandle> wait_block_handle_;

  td::actor::ActorOwn<OutMsgQueueImporter> out_msg_queue_importer_;

 private:
  // HANDLES CACHE
  std::map<BlockIdExt, std::weak_ptr<BlockHandleInterface>> handles_;

  static constexpr td::uint32 handle_lru_max_size_ = 16;
  td::uint32 handle_lru_size_ = 0;

  std::map<BlockIdExt, std::unique_ptr<BlockHandleLru>> handle_lru_map_;
  td::ListNode handle_lru_;

  void add_handle_to_lru(BlockHandle handle);
  BlockHandle get_handle_from_lru(BlockIdExt id);

 private:
  struct ShardTopBlockDescriptionId {
    ShardIdFull id;
    CatchainSeqno cc_seqno;

    bool operator<(const ShardTopBlockDescriptionId &with) const {
      return id < with.id || (id == with.id && cc_seqno < with.cc_seqno);
    }
  };
  // DATA FOR COLLATOR
  struct ShardTopBlock {
    td::Ref<ShardTopBlockDescription> latest_desc;
  };
  std::map<ShardTopBlockDescriptionId, ShardTopBlock> shard_blocks_;
  std::set<BlockIdExt> active_shard_block_desc_generation_;

  td::LRUCache<BlockIdExt, td::BufferSlice> cached_block_data_{/* max_size = */ 256};
  td::LRUCache<BlockIdExt, PendingBlockFinality> pending_block_finality_{/* max_size = */ 256};
  std::set<BlockIdExt> active_broadcast_checks_;

  td::actor::ActorOwn<ExtMessagePool> ext_message_pool_;
  td::actor::ActorOwn<AppliedExtMessageCleanupActor> applied_ext_message_cleanup_actor_;

 private:
  // VALIDATOR GROUPS
  std::unique_ptr<NetworkState> network_state_;

 private:
  // MASTERCHAIN LAST BLOCK
  BlockSeqno last_masterchain_seqno_ = 0;
  BlockSeqno last_init_masterchain_seqno_ = 0;
  td::Ref<MasterchainState> last_masterchain_state_;
  BlockIdExt last_masterchain_block_id_;
  BlockHandle last_masterchain_block_handle_;

  BlockHandle last_key_block_handle_;
  BlockHandle last_known_key_block_handle_;
  BlockHandle shard_client_handle_;
  td::Ref<MasterchainState> shard_client_state_;
  std::vector<td::Ref<McShardHash>> shard_client_shards_;
  td::Ref<MasterchainState> last_liteserver_state_;

  td::Ref<MasterchainState> do_get_last_liteserver_state();

  BlockHandle gc_masterchain_handle_;
  td::Ref<MasterchainState> gc_masterchain_state_;
  bool gc_advancing_ = false;

  BlockIdExt last_rotate_block_id_;

  std::map<BlockSeqno, std::tuple<BlockHandle, td::Ref<MasterchainState>, std::vector<td::Promise<td::Unit>>>>
      pending_masterchain_states_;

  std::vector<PerfTimerStats> perf_timer_stats;

  void new_masterchain_block();
  void update_shard_overlays();
  void update_shards();
  void update_shard_blocks();
  void updated_init_block(BlockIdExt last_rotate_block_id);
  void got_next_gc_masterchain_handle(BlockHandle handle);
  void got_next_gc_masterchain_state(BlockHandle handle, td::Ref<MasterchainState> state);
  void advance_gc(BlockHandle handle, td::Ref<MasterchainState> state);
  void try_advance_gc_masterchain_block();
  void update_gc_block_handle(BlockHandle handle, td::Promise<td::Unit> promise) override;
  void update_shard_client_block_handle(BlockHandle handle, td::Ref<MasterchainState> state,
                                        td::Promise<td::Unit> promise) override;

  bool out_of_sync();
  void applied_hardfork();
  void prestart_sync();
  void download_next_archive();
  void checked_archive_slice(BlockSeqno new_last_mc_seqno, BlockSeqno new_shard_client_seqno);
  void finish_prestart_sync();
  void completed_prestart_sync();

 public:
  void install_callback(std::unique_ptr<Callback> new_callback, td::Promise<td::Unit> promise) override {
    callback_ = std::move(new_callback);
    promise.set_value(td::Unit());
  }

  void add_temp_key(PublicKeyHash key, td::Promise<td::Unit> promise) override {
    if (validator_keys_.insert(key).second) {
      validator_registry_watchers_[key] =
          td::actor::create_actor<ValidatorRegistryWatcher>("ValidatorRegistry", key, actor_id(this), keyring_);
    }
    promise.set_value(td::Unit());
  }
  void del_temp_key(PublicKeyHash key, td::Promise<td::Unit> promise) override {
    validator_keys_.erase(key);
    validator_registry_watchers_.erase(key);
    promise.set_value(td::Unit());
  }

  void validate_block_is_next_proof(BlockIdExt prev_block_id, BlockIdExt next_block_id, td::BufferSlice proof,
                                    td::Promise<td::Unit> promise) override;
  void validate_block_proof(BlockIdExt block_id, td::BufferSlice proof, td::Promise<td::Unit> promise) override;
  void validate_block_proof_link(BlockIdExt block_id, td::BufferSlice proof, td::Promise<td::Unit> promise) override;
  void validate_block_proof_rel(BlockIdExt block_id, BlockIdExt rel_block_id, td::BufferSlice proof,
                                td::Promise<td::Unit> promise) override;
  void got_next_masterchain_block(ReceivedBlock block, td::Promise<BlockHandle> promise) override;
  td::Status check_need_generate_shard_block_description(BlockIdExt block_id, CatchainSeqno cc_seqno);
  td::actor::Task<> generate_shard_block_description(BlockIdExt block_id, Ref<block::BlockSignatureSet> sig_set);

  //void create_validate_block(BlockId block, td::BufferSlice data, td::Promise<Block> promise) = 0;
  void sync_complete(td::Promise<td::Unit> promise) override;
  void wait_initial_sync(td::Promise<td::Unit> promise) override;

  void get_next_block(BlockIdExt block_id, td::Promise<BlockHandle> promise) override;
  void get_next_key_blocks(BlockIdExt block_id, td::uint32 cnt, td::Promise<std::vector<BlockIdExt>> promise) override;
  void get_block_data(BlockHandle handle, td::Promise<td::BufferSlice> promise) override;
  void check_zero_state_exists(BlockIdExt block_id, td::Promise<bool> promise) override;
  void get_zero_state(BlockIdExt block_id, td::Promise<td::BufferSlice> promise) override;
  void get_persistent_state_size(BlockIdExt block_id, BlockIdExt masterchain_block_id, PersistentStateType type,
                                 td::Promise<td::uint64> promise) override;
  void get_persistent_state(BlockIdExt block_id, BlockIdExt masterchain_block_id, PersistentStateType type,
                            td::Promise<td::BufferSlice> promise) override;
  void get_persistent_state_slice(BlockIdExt block_id, BlockIdExt masterchain_block_id, PersistentStateType type,
                                  td::int64 offset, td::int64 max_length,
                                  td::Promise<td::BufferSlice> promise) override;
  void get_previous_persistent_state_files(
      BlockSeqno cur_mc_seqno, td::Promise<std::vector<std::pair<std::string, ShardIdFull>>> promise) override;
  void get_cached_candidate_data(BlockIdExt id, td::Promise<td::BufferSlice> promise) override;
  void get_block_proof(BlockHandle handle, td::Promise<td::BufferSlice> promise) override;
  void get_block_proof_link(BlockHandle block_id, td::Promise<td::BufferSlice> promise) override;
  void get_key_block_proof(BlockIdExt block_id, td::Promise<td::BufferSlice> promise) override;
  void get_key_block_proof_link(BlockIdExt block_id, td::Promise<td::BufferSlice> promise) override;
  //void get_block_description(BlockIdExt block_id, td::Promise<BlockDescription> promise) override;

  td::actor::Task<> new_external_message_broadcast(td::BufferSlice data, int priority) override;
  td::actor::Task<> new_external_message_query(td::BufferSlice data) override;
  td::actor::Task<> new_external_message_query_cont(td::Ref<ExtMessage> message,
                                                    td::actor::StartedTask<> wait_allow_broadcast);

  td::actor::Task<> got_block_finality(BlockIdExt block_id, Ref<block::BlockSignatureSet> sig_set,
                                       BroadcastSource source) override;

  void add_ext_server_id(adnl::AdnlNodeIdShort id) override;
  void add_ext_server_port(td::uint16 port) override;
  void notify_added_initial_liteservers() override;
  void wait_liteserver_ready(td::Promise<td::Unit> promise) override;

  void run_ext_query(td::BufferSlice data, td::Promise<td::BufferSlice> promise) override;

  void get_block_handle(BlockIdExt id, bool force, td::Promise<BlockHandle> promise) override;
  void get_block_handle_cont(BlockIdExt id, td::Result<BlockHandle> R);

  void set_block_state(BlockHandle handle, td::Ref<ShardState> state, vm::StoreCellHint hint,
                       td::Promise<td::Ref<ShardState>> promise) override;
  void store_block_state_part(BlockId effective_block, td::Ref<vm::Cell> cell,
                              td::Promise<td::Ref<vm::DataCell>> promise) override;
  void set_block_state_from_data(BlockHandle handle, td::Ref<BlockData> block,
                                 td::Promise<td::Ref<ShardState>> promise) override;
  void set_block_state_from_data_bulk(std::vector<td::Ref<BlockData>> blocks, td::Promise<td::Unit> promise) override;
  void get_cell_db_reader(td::Promise<std::shared_ptr<vm::CellDbReader>> promise) override;
  void store_persistent_state_file(BlockIdExt block_id, BlockIdExt masterchain_block_id, PersistentStateType type,
                                   td::BufferSlice state, td::Promise<td::Unit> promise) override;
  void store_persistent_state_file_gen(BlockIdExt block_id, BlockIdExt masterchain_block_id, PersistentStateType type,
                                       std::function<td::Status(td::FileFd &)> write_data,
                                       td::Promise<td::Unit> promise) override;
  void store_zero_state_file(BlockIdExt block_id, td::BufferSlice state, td::Promise<td::Unit> promise) override;
  void wait_block_state(BlockHandle handle, td::uint32 priority, td::Timestamp timeout, bool wait_store,
                        td::Promise<td::Ref<ShardState>> promise) override;
  void wait_block_state_short(BlockIdExt block_id, td::uint32 priority, td::Timestamp timeout, bool wait_store,
                              td::Promise<td::Ref<ShardState>> promise) override;
  void wait_neighbor_msg_queue_proofs(ShardIdFull dst_shard, std::vector<BlockIdExt> blocks, td::Timestamp timeout,
                                      td::Promise<std::map<BlockIdExt, td::Ref<OutMsgQueueProof>>> promise) override;

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

  void set_block_signatures(BlockHandle handle, td::Ref<block::BlockSignatureSet> signatures,
                            Ref<block::ValidatorSet> vset, td::Promise<td::Unit> promise) override;
  void wait_block_signatures(BlockHandle handle, td::Timestamp timeout,
                             td::Promise<td::Ref<block::BlockSignatureSet>> promise) override;
  void wait_block_signatures_short(BlockIdExt id, td::Timestamp timeout,
                                   td::Promise<td::Ref<block::BlockSignatureSet>> promise) override;

  void send_block_candidate_broadcast(BlockIdExt id, CatchainSeqno cc_seqno, td::uint32 validator_set_hash,
                                      td::BufferSlice data, int mode) override;

  void wait_block_state_merge(BlockIdExt left_id, BlockIdExt right_id, td::uint32 priority, td::Timestamp timeout,
                              td::Promise<td::Ref<ShardState>> promise) override;
  void wait_state_by_prev_blocks(BlockIdExt block_id, std::vector<BlockIdExt> prev_blocks,
                                 td::Promise<td::Ref<ShardState>> promise) override;
  void wait_prev_block_state(BlockHandle handle, td::uint32 priority, td::Timestamp timeout,
                             td::Promise<td::Ref<ShardState>> promise) override;

  void wait_block_message_queue(BlockHandle handle, td::uint32 priority, td::Timestamp timeout,
                                td::Promise<td::Ref<MessageQueue>> promise) override;
  void wait_block_message_queue_short(BlockIdExt id, td::uint32 priority, td::Timestamp timeout,
                                      td::Promise<td::Ref<MessageQueue>> promise) override;
  void get_external_messages(ShardIdFull shard, std::unique_ptr<ExtMsgCallback> callback) override;
  void get_shard_blocks_for_collator(BlockIdExt masterchain_block_id,
                                     td::Promise<std::vector<td::Ref<ShardTopBlockDescription>>> promise) override;
  void complete_external_messages(std::vector<ExtMessage::Hash> to_delay,
                                  std::vector<ExtMessage::Hash> to_delete) override;
  void cleanup_applied_external_messages(BlockHandle handle, td::Ref<BlockData> block) override;

  void set_next_block(BlockIdExt prev, BlockIdExt next, td::Promise<td::Unit> promise) override;

  void get_block_data_from_db(ConstBlockHandle handle, td::Promise<td::Ref<BlockData>> promise) override;
  void get_block_data_from_db_short(BlockIdExt block_id, td::Promise<td::Ref<BlockData>> promise) override;
  void get_shard_state_from_db(ConstBlockHandle handle, td::Promise<td::Ref<ShardState>> promise) override;
  void get_shard_state_from_db_short(BlockIdExt block_id, td::Promise<td::Ref<ShardState>> promise) override;
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
  void written_handle(BlockHandle handle, td::Promise<td::Unit> promise);

  void new_block(BlockHandle handle, td::Ref<ShardState> state, td::Promise<td::Unit> promise) override;
  void new_block_cont(BlockHandle handle, td::Ref<ShardState> state, td::Promise<td::Unit> promise);
  void on_block_accepted(BlockIdExt block_id) override;
  void get_top_masterchain_state(td::Promise<td::Ref<MasterchainState>> promise) override;
  void get_top_masterchain_block(td::Promise<BlockIdExt> promise) override;
  void get_top_masterchain_state_block(td::Promise<std::pair<td::Ref<MasterchainState>, BlockIdExt>> promise) override;
  void get_last_liteserver_state_block(td::Promise<std::pair<td::Ref<MasterchainState>, BlockIdExt>> promise) override;
  void get_shard_client_state_block(td::Promise<std::pair<td::Ref<MasterchainState>, BlockIdExt>> promise) override;

  td::actor::Task<ReceivedBlock> send_get_block_request(BlockIdExt id, td::uint32 priority) override;
  void send_get_zero_state_request(BlockIdExt id, td::uint32 priority, td::Promise<td::BufferSlice> promise) override;
  void send_get_persistent_state_request(BlockIdExt id, BlockIdExt masterchain_block_id, PersistentStateType type,
                                         td::uint32 priority, td::Promise<td::BufferSlice> promise) override;
  void send_get_block_proof_request(BlockIdExt block_id, td::uint32 priority,
                                    td::Promise<td::BufferSlice> promise) override;
  void send_get_block_proof_link_request(BlockIdExt block_id, td::uint32 priority,
                                         td::Promise<td::BufferSlice> promise) override;
  void send_get_next_key_blocks_request(BlockIdExt block_id, td::uint32 priority,
                                        td::Promise<std::vector<BlockIdExt>> promise) override;
  void send_block_finality_broadcast(BlockFinalityBroadcast finality, int mode) override;
  void send_get_out_msg_queue_proof_request(ShardIdFull dst_shard, std::vector<BlockIdExt> blocks,
                                            block::ImportedMsgQueueLimits limits,
                                            td::Promise<std::vector<td::Ref<OutMsgQueueProof>>> promise) override;
  void send_download_archive_request(BlockSeqno mc_seqno, ShardIdFull shard_prefix, std::string tmp_dir,
                                     td::Timestamp timeout, td::Promise<std::string> promise) override;

  void get_block_proof_link_from_import(BlockIdExt block_id, BlockIdExt masterchain_block_id,
                                        td::Promise<td::BufferSlice> promise) override;

  void update_shard_client_state(BlockIdExt masterchain_block_id, td::Promise<td::Unit> promise) override;
  void get_shard_client_state(bool from_db, td::Promise<BlockIdExt> promise) override;

  void update_async_serializer_state(AsyncSerializerState state, td::Promise<td::Unit> promise) override;
  void get_async_serializer_state(td::Promise<AsyncSerializerState> promise) override;

  void try_get_static_file(FileHash file_hash, td::Promise<td::BufferSlice> promise) override;

  void get_download_token(size_t download_size, td::uint32 priority, td::Timestamp timeout,
                          td::Promise<std::unique_ptr<ActionToken>> promise) override {
    td::actor::send_closure(token_manager_, &TokenManager::get_token, download_size, priority, timeout,
                            std::move(promise));
  }

  void get_archive_id(BlockSeqno masterchain_seqno, ShardIdFull shard_prefix, td::Promise<td::uint64> promise) override;
  void get_archive_slice(td::uint64 archive_id, td::uint64 offset, td::uint32 limit,
                         td::Promise<td::BufferSlice> promise) override;

  void check_is_hardfork(BlockIdExt block_id, td::Promise<bool> promise) override {
    CHECK(block_id.is_masterchain());
    promise.set_result(opts_->is_hardfork(block_id));
  }
  void get_vertical_seqno(BlockSeqno seqno, td::Promise<td::uint32> promise) override {
    promise.set_result(opts_->get_vertical_seqno(seqno));
  }

  td::actor::Task<> check_pending_block_needed(BlockIdExt block_id, bool check_block_received = true);
  td::actor::Task<> add_cached_block_data(BlockIdExt block_id, CatchainSeqno cc_seqno, td::BufferSlice data,
                                          BroadcastSource source) override;
  td::actor::Task<> try_process_pending_block_finality(BlockIdExt block_id);
  td::actor::Task<> try_process_pending_block_finality_inner(BlockIdExt block_id);

  void finished_wait_state(BlockHandle handle, td::Result<td::Ref<ShardState>> R, bool preliminary);
  void finished_wait_data(BlockHandle handle, td::Result<td::Ref<BlockData>> R);

  void start_up() override;
  void init_last_masterchain_state(td::Ref<MasterchainState> state) override;
  void started(ValidatorManagerInitResult result);
  td::actor::Task<> finish_start_up();
  td::actor::Task<> start_up_advance_mc();

  bool is_validator();
  bool validating_masterchain();
  PublicKeyHash get_validator(ShardIdFull shard, td::Ref<block::ValidatorSet> val_set);
  bool is_shard_collator(ShardIdFull shard);

  ValidatorManagerImpl(td::Ref<ValidatorManagerOptions> opts, std::string db_root,
                       td::actor::ActorId<keyring::Keyring> keyring, td::actor::ActorId<adnl::Adnl> adnl,
                       td::actor::ActorId<rldp2::Rldp> rldp2, td::actor::ActorId<quic::QuicSender> quic,
                       td::actor::ActorId<overlay::Overlays> overlays)
      : opts_(std::move(opts))
      , db_root_(db_root)
      , keyring_(keyring)
      , adnl_(adnl)
      , rldp2_(rldp2)
      , quic_(quic)
      , overlays_(overlays) {
  }

 public:
  void allow_block_state_gc(BlockIdExt block_id, td::Promise<bool> promise) override;
  void archive(BlockHandle handle, td::Promise<td::Unit> promise) override {
    td::actor::send_closure(db_, &Db::archive, std::move(handle), std::move(promise));
  }

  void send_peek_key_block_request();
  void got_next_key_blocks(std::vector<BlockIdExt> vec);
  void update_last_known_key_block(BlockHandle handle, bool send_request) override;

  void prepare_stats(td::Promise<std::vector<std::pair<std::string, std::string>>> promise) override;

  void prepare_actor_stats(td::Promise<std::string> promise) override;

  void prepare_perf_timer_stats(td::Promise<std::vector<PerfTimerStats>> promise) override;
  void add_perf_timer_stat(std::string name, double duration) override;

  void truncate(BlockSeqno seqno, ConstBlockHandle handle, td::Promise<td::Unit> promise) override;

  void wait_shard_client_state(BlockSeqno seqno, td::Timestamp timeout, td::Promise<td::Unit> promise) override;

  void log_stats(std::string);

  void update_options(td::Ref<ValidatorManagerOptions> opts) override;

  void add_persistent_state_description(td::Ref<PersistentStateDescription> desc) override;

  void add_collator(adnl::AdnlNodeIdShort id, ShardIdFull shard) override;
  void del_collator(adnl::AdnlNodeIdShort id, ShardIdFull shard) override;
  void add_out_msg_queue_proof(ShardIdFull dst_shard, td::Ref<OutMsgQueueProof> proof) override;

  void get_collation_manager_stats(
      td::Promise<tl_object_ptr<ton_api::engine_validator_collationManagerStats>> promise) override;

  void get_out_msg_queue_size(BlockIdExt block_id, td::Promise<td::uint64> promise) override {
    if (queue_size_counter_.empty()) {
      if (last_masterchain_state_.is_null()) {
        promise.set_error(td::Status::Error(ErrorCode::notready, "not ready"));
        return;
      }
      queue_size_counter_ =
          td::actor::create_actor<QueueSizeCounter>("queuesizecounter", last_masterchain_state_, opts_, actor_id(this));
    }
    if (!opts_->need_monitor(block_id.shard_full(), last_masterchain_state_)) {
      return promise.set_error(td::Status::Error(PSTRING() << "not monitoring shard " << block_id.shard_full()));
    }
    td::actor::send_closure(queue_size_counter_, &QueueSizeCounter::get_queue_size, block_id, std::move(promise));
  }

  void get_block_handle_for_litequery(BlockIdExt block_id, td::Promise<ConstBlockHandle> promise) override;
  void get_block_data_for_litequery(BlockIdExt block_id, td::Promise<td::Ref<BlockData>> promise) override;
  void get_block_state_for_litequery(BlockIdExt block_id, td::Promise<td::Ref<ShardState>> promise) override;
  void get_block_by_lt_for_litequery(AccountIdPrefixFull account, LogicalTime lt,
                                     td::Promise<ConstBlockHandle> promise) override;
  void get_block_by_unix_time_for_litequery(AccountIdPrefixFull account, UnixTime ts,
                                            td::Promise<ConstBlockHandle> promise) override;
  void get_block_by_seqno_for_litequery(AccountIdPrefixFull account, BlockSeqno seqno,
                                        td::Promise<ConstBlockHandle> promise) override;
  void process_block_handle_for_litequery_error(BlockIdExt block_id, td::Result<BlockHandle> r_handle,
                                                td::Promise<ConstBlockHandle> promise);
  void process_lookup_block_for_litequery_error(AccountIdPrefixFull account, int type, td::uint64 value,
                                                td::Result<ConstBlockHandle> r_handle,
                                                td::Promise<ConstBlockHandle> promise);
  void get_pending_shard_blocks_for_litequery(
      td::optional<ShardIdFull> shard,
      td::Promise<tl_object_ptr<lite_api::liteServer_nonfinal_pendingShardBlocks>> promise) override;

  void add_lite_query_stats(int lite_query_id, bool success) override {
    ++ls_stats_[lite_query_id];
    ++(success ? total_ls_queries_ok_ : total_ls_queries_error_)[lite_query_id];
  }

  void get_storage_stat_cache(td::Promise<std::function<td::Ref<vm::Cell>(const td::Bits256 &)>> promise) override {
    td::actor::send_closure(storage_stat_cache_, &StorageStatCache::get_cache, std::move(promise));
  }
  void update_storage_stat_cache(std::vector<std::pair<td::Ref<vm::Cell>, td::uint32>> data) override {
    td::actor::send_closure(storage_stat_cache_, &StorageStatCache::update, std::move(data));
  }

 private:
  td::Timestamp check_waiters_at_;
  td::Timestamp check_shard_clients_;
  td::Timestamp log_status_at_;
  void alarm() override;

 private:
  td::actor::ActorOwn<TokenManager> token_manager_;

 private:
  std::set<PublicKeyHash> validator_keys_;
  std::map<PublicKeyHash, td::actor::ActorOwn<ValidatorRegistryWatcher>> validator_registry_watchers_;
  std::set<adnl::AdnlNodeIdShort> local_collator_adnl_ids_;
  td::actor::ActorOwn<CollatorScoreboard> collator_scoreboard_ =
      td::actor::create_actor<CollatorScoreboard>("CollatorScoreboard");

 private:
  td::Ref<ValidatorManagerOptions> opts_;

 private:
  td::actor::ActorOwn<adnl::AdnlExtServer> lite_server_;
  td::actor::ActorOwn<LiteServerCache> lite_server_cache_;
  std::vector<std::pair<td::uint16, td::Promise<td::Unit>>> pending_ext_ports_;
  std::vector<adnl::AdnlNodeIdShort> pending_ext_ids_;
  bool liteserver_ready_ = false;
  td::MultiPromise::InitGuard initial_liteservers_guard_;
  std::vector<td::Promise<td::Unit>> pending_liteserver_promises_;
  std::vector<td::Promise<td::Unit>> pending_sync_promises_;

  void created_ext_server(td::actor::ActorOwn<adnl::AdnlExtServer> lite_server);
  void liteserver_ports_bound();

 private:
  td::actor::ActorOwn<ShardClient> shard_client_;
  BlockSeqno min_confirmed_masterchain_seqno_{0};
  BlockSeqno state_serializer_masterchain_seqno_{0};

  void shard_client_update(BlockSeqno seqno);
  void state_serializer_update(BlockSeqno seqno);

  std::string db_root_;
  td::actor::ActorId<keyring::Keyring> keyring_;
  td::actor::ActorId<adnl::Adnl> adnl_;
  td::actor::ActorId<rldp2::Rldp> rldp2_;
  td::actor::ActorId<quic::QuicSender> quic_;
  td::actor::ActorId<overlay::Overlays> overlays_;

  td::actor::ActorOwn<AsyncStateSerializer> serializer_;

  std::map<BlockSeqno, std::vector<std::string>> to_import_;
  std::map<BlockSeqno, std::vector<std::string>> to_import_all_;

 private:
  std::unique_ptr<Callback> callback_;
  td::actor::ActorOwn<Db> db_;
  td::actor::ActorOwn<td::actor::ActorStats> actor_stats_;

  bool started_ = false;

 private:
  double state_ttl() const {
    return opts_->state_ttl();
  }

  void got_persistent_state_descriptions(std::vector<td::Ref<PersistentStateDescription>> descs);
  void add_persistent_state_description_impl(td::Ref<PersistentStateDescription> desc);
  td::Ref<PersistentStateDescription> get_block_persistent_state_to_download(BlockIdExt block_id);

 private:
  bool need_monitor(ShardIdFull shard) const {
    return opts_->need_monitor(shard, last_masterchain_state_);
  }

  std::map<BlockSeqno, WaitList<td::actor::Actor, td::Unit>> shard_client_waiters_;
  td::actor::ActorOwn<QueueSizeCounter> queue_size_counter_;

  td::Timestamp log_ls_stats_at_;
  std::map<int, td::uint32> ls_stats_;  // lite_api ID -> count, 0 for unknown

  UnixTime started_at_ = (UnixTime)td::Clocks::system();
  std::map<int, td::uint64> total_ls_queries_ok_, total_ls_queries_error_;  // lite_api ID -> count, 0 for unknown
  td::uint64 total_collated_blocks_master_ok_{0}, total_collated_blocks_master_error_{0};
  td::uint64 total_validated_blocks_master_ok_{0}, total_validated_blocks_master_error_{0};
  td::uint64 total_collated_blocks_shard_ok_{0}, total_collated_blocks_shard_error_{0};
  td::uint64 total_validated_blocks_shard_ok_{0}, total_validated_blocks_shard_error_{0};

  void log_collate_query_stats(CollationStats stats) override;
  void log_validate_query_stats(ValidationStats stats) override;

  void register_stats_provider(
      td::uint64 idx, std::string prefix,
      std::function<void(td::Promise<std::vector<std::pair<std::string, std::string>>>)> callback) override;
  void unregister_stats_provider(td::uint64 idx) override;

  void wait_verify_shard_blocks(std::vector<BlockIdExt> blocks, td::Promise<td::Unit> promise) override;

  void add_shard_block_retainer(adnl::AdnlNodeIdShort id) override;

  void iterate_temp_block_handles(std::function<void(const BlockHandleInterface &)> f) override;

  std::map<BlockSeqno, td::Ref<PersistentStateDescription>> persistent_state_descriptions_;
  std::map<BlockIdExt, td::Ref<PersistentStateDescription>> persistent_state_blocks_;

  std::map<td::uint64,
           std::pair<std::string, std::function<void(td::Promise<std::vector<std::pair<std::string, std::string>>>)>>>
      stats_providers_;

  td::actor::ActorOwn<StorageStatCache> storage_stat_cache_;

  template <typename T>
  void write_session_stats(const T &obj);

  td::actor::ActorOwn<ShardBlockVerifier> shard_block_verifier_;
  adnl::AdnlNodeIdShort shard_block_verifier_local_id_ = adnl::AdnlNodeIdShort::zero();
  std::map<adnl::AdnlNodeIdShort, td::actor::ActorOwn<ShardBlockRetainer>> shard_block_retainers_;

  void init_shard_block_verifier(adnl::AdnlNodeIdShort local_id);

  td::actor::ActorOwn<DbEventPublisher> db_event_publisher_;

  struct NonfinalGroupInfo {
    BlockIdExt last_accepted, last_candidate;
  };
  std::map<std::pair<ShardIdFull, CatchainSeqno>, NonfinalGroupInfo> nonfinal_info_;

  bool is_valid_nonfinal_group(ShardIdFull shard, CatchainSeqno cc_seqno);
  void process_accepted_nonfinal_block(BlockIdExt block_id, CatchainSeqno cc_seqno);
  void cleanup_nonfinal_groups();

  td::LRUCache<BlockIdExt, BlockReceiveStats> block_receive_stats_{1000};
  metrics::Labeled<metrics::Counter, WorkchainLabel, BlockSource> first_received_;
  metrics::Labeled<metrics::Counter, WorkchainLabel, BlockSource> received_;

  td::actor::Task<> collect(metrics::Context ctx) override;
  void update_block_receive_stats(BlockIdExt block_id, BlockSource type);
};

}  // namespace validator

}  // namespace ton
