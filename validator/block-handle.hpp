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

#include "interfaces/block-handle.h"
#include "ton/ton-shard.h"
#include "td/actor/actor.h"
#include "interfaces/validator-manager.h"
#include "ton/ton-io.hpp"

#include "td/utils/ThreadSafeCounter.h"

namespace ton {

namespace validator {

class ValidatorManager;

struct BlockHandleImpl : public BlockHandleInterface {
 private:
  enum Flags : td::uint32 {
    dbf_masterchain = 0x1,
    dbf_inited_prev_left = 0x2,
    dbf_inited_prev_right = 0x4,
    dbf_inited_next_left = 0x8,
    dbf_inited_next_right = 0x10,
    dbf_inited_split_after = 0x20,
    dbf_split_after = 0x40,
    dbf_inited_merge_before = 0x80,
    dbf_merge_before = 0x100,
    dbf_received = 0x200,
    dbf_is_key_block = 0x400,
    dbf_inited_proof = 0x800,
    dbf_inited_proof_link = 0x1000,
    dbf_inited_lt = 0x2000,
    dbf_inited_ts = 0x4000,
    dbf_inited_is_key_block = 0x8000,
    dbf_inited_state = 0x20000,
    dbf_inited_signatures = 0x40000,
    dbf_inited_state_boc = 0x100000,
    dbf_archived = 0x200000,
    dbf_applied = 0x400000,
    dbf_inited_masterchain_ref_block = 0x800000,
    dbf_deleted = 0x2000000,
    dbf_deleted_boc = 0x4000000,
    dbf_moved_new = 0x8000000,
    dbf_processed = 0x10000000,
    dbf_moved_handle = 0x20000000,
  };

  std::atomic<td::uint64> version_{0};
  std::atomic<td::uint32> written_version_{0};
  BlockIdExt id_;
  std::atomic<td::uint32> flags_{0};
  std::array<BlockIdExt, 2> prev_;
  std::array<BlockIdExt, 2> next_;
  LogicalTime lt_;
  UnixTime ts_;
  RootHash state_;
  BlockSeqno masterchain_ref_seqno_;

  static constexpr td::uint64 lock_const() {
    return static_cast<td::uint64>(1) << 32;
  }
  bool locked() const {
    return version_.load(std::memory_order_consume) >> 32;
  }
  void lock() {
    version_ += 1 + lock_const();
  }
  void unlock() {
    version_ -= lock_const();
  }

 public:
  BlockIdExt id() const override {
    return id_;
  }
  bool received() const override {
    return flags_.load(std::memory_order_consume) & Flags::dbf_received;
  }
  bool moved_to_archive() const override {
    return flags_.load(std::memory_order_consume) & Flags::dbf_moved_new;
  }
  bool handle_moved_to_archive() const override {
    return flags_.load(std::memory_order_consume) & Flags::dbf_moved_handle;
  }
  bool deleted() const override {
    return flags_.load(std::memory_order_consume) & Flags::dbf_deleted;
  }
  bool inited_next_left() const override {
    return flags_.load(std::memory_order_consume) & Flags::dbf_inited_next_left;
  }
  bool inited_next_right() const override {
    return flags_.load(std::memory_order_consume) & Flags::dbf_inited_next_right;
  }
  bool inited_next() const override {
    auto f = flags_.load(std::memory_order_consume);
    if (!(f & Flags::dbf_inited_next_left)) {
      return false;
    }
    if (f & Flags::dbf_inited_next_right) {
      return true;
    }
    if ((f & Flags::dbf_inited_split_after) && !(f & Flags::dbf_split_after)) {
      return true;
    }
    return false;
  }
  bool inited_prev_left() const override {
    return flags_.load(std::memory_order_consume) & Flags::dbf_inited_prev_left;
  }
  bool inited_prev_right() const override {
    return flags_.load(std::memory_order_consume) & Flags::dbf_inited_prev_right;
  }
  bool inited_prev() const override {
    auto f = flags_.load(std::memory_order_consume);
    if (!(f & Flags::dbf_inited_prev_left)) {
      return false;
    }
    if (f & Flags::dbf_inited_prev_right) {
      return true;
    }
    if ((f & Flags::dbf_inited_merge_before) && !(f & Flags::dbf_merge_before)) {
      return true;
    }
    return false;
  }
  bool inited_proof() const override {
    return flags_.load(std::memory_order_consume) & Flags::dbf_inited_proof;
  }
  bool inited_proof_link() const override {
    return flags_.load(std::memory_order_consume) & Flags::dbf_inited_proof_link;
  }
  bool inited_signatures() const override {
    return flags_.load(std::memory_order_consume) & Flags::dbf_inited_signatures;
  }
  bool inited_split_after() const override {
    return flags_.load(std::memory_order_consume) & Flags::dbf_inited_split_after;
  }
  bool inited_merge_before() const override {
    return flags_.load(std::memory_order_consume) & Flags::dbf_inited_merge_before;
  }
  bool inited_is_key_block() const override {
    auto f = flags_.load(std::memory_order_consume);
    return f & Flags::dbf_inited_is_key_block;
  }
  bool split_after() const override {
    auto f = flags_.load(std::memory_order_consume);
    CHECK(f & Flags::dbf_inited_split_after);
    return f & Flags::dbf_split_after;
  }
  bool merge_before() const override {
    auto f = flags_.load(std::memory_order_consume);
    CHECK(f & Flags::dbf_inited_merge_before);
    return f & Flags::dbf_merge_before;
  }
  bool is_key_block() const override {
    auto f = flags_.load(std::memory_order_consume);
    CHECK(f & Flags::dbf_inited_is_key_block);
    return f & Flags::dbf_is_key_block;
  }

  bool inited_state_root_hash() const override {
    return flags_.load(std::memory_order_consume) & Flags::dbf_inited_state;
  }
  bool inited_state_boc() const override {
    return flags_.load(std::memory_order_consume) & Flags::dbf_inited_state_boc;
  }
  bool deleted_state_boc() const override {
    return flags_.load(std::memory_order_consume) & Flags::dbf_deleted_boc;
  }
  bool received_state() const override {
    return flags_.load(std::memory_order_consume) & Flags::dbf_inited_state_boc;
  }
  bool need_flush() const override {
    return written_version_.load(std::memory_order_consume) < version();
  }
  bool is_zero() const override {
    return id_.id.seqno == 0;
  }
  bool is_archived() const override {
    return flags_.load(std::memory_order_consume) & Flags::dbf_archived;
  }
  bool is_applied() const override {
    return flags_.load(std::memory_order_consume) & Flags::dbf_applied;
  }
  bool inited_masterchain_ref_block() const override {
    return id_.is_masterchain() || (flags_.load(std::memory_order_consume) & Flags::dbf_inited_masterchain_ref_block);
  }
  BlockSeqno masterchain_ref_block() const override {
    CHECK(inited_masterchain_ref_block());
    return id_.is_masterchain() ? id_.seqno() : masterchain_ref_seqno_;
  }
  std::vector<BlockIdExt> prev() const override {
    if (is_zero()) {
      return {};
    }
    auto f = flags_.load(std::memory_order_consume);
    CHECK(f & Flags::dbf_inited_merge_before);
    if (!(f & Flags::dbf_merge_before)) {
      CHECK(f & Flags::dbf_inited_prev_left);
      return {prev_[0]};
    } else {
      CHECK(f & Flags::dbf_inited_prev_left);
      CHECK(f & Flags::dbf_inited_prev_right);
      return {prev_[0], prev_[1]};
    }
  }
  BlockIdExt one_prev(bool left) const override {
    CHECK(!is_zero());
    if (left) {
      CHECK(flags_.load(std::memory_order_consume) & Flags::dbf_inited_prev_left);
    } else {
      CHECK(flags_.load(std::memory_order_consume) & Flags::dbf_inited_prev_right);
    }
    return prev_[left ? 0 : 1];
  }
  std::vector<BlockIdExt> next() const override {
    auto f = flags_.load(std::memory_order_consume);
    CHECK(f & Flags::dbf_inited_split_after);
    if (!(f & Flags::dbf_split_after)) {
      CHECK(f & Flags::dbf_inited_next_left);
      return {next_[0]};
    } else {
      CHECK(f & Flags::dbf_inited_next_left);
      CHECK(f & Flags::dbf_inited_next_right);
      return {next_[0], next_[1]};
    }
  }
  BlockIdExt one_next(bool left) const override {
    if (left) {
      CHECK(flags_.load(std::memory_order_consume) & Flags::dbf_inited_next_left);
    } else {
      CHECK(flags_.load(std::memory_order_consume) & Flags::dbf_inited_next_right);
    }
    return next_[left ? 0 : 1];
  }
  RootHash state() const override {
    CHECK(flags_.load(std::memory_order_consume) & Flags::dbf_inited_state);
    return state_;
  }

  bool processed() const override {
    return flags_.load(std::memory_order_consume) & Flags::dbf_processed;
  }
  void set_processed() override {
    // does not increase version
    flags_ |= Flags::dbf_processed;
  }

  td::uint32 version() const override {
    return static_cast<td::uint32>(version_.load(std::memory_order_consume));
  }
  void flush(td::actor::ActorId<ValidatorManagerInterface> manager, BlockHandle self,
             td::Promise<td::Unit> promise) override;
  void flushed_upto(td::uint32 version) override {
    if (version > written_version_.load(std::memory_order_consume)) {
      written_version_.store(version, std::memory_order_release);
    }
  }
  bool inited_logical_time() const override {
    return flags_.load(std::memory_order_consume) & Flags::dbf_inited_lt;
  }
  LogicalTime logical_time() const override {
    CHECK(inited_logical_time());
    return lt_;
  }
  void set_logical_time(LogicalTime lt) override {
    if (inited_logical_time()) {
      CHECK(lt_ == lt);
    } else {
      lock();
      lt_ = lt;
      flags_ |= Flags::dbf_inited_lt;
      unlock();
    }
  }
  bool inited_unix_time() const override {
    return flags_.load(std::memory_order_consume) & Flags::dbf_inited_ts;
  }
  UnixTime unix_time() const override {
    CHECK(inited_unix_time());
    return ts_;
  }
  void set_unix_time(UnixTime ts) override {
    if (inited_unix_time()) {
      CHECK(ts_ == ts);
    } else {
      lock();
      ts_ = ts;
      flags_ |= Flags::dbf_inited_ts;
      unlock();
    }
  }
  void set_proof() override {
    if (!inited_proof()) {
      lock();
      flags_ |= Flags::dbf_inited_proof;
      unlock();
    }
  }
  void set_proof_link() override {
    if (!inited_proof_link()) {
      lock();
      flags_ |= Flags::dbf_inited_proof_link;
      unlock();
    }
  }
  void set_signatures() override {
    if (!inited_signatures()) {
      lock();
      flags_ |= Flags::dbf_inited_signatures;
      unlock();
    }
  }
  void set_next_left(BlockIdExt next) {
    auto f = flags_.load(std::memory_order_consume);
    if (f & Flags::dbf_inited_next_left) {
      LOG_CHECK(next_[0] == next) << "id=" << id_ << " next=" << next_[0] << " to_be_next=" << next;
    } else {
      lock();
      next_[0] = next;
      flags_ |= Flags::dbf_inited_next_left;
      unlock();
    }
  }
  void set_next_right(BlockIdExt next) {
    auto f = flags_.load(std::memory_order_consume);
    if (f & Flags::dbf_inited_next_right) {
      LOG_CHECK(next_[1] == next) << "id=" << id_ << " next=" << next_[1] << " to_be_next=" << next;
    } else {
      lock();
      next_[1] = next;
      flags_ |= Flags::dbf_inited_next_right;
      unlock();
    }
  }
  void set_next(BlockIdExt next) override {
    bool right = shard_child(id_.id.shard, false) == next.id.shard;
    if (right) {
      set_next_right(next);
    } else {
      set_next_left(next);
    }
  }
  void set_prev_left(BlockIdExt prev) {
    auto f = flags_.load(std::memory_order_consume);
    if (f & Flags::dbf_inited_prev_left) {
      LOG_CHECK(prev_[0] == prev) << "id=" << id_ << " prev=" << prev_[0] << " to_be_prev=" << prev;
    } else {
      lock();
      prev_[0] = prev;
      flags_ |= Flags::dbf_inited_prev_left;
      unlock();
    }
  }
  void set_prev_right(BlockIdExt prev) {
    auto f = flags_.load(std::memory_order_consume);
    if (f & Flags::dbf_inited_prev_right) {
      LOG_CHECK(prev_[1] == prev) << "id=" << id_ << " prev=" << prev_[1] << " to_be_prev=" << prev;
    } else {
      lock();
      prev_[1] = prev;
      flags_ |= Flags::dbf_inited_prev_right;
      unlock();
    }
  }
  void set_prev(BlockIdExt prev) override {
    bool right = shard_child(id_.id.shard, false) == prev.id.shard;
    if (right) {
      set_prev_right(prev);
    } else {
      set_prev_left(prev);
    }
  }
  void set_received() override {
    if (flags_.load(std::memory_order_consume) & Flags::dbf_received) {
      return;
    }
    lock();
    flags_ |= Flags::dbf_received;
    unlock();
  }
  void set_moved_to_archive() override {
    if (flags_.load(std::memory_order_consume) & Flags::dbf_moved_new) {
      return;
    }
    lock();
    flags_ |= Flags::dbf_moved_new;
    unlock();
  }
  void set_handle_moved_to_archive() override {
    flags_ |= Flags::dbf_moved_handle;
  }
  void set_deleted() override {
    if (flags_.load(std::memory_order_consume) & Flags::dbf_deleted) {
      return;
    }
    lock();
    flags_ |= Flags::dbf_deleted;
    unlock();
  }
  void set_split(bool value) override {
    td::uint32 v = value ? static_cast<td::uint32>(Flags::dbf_split_after) : 0;
    auto f = flags_.load(std::memory_order_consume);
    if (f & Flags::dbf_inited_split_after) {
      CHECK((f & Flags::dbf_split_after) == v);
    } else {
      lock();
      flags_ |= v | Flags::dbf_inited_split_after;
      unlock();
    }
  }
  void set_merge(bool value) override {
    td::uint32 v = value ? static_cast<td::uint32>(Flags::dbf_merge_before) : 0;
    auto f = flags_.load(std::memory_order_consume);
    if (f & Flags::dbf_inited_merge_before) {
      CHECK((f & Flags::dbf_merge_before) == v);
    } else {
      lock();
      flags_ |= v | Flags::dbf_inited_merge_before;
      unlock();
    }
  }
  void set_is_key_block(bool value) override {
    td::uint32 v = value ? static_cast<td::uint32>(Flags::dbf_is_key_block) : 0;
    auto f = flags_.load(std::memory_order_consume);
    if (f & Flags::dbf_inited_is_key_block) {
      CHECK((f & Flags::dbf_is_key_block) == v);
    } else {
      lock();
      flags_ |= v | Flags::dbf_inited_is_key_block;
      unlock();
    }
  }
  void set_state_root_hash(RootHash hash) override {
    if (!(flags_.load(std::memory_order_consume) & Flags::dbf_inited_state)) {
      lock();
      state_ = hash;
      flags_ |= Flags::dbf_inited_state;
      unlock();
    }
  }
  void set_state_boc() override {
    if (!inited_state_boc()) {
      CHECK(inited_state_root_hash());
      lock();
      flags_ |= Flags::dbf_inited_state_boc;
      unlock();
    }
  }
  void set_deleted_state_boc() override {
    if (flags_.load(std::memory_order_consume) & Flags::dbf_deleted_boc) {
      return;
    }
    lock();
    flags_ |= Flags::dbf_deleted_boc;
    unlock();
  }
  void set_archived() override {
    if (!is_archived()) {
      lock();
      flags_ |= Flags::dbf_archived;
      unlock();
    }
  }
  void set_applied() override {
    if (!is_applied()) {
      lock();
      flags_ |= Flags::dbf_applied;
      unlock();
    }
  }
  void set_masterchain_ref_block(BlockSeqno seqno) override {
    if (!inited_masterchain_ref_block()) {
      lock();
      masterchain_ref_seqno_ = seqno;
      flags_ |= Flags::dbf_inited_masterchain_ref_block;
      unlock();
    }
  }

  void unsafe_clear_applied() override {
    if (is_applied()) {
      lock();
      flags_ &= ~Flags::dbf_applied;
      unlock();
    }
  }
  void unsafe_clear_next() override {
    if (inited_next_left() || inited_next_right()) {
      lock();
      flags_ &= ~(Flags::dbf_inited_next_left | Flags::dbf_inited_next_right);
      unlock();
    }
  }

  td::BufferSlice serialize() const override;
  BlockHandleImpl(BlockIdExt id)
      : id_(id), flags_(id_.is_masterchain() ? static_cast<td::uint32>(dbf_masterchain) : 0) {
    get_thread_safe_counter().add(1);
  }
  BlockHandleImpl(td::BufferSlice data);
  ~BlockHandleImpl() {
    LOG_CHECK(!need_flush()) << "flags=" << flags_;
    get_thread_safe_counter().add(-1);
  }

  static td::NamedThreadSafeCounter::CounterRef get_thread_safe_counter() {
    static auto res = td::NamedThreadSafeCounter::get_default().get_counter("BlockHandleImpl");
    return res;
  }

  static BlockHandle create_empty(BlockIdExt id) {
    return std::make_shared<BlockHandleImpl>(id);
  }

  static BlockHandle create(td::BufferSlice data) {
    return std::make_shared<BlockHandleImpl>(std::move(data));
  }
};

}  // namespace validator

}  // namespace ton
