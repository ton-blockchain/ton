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
#include "td/actor/PromiseFuture.h"
#include "td/utils/port/Poll.h"
#include "validator-group.hpp"
#include "shard-client.hpp"
#include "manager-init.h"
#include "state-serializer.hpp"
#include "rldp/rldp.h"
#include "token-manager.h"

#include <map>
#include <set>
#include <list>

namespace ton {

namespace validator {

class WaitBlockState;
class WaitZeroState;
class WaitShardState;
class WaitBlockData;

template <class MType>
struct MessageId {
  AccountIdPrefixFull dst;
  typename MType::Hash hash;

  bool operator<(const MessageId &msg) const {
    if (dst < msg.dst) {
      return true;
    }
    if (msg.dst < dst) {
      return false;
    }
    return hash < msg.hash;
  }
};

template <class MType>
class MessageExt {
 public:
  auto shard() const {
    return message_->shard();
  }
  auto ext_id() const {
    auto shard = message_->shard();
    return MessageId<MType>{shard, message_->hash()};
  }
  auto message() const {
    return message_;
  }
  auto hash() const {
    return message_->hash();
  }
  auto address() const {
    return std::make_pair(message_->wc(), message_->addr());
  }
  bool is_active() {
    if (!active_) {
      if (reactivate_at_.is_in_past()) {
        active_ = true;
        generation_++;
      }
    }
    return active_;
  }
  bool can_postpone() const {
    return generation_ <= 2;
  }
  void postpone() {
    if (!active_) {
      return;
    }
    active_ = false;
    reactivate_at_ = td::Timestamp::in(generation_ * 5.0);
  }
  bool expired() const {
    return delete_at_.is_in_past();
  }
  MessageExt(td::Ref<MType> msg) : message_(std::move(msg)) {
    delete_at_ = td::Timestamp::in(600);
  }

 private:
  td::Ref<MType> message_;
  td::uint32 generation_ = 0;
  bool active_ = true;
  td::Timestamp reactivate_at_;
  td::Timestamp delete_at_;
};

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
      td::Timestamp t = td::Timestamp::now();
      td::uint32 prio = 0;
      for (auto &v : waiting_) {
        if (v.timeout.at() > t.at()) {
          t = v.timeout;
        }
        if (v.priority > prio) {
          prio = v.priority;
        }
      }
      return {td::Timestamp::at(t.at() + 10.0), prio};
    }
    void check_timers() {
      td::uint32 j = 0;
      auto f = waiting_.begin();
      auto t = waiting_.end();
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
      waiting_.resize(j);
    }
  };
  std::map<BlockIdExt, WaitList<WaitBlockState, td::Ref<ShardState>>> wait_state_;
  std::map<BlockIdExt, WaitList<WaitBlockData, td::Ref<BlockData>>> wait_block_data_;

  struct WaitBlockHandle {
    std::vector<td::Promise<BlockHandle>> waiting_;
  };
  std::map<BlockIdExt, WaitBlockHandle> wait_block_handle_;

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
  std::map<ShardTopBlockDescriptionId, td::Ref<ShardTopBlockDescription>> shard_blocks_;
  std::map<MessageId<ExtMessage>, std::unique_ptr<MessageExt<ExtMessage>>> ext_messages_;
  std::map<std::pair<ton::WorkchainId,ton::StdSmcAddress>, std::map<ExtMessage::Hash, MessageId<ExtMessage>>> ext_addr_messages_;
  std::map<ExtMessage::Hash, MessageId<ExtMessage>> ext_messages_hashes_;
  // IHR ?
  std::map<MessageId<IhrMessage>, std::unique_ptr<MessageExt<IhrMessage>>> ihr_messages_;
  std::map<IhrMessage::Hash, MessageId<IhrMessage>> ihr_messages_hashes_;

 private:
  // VALIDATOR GROUPS
  ValidatorSessionId get_validator_set_id(ShardIdFull shard, td::Ref<ValidatorSet> val_set, td::Bits256 opts_hash,
                                          BlockSeqno last_key_block_seqno,
                                          const validatorsession::ValidatorSessionOptions &opts);
  td::actor::ActorOwn<ValidatorGroup> create_validator_group(ValidatorSessionId session_id, ShardIdFull shard,
                                                             td::Ref<ValidatorSet> validator_set,
                                                             validatorsession::ValidatorSessionOptions opts,
                                                             bool create_catchain);
  std::map<ValidatorSessionId, td::actor::ActorOwn<ValidatorGroup>> validator_groups_;
  std::map<ValidatorSessionId, td::actor::ActorOwn<ValidatorGroup>> next_validator_groups_;

  std::set<ValidatorSessionId> check_gc_list_;
  std::vector<ValidatorSessionId> gc_list_;

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

  BlockHandle gc_masterchain_handle_;
  td::Ref<MasterchainState> gc_masterchain_state_;
  bool gc_advancing_ = false;

  BlockIdExt last_rotate_block_id_;

  std::map<BlockSeqno, std::tuple<BlockHandle, td::Ref<MasterchainState>, std::vector<td::Promise<td::Unit>>>>
      pending_masterchain_states_;

  void new_masterchain_block();
  void update_shards();
  void update_shard_blocks();
  void written_destroyed_validator_sessions(std::vector<td::actor::ActorId<ValidatorGroup>> groups);
  void updated_init_block(BlockIdExt last_rotate_block_id) {
    last_rotate_block_id_ = last_rotate_block_id;
  }
  void got_next_gc_masterchain_handle(BlockHandle handle);
  void got_next_gc_masterchain_state(BlockHandle handle, td::Ref<MasterchainState> state);
  void advance_gc(BlockHandle handle, td::Ref<MasterchainState> state);
  void try_advance_gc_masterchain_block();
  void update_gc_block_handle(BlockHandle handle, td::Promise<td::Unit> promise) override;
  void update_shard_client_block_handle(BlockHandle handle, td::Promise<td::Unit> promise) override;

  bool out_of_sync();
  void applied_hardfork();
  void prestart_sync();
  void download_next_archive();
  void downloaded_archive_slice(std::string name, bool is_tmp);
  void checked_archive_slice(std::vector<BlockSeqno> seqno);
  void finish_prestart_sync();
  void completed_prestart_sync();

 public:
  void install_callback(std::unique_ptr<Callback> new_callback, td::Promise<td::Unit> promise) override {
    callback_ = std::move(new_callback);
    promise.set_value(td::Unit());
  }

  void add_permanent_key(PublicKeyHash key, td::Promise<td::Unit> promise) override {
    permanent_keys_.insert(key);
    promise.set_value(td::Unit());
  }
  void add_temp_key(PublicKeyHash key, td::Promise<td::Unit> promise) override {
    temp_keys_.insert(key);
    promise.set_value(td::Unit());
  }
  void del_permanent_key(PublicKeyHash key, td::Promise<td::Unit> promise) override {
    permanent_keys_.erase(key);
    promise.set_value(td::Unit());
  }
  void del_temp_key(PublicKeyHash key, td::Promise<td::Unit> promise) override {
    temp_keys_.erase(key);
    promise.set_value(td::Unit());
  }

  void validate_block_is_next_proof(BlockIdExt prev_block_id, BlockIdExt next_block_id, td::BufferSlice proof,
                                    td::Promise<td::Unit> promise) override;
  void validate_block_proof(BlockIdExt block_id, td::BufferSlice proof, td::Promise<td::Unit> promise) override;
  void validate_block_proof_link(BlockIdExt block_id, td::BufferSlice proof, td::Promise<td::Unit> promise) override;
  void validate_block_proof_rel(BlockIdExt block_id, BlockIdExt rel_block_id, td::BufferSlice proof,
                                td::Promise<td::Unit> promise) override;
  void validate_block(ReceivedBlock block, td::Promise<BlockHandle> promise) override;
  void prevalidate_block(BlockBroadcast broadcast, td::Promise<td::Unit> promise) override;

  //void create_validate_block(BlockId block, td::BufferSlice data, td::Promise<Block> promise) = 0;
  void sync_complete(td::Promise<td::Unit> promise) override;

  void get_next_block(BlockIdExt block_id, td::Promise<BlockHandle> promise) override;
  void get_next_key_blocks(BlockIdExt block_id, td::uint32 cnt, td::Promise<std::vector<BlockIdExt>> promise) override;
  void get_block_data(BlockHandle handle, td::Promise<td::BufferSlice> promise) override;
  void check_zero_state_exists(BlockIdExt block_id, td::Promise<bool> promise) override;
  void get_zero_state(BlockIdExt block_id, td::Promise<td::BufferSlice> promise) override;
  void check_persistent_state_exists(BlockIdExt block_id, BlockIdExt masterchain_block_id,
                                     td::Promise<bool> promise) override;
  void get_persistent_state(BlockIdExt block_id, BlockIdExt masterchain_block_id,
                            td::Promise<td::BufferSlice> promise) override;
  void get_persistent_state_slice(BlockIdExt block_id, BlockIdExt masterchain_block_id, td::int64 offset,
                                  td::int64 max_length, td::Promise<td::BufferSlice> promise) override;
  void get_block_proof(BlockHandle handle, td::Promise<td::BufferSlice> promise) override;
  void get_block_proof_link(BlockHandle block_id, td::Promise<td::BufferSlice> promise) override;
  void get_key_block_proof(BlockIdExt block_id, td::Promise<td::BufferSlice> promise) override;
  void get_key_block_proof_link(BlockIdExt block_id, td::Promise<td::BufferSlice> promise) override;
  //void get_block_description(BlockIdExt block_id, td::Promise<BlockDescription> promise) override;

  void new_external_message(td::BufferSlice data) override;
  void add_external_message(td::Ref<ExtMessage> message);
  void check_external_message(td::BufferSlice data, td::Promise<td::Unit> promise) override;

  void new_ihr_message(td::BufferSlice data) override;
  void new_shard_block(BlockIdExt block_id, CatchainSeqno cc_seqno, td::BufferSlice data) override;

  void add_ext_server_id(adnl::AdnlNodeIdShort id) override;
  void add_ext_server_port(td::uint16 port) override;
  void run_ext_query(td::BufferSlice data, td::Promise<td::BufferSlice> promise) override;

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

  void set_block_candidate(BlockIdExt id, BlockCandidate candidate, td::Promise<td::Unit> promise) override;

  void wait_block_state_merge(BlockIdExt left_id, BlockIdExt right_id, td::uint32 priority, td::Timestamp timeout,
                              td::Promise<td::Ref<ShardState>> promise) override;
  void wait_prev_block_state(BlockHandle handle, td::uint32 priority, td::Timestamp timeout,
                             td::Promise<td::Ref<ShardState>> promise) override;

  void wait_block_message_queue(BlockHandle handle, td::uint32 priority, td::Timestamp timeout,
                                td::Promise<td::Ref<MessageQueue>> promise) override;
  void wait_block_message_queue_short(BlockIdExt id, td::uint32 priority, td::Timestamp timeout,
                                      td::Promise<td::Ref<MessageQueue>> promise) override;
  void get_external_messages(ShardIdFull shard, td::Promise<std::vector<td::Ref<ExtMessage>>> promise) override;
  void get_ihr_messages(ShardIdFull shard, td::Promise<std::vector<td::Ref<IhrMessage>>> promise) override;
  void get_shard_blocks(BlockIdExt masterchain_block_id,
                        td::Promise<std::vector<td::Ref<ShardTopBlockDescription>>> promise) override;
  void complete_external_messages(std::vector<ExtMessage::Hash> to_delay,
                                  std::vector<ExtMessage::Hash> to_delete) override;
  void complete_ihr_messages(std::vector<IhrMessage::Hash> to_delay, std::vector<IhrMessage::Hash> to_delete) override;

  void set_next_block(BlockIdExt prev, BlockIdExt next, td::Promise<td::Unit> promise) override;

  void get_block_data_from_db(ConstBlockHandle handle, td::Promise<td::Ref<BlockData>> promise) override;
  void get_block_data_from_db_short(BlockIdExt block_id, td::Promise<td::Ref<BlockData>> promise) override;
  void get_shard_state_from_db(ConstBlockHandle handle, td::Promise<td::Ref<ShardState>> promise) override;
  void get_shard_state_from_db_short(BlockIdExt block_id, td::Promise<td::Ref<ShardState>> promise) override;
  void get_block_candidate_from_db(PublicKey source, BlockIdExt id, FileHash collated_data_file_hash,
                                   td::Promise<BlockCandidate> promise) override;
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
  void get_top_masterchain_state(td::Promise<td::Ref<MasterchainState>> promise) override;
  void get_top_masterchain_block(td::Promise<BlockIdExt> promise) override;
  void get_top_masterchain_state_block(td::Promise<std::pair<td::Ref<MasterchainState>, BlockIdExt>> promise) override;

  void send_get_block_request(BlockIdExt id, td::uint32 priority, td::Promise<ReceivedBlock> promise) override;
  void send_get_zero_state_request(BlockIdExt id, td::uint32 priority, td::Promise<td::BufferSlice> promise) override;
  void send_get_persistent_state_request(BlockIdExt id, BlockIdExt masterchain_block_id, td::uint32 priority,
                                         td::Promise<td::BufferSlice> promise) override;
  void send_get_block_proof_request(BlockIdExt block_id, td::uint32 priority,
                                    td::Promise<td::BufferSlice> promise) override;
  void send_get_block_proof_link_request(BlockIdExt block_id, td::uint32 priority,
                                         td::Promise<td::BufferSlice> promise) override;
  void send_get_next_key_blocks_request(BlockIdExt block_id, td::uint32 priority,
                                        td::Promise<std::vector<BlockIdExt>> promise) override;
  void send_external_message(td::Ref<ExtMessage> message) override;
  void send_ihr_message(td::Ref<IhrMessage> message) override;
  void send_top_shard_block_description(td::Ref<ShardTopBlockDescription> desc) override;
  void send_block_broadcast(BlockBroadcast broadcast) override;

  void update_shard_client_state(BlockIdExt masterchain_block_id, td::Promise<td::Unit> promise) override;
  void get_shard_client_state(bool from_db, td::Promise<BlockIdExt> promise) override;
  void subscribe_to_shard(ShardIdFull shard) override;

  void update_async_serializer_state(AsyncSerializerState state, td::Promise<td::Unit> promise) override;
  void get_async_serializer_state(td::Promise<AsyncSerializerState> promise) override;

  void try_get_static_file(FileHash file_hash, td::Promise<td::BufferSlice> promise) override;

  void get_download_token(size_t download_size, td::uint32 priority, td::Timestamp timeout,
                          td::Promise<std::unique_ptr<DownloadToken>> promise) override {
    td::actor::send_closure(token_manager_, &TokenManager::get_download_token, download_size, priority, timeout,
                            std::move(promise));
  }

  void get_archive_id(BlockSeqno masterchain_seqno, td::Promise<td::uint64> promise) override;
  void get_archive_slice(td::uint64 archive_id, td::uint64 offset, td::uint32 limit,
                         td::Promise<td::BufferSlice> promise) override;

  void check_is_hardfork(BlockIdExt block_id, td::Promise<bool> promise) override {
    CHECK(block_id.is_masterchain());
    promise.set_result(opts_->is_hardfork(block_id));
  }
  void get_vertical_seqno(BlockSeqno seqno, td::Promise<td::uint32> promise) override {
    promise.set_result(opts_->get_vertical_seqno(seqno));
  }

  void add_shard_block_description(td::Ref<ShardTopBlockDescription> desc);

  void register_block_handle(BlockHandle handle);

  void finished_wait_state(BlockHandle handle, td::Result<td::Ref<ShardState>> R);
  void finished_wait_data(BlockHandle handle, td::Result<td::Ref<BlockData>> R);

  void start_up() override;
  void started(ValidatorManagerInitResult result);
  void read_gc_list(std::vector<ValidatorSessionId> list);

  bool is_validator();
  PublicKeyHash get_validator(ShardIdFull shard, td::Ref<ValidatorSet> val_set);

  ValidatorManagerImpl(td::Ref<ValidatorManagerOptions> opts, std::string db_root,
                       td::actor::ActorId<keyring::Keyring> keyring, td::actor::ActorId<adnl::Adnl> adnl,
                       td::actor::ActorId<rldp::Rldp> rldp, td::actor::ActorId<overlay::Overlays> overlays)
      : opts_(std::move(opts)), db_root_(db_root), keyring_(keyring), adnl_(adnl), rldp_(rldp), overlays_(overlays) {
  }

 public:
  void allow_delete(BlockIdExt block_id, td::Promise<bool> promise);
  void allow_archive(BlockIdExt block_id, td::Promise<bool> promise);
  void allow_block_data_gc(BlockIdExt block_id, bool is_archive, td::Promise<bool> promise) override {
    allow_archive(block_id, std::move(promise));
  }
  void allow_block_state_gc(BlockIdExt block_id, td::Promise<bool> promise) override;
  void allow_zero_state_file_gc(BlockIdExt block_id, td::Promise<bool> promise) override {
    promise.set_result(false);
  }
  void allow_persistent_state_file_gc(BlockIdExt block_id, BlockIdExt masterchain_block_id,
                                      td::Promise<bool> promise) override;
  void allow_block_signatures_gc(BlockIdExt block_id, td::Promise<bool> promise) override {
    allow_archive(block_id, std::move(promise));
  }
  void allow_block_proof_gc(BlockIdExt block_id, bool is_archive, td::Promise<bool> promise) override {
    allow_archive(block_id, std::move(promise));
  }
  void allow_block_proof_link_gc(BlockIdExt block_id, bool is_archive, td::Promise<bool> promise) override {
    allow_archive(block_id, std::move(promise));
  }
  void allow_block_candidate_gc(BlockIdExt block_id, td::Promise<bool> promise) override {
    allow_block_state_gc(block_id, std::move(promise));
  }
  void allow_block_info_gc(BlockIdExt block_id, td::Promise<bool> promise) override;
  void archive(BlockHandle handle, td::Promise<td::Unit> promise) override {
    td::actor::send_closure(db_, &Db::archive, std::move(handle), std::move(promise));
  }

  void send_peek_key_block_request();
  void got_next_key_blocks(std::vector<BlockIdExt> vec);
  void update_last_known_key_block(BlockHandle handle, bool send_request) override;

  void prepare_stats(td::Promise<std::vector<std::pair<std::string, std::string>>> promise) override;

  void truncate(BlockSeqno seqno, ConstBlockHandle handle, td::Promise<td::Unit> promise) override;

  void wait_shard_client_state(BlockSeqno seqno, td::Timestamp timeout, td::Promise<td::Unit> promise) override;

 private:
  td::Timestamp resend_shard_blocks_at_;
  td::Timestamp check_waiters_at_;
  td::Timestamp check_shard_clients_;
  td::Timestamp log_status_at_;
  void alarm() override;
  std::map<ShardTopBlockDescriptionId, td::Ref<ShardTopBlockDescription>> out_shard_blocks_;

 private:
  td::actor::ActorOwn<TokenManager> token_manager_;

 private:
  std::set<PublicKeyHash> permanent_keys_;
  std::set<PublicKeyHash> temp_keys_;

 private:
  td::Ref<ValidatorManagerOptions> opts_;

 private:
  td::actor::ActorOwn<adnl::AdnlExtServer> lite_server_;
  td::actor::ActorOwn<LiteServerCache> lite_server_cache_;
  std::vector<td::uint16> pending_ext_ports_;
  std::vector<adnl::AdnlNodeIdShort> pending_ext_ids_;

  void created_ext_server(td::actor::ActorOwn<adnl::AdnlExtServer> lite_server);

 private:
  td::actor::ActorOwn<ShardClient> shard_client_;
  BlockSeqno min_confirmed_masterchain_seqno_{0};
  BlockSeqno state_serializer_masterchain_seqno_{0};

  void shard_client_update(BlockSeqno seqno);
  void state_serializer_update(BlockSeqno seqno);

  std::string db_root_;
  td::actor::ActorId<keyring::Keyring> keyring_;
  td::actor::ActorId<adnl::Adnl> adnl_;
  td::actor::ActorId<rldp::Rldp> rldp_;
  td::actor::ActorId<overlay::Overlays> overlays_;

  td::actor::ActorOwn<AsyncStateSerializer> serializer_;

  std::map<BlockSeqno, std::pair<std::string, bool>> to_import_;

 private:
  std::unique_ptr<Callback> callback_;
  td::actor::ActorOwn<Db> db_;

  bool started_ = false;
  bool allow_validate_ = false;

 private:
  double state_ttl() const {
    return opts_->state_ttl();
  }
  double block_ttl() const {
    return opts_->block_ttl();
  }
  double max_mempool_num() const {
    return opts_->max_mempool_num();
  }

 private:
  std::map<BlockSeqno, WaitList<td::actor::Actor, td::Unit>> shard_client_waiters_;
};

}  // namespace validator

}  // namespace ton
