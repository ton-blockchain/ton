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
#pragma once

#include "ton/ton-types.h"
#include "td/actor/PromiseFuture.h"
#include "td/actor/actor.h"

namespace ton {

namespace validator {

class ValidatorManagerInterface;

struct BlockHandleInterface {
 public:
  virtual BlockIdExt id() const = 0;
  virtual bool received() const = 0;
  virtual bool moved_to_archive() const = 0;
  virtual bool handle_moved_to_archive() const = 0;
  virtual bool deleted() const = 0;
  virtual bool inited_next_left() const = 0;
  virtual bool inited_next_right() const = 0;
  virtual bool inited_next() const = 0;
  virtual bool inited_prev_left() const = 0;
  virtual bool inited_prev_right() const = 0;
  virtual bool inited_prev() const = 0;
  virtual bool inited_logical_time() const = 0;
  virtual bool inited_unix_time() const = 0;
  virtual bool inited_proof() const = 0;
  virtual bool inited_proof_link() const = 0;
  virtual bool inited_signatures() const = 0;
  virtual bool inited_split_after() const = 0;
  virtual bool inited_merge_before() const = 0;
  virtual bool inited_is_key_block() const = 0;
  virtual bool inited_masterchain_ref_block() const = 0;
  virtual bool split_after() const = 0;
  virtual bool merge_before() const = 0;
  virtual bool is_key_block() const = 0;
  virtual bool inited_state_root_hash() const = 0;
  virtual bool received_state() const = 0;
  virtual bool inited_state_boc() const = 0;
  virtual bool deleted_state_boc() const = 0;
  virtual bool need_flush() const = 0;
  virtual bool is_zero() const = 0;
  virtual bool is_archived() const = 0;
  virtual bool is_applied() const = 0;
  virtual BlockSeqno masterchain_ref_block() const = 0;
  virtual std::vector<BlockIdExt> prev() const = 0;
  virtual BlockIdExt one_prev(bool left) const = 0;
  virtual std::vector<BlockIdExt> next() const = 0;
  virtual BlockIdExt one_next(bool left) const = 0;
  virtual RootHash state() const = 0;
  virtual td::uint32 version() const = 0;

  virtual bool processed() const = 0;
  virtual void set_processed() = 0;

  virtual void flush(td::actor::ActorId<ValidatorManagerInterface> manager, std::shared_ptr<BlockHandleInterface> self,
                     td::Promise<td::Unit> promise) = 0;
  virtual void flushed_upto(td::uint32 version) = 0;
  virtual void set_logical_time(LogicalTime lt) = 0;
  virtual void set_unix_time(UnixTime ts) = 0;
  virtual LogicalTime logical_time() const = 0;
  virtual UnixTime unix_time() const = 0;
  virtual void set_proof() = 0;
  virtual void set_proof_link() = 0;
  virtual void set_signatures() = 0;
  virtual void set_next(BlockIdExt next) = 0;
  virtual void set_prev(BlockIdExt prev) = 0;
  virtual void set_received() = 0;
  virtual void set_moved_to_archive() = 0;
  virtual void set_handle_moved_to_archive() = 0;
  virtual void set_deleted() = 0;
  virtual void set_split(bool value) = 0;
  virtual void set_merge(bool value) = 0;
  virtual void set_is_key_block(bool value) = 0;
  virtual void set_state_root_hash(RootHash hash) = 0;
  virtual void set_state_boc() = 0;
  virtual void set_deleted_state_boc() = 0;
  virtual void set_archived() = 0;
  virtual void set_applied() = 0;
  virtual void set_masterchain_ref_block(BlockSeqno seqno) = 0;

  virtual void unsafe_clear_applied() = 0;
  virtual void unsafe_clear_next() = 0;

  virtual td::BufferSlice serialize() const = 0;

  virtual ~BlockHandleInterface() = default;
};

using BlockHandle = std::shared_ptr<BlockHandleInterface>;
using ConstBlockHandle = std::shared_ptr<const BlockHandleInterface>;

}  // namespace validator

}  // namespace ton
