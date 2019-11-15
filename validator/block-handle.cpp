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
#include "block-handle.hpp"
#include "adnl/utils.hpp"
#include "ton/ton-tl.hpp"

namespace ton {

namespace validator {

void BlockHandleImpl::flush(td::actor::ActorId<ValidatorManagerInterface> manager, BlockHandle self,
                            td::Promise<td::Unit> promise) {
  td::actor::send_closure(manager, &ValidatorManager::write_handle, self, std::move(promise));
}

td::BufferSlice BlockHandleImpl::serialize() const {
  while (locked()) {
  }
  auto flags = flags_.load(std::memory_order_consume) & ~(Flags::dbf_processed | Flags::dbf_moved_handle);
  return create_serialize_tl_object<ton_api::db_block_info>(
      create_tl_block_id(id_), flags, (flags & dbf_inited_prev_left) ? create_tl_block_id(prev_[0]) : nullptr,
      (flags & dbf_inited_prev_right) ? create_tl_block_id(prev_[1]) : nullptr,
      (flags & dbf_inited_next_left) ? create_tl_block_id(next_[0]) : nullptr,
      (flags & dbf_inited_next_right) ? create_tl_block_id(next_[1]) : nullptr, (flags & dbf_inited_lt) ? lt_ : 0,
      (flags & dbf_inited_ts) ? ts_ : 0, (flags & dbf_inited_state) ? state_ : RootHash::zero(),
      (flags & dbf_inited_masterchain_ref_block) ? masterchain_ref_seqno_ : 0);
}

BlockHandleImpl::BlockHandleImpl(td::BufferSlice data) {
  auto obj = fetch_tl_object<ton_api::db_block_info>(std::move(data), true).move_as_ok();
  flags_ = obj->flags_ & ~(Flags::dbf_processed | Flags::dbf_moved_handle);
  id_ = create_block_id(obj->id_);
  prev_[0] = (flags_ & dbf_inited_prev_left) ? create_block_id(obj->prev_left_) : BlockIdExt{};
  prev_[1] = (flags_ & dbf_inited_prev_right) ? create_block_id(obj->prev_right_) : BlockIdExt{};
  next_[0] = (flags_ & dbf_inited_next_left) ? create_block_id(obj->next_left_) : BlockIdExt{};
  next_[1] = (flags_ & dbf_inited_next_right) ? create_block_id(obj->next_right_) : BlockIdExt{};
  lt_ = (flags_ & dbf_inited_lt) ? obj->lt_ : 0;
  ts_ = (flags_ & dbf_inited_ts) ? obj->ts_ : 0;
  state_ = (flags_ & dbf_inited_state) ? obj->state_ : RootHash::zero();
  masterchain_ref_seqno_ =
      (flags_ & dbf_inited_masterchain_ref_block) ? static_cast<BlockSeqno>(obj->masterchain_ref_seqno_) : 0;
  get_thread_safe_counter().add(1);
}

}  // namespace validator

}  // namespace ton
