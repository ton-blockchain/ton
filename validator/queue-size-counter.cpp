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
*/
#include "queue-size-counter.hpp"
#include "block/block-auto.h"
#include "block/block-parse.h"
#include "common/delay.h"
#include "td/actor/MultiPromise.h"
#include "td/utils/Random.h"

namespace ton::validator {

static td::Result<td::uint64> calc_queue_size(const td::Ref<ShardState> &state) {
  td::uint64 size = 0;
  TRY_RESULT(outq_descr, state->message_queue());
  block::gen::OutMsgQueueInfo::Record qinfo;
  if (!tlb::unpack_cell(outq_descr->root_cell(), qinfo)) {
    return td::Status::Error("invalid message queue");
  }
  vm::AugmentedDictionary queue{qinfo.out_queue->prefetch_ref(0), 352, block::tlb::aug_OutMsgQueue};
  bool ok = queue.check_for_each([&](td::Ref<vm::CellSlice>, td::ConstBitPtr, int) -> bool {
    ++size;
    return true;
  });
  if (!ok) {
    return td::Status::Error("invalid message queue dict");
  }
  return size;
}

static td::Result<td::uint64> recalc_queue_size(const td::Ref<ShardState> &state, const td::Ref<ShardState> &prev_state,
                                                td::uint64 prev_size) {
  TRY_RESULT(outq_descr, state->message_queue());
  block::gen::OutMsgQueueInfo::Record qinfo;
  if (!tlb::unpack_cell(outq_descr->root_cell(), qinfo)) {
    return td::Status::Error("invalid message queue");
  }
  vm::AugmentedDictionary queue{qinfo.out_queue->prefetch_ref(0), 352, block::tlb::aug_OutMsgQueue};

  TRY_RESULT(prev_outq_descr, prev_state->message_queue());
  block::gen::OutMsgQueueInfo::Record prev_qinfo;
  if (!tlb::unpack_cell(prev_outq_descr->root_cell(), prev_qinfo)) {
    return td::Status::Error("invalid message queue");
  }
  vm::AugmentedDictionary prev_queue{prev_qinfo.out_queue->prefetch_ref(0), 352, block::tlb::aug_OutMsgQueue};
  td::uint64 add = 0, rem = 0;
  bool ok = prev_queue.scan_diff(
      queue, [&](td::ConstBitPtr, int, td::Ref<vm::CellSlice> prev_val, td::Ref<vm::CellSlice> new_val) -> bool {
        if (prev_val.not_null()) {
          ++rem;
        }
        if (new_val.not_null()) {
          ++add;
        }
        return true;
      });
  if (!ok) {
    return td::Status::Error("invalid message queue dict");
  }
  if (prev_size + add < rem) {
    return td::Status::Error("negative value");
  }
  return prev_size + add - rem;
}

void QueueSizeCounter::start_up() {
  if (init_masterchain_state_.is_null()) {
    // Used in manager-hardfork or manager-disk
    simple_mode_ = true;
    return;
  }
  current_seqno_ = init_masterchain_state_->get_seqno();
  process_top_shard_blocks_cont(init_masterchain_state_, true);
  init_masterchain_state_ = {};
  alarm();
}

void QueueSizeCounter::get_queue_size(BlockIdExt block_id, td::Promise<td::uint64> promise) {
  get_queue_size_ex(block_id, simple_mode_ || is_block_too_old(block_id), std::move(promise));
}

void QueueSizeCounter::get_queue_size_ex(ton::BlockIdExt block_id, bool calc_whole, td::Promise<td::uint64> promise) {
  Entry &entry = results_[block_id];
  if (entry.done_) {
    promise.set_result(entry.queue_size_);
    return;
  }
  entry.promises_.push_back(std::move(promise));
  if (entry.started_) {
    return;
  }
  entry.started_ = true;
  entry.calc_whole_ = calc_whole;
  td::actor::send_closure(manager_, &ValidatorManager::get_block_handle, block_id, true,
                          [SelfId = actor_id(this), block_id, manager = manager_](td::Result<BlockHandle> R) mutable {
                            if (R.is_error()) {
                              td::actor::send_closure(SelfId, &QueueSizeCounter::on_error, block_id, R.move_as_error());
                              return;
                            }
                            BlockHandle handle = R.move_as_ok();
                            td::actor::send_closure(
                                manager, &ValidatorManager::wait_block_state, handle, 0, td::Timestamp::in(10.0),
                                [SelfId, handle](td::Result<td::Ref<ShardState>> R) mutable {
                                  if (R.is_error()) {
                                    td::actor::send_closure(SelfId, &QueueSizeCounter::on_error, handle->id(),
                                                            R.move_as_error());
                                    return;
                                  }
                                  td::actor::send_closure(SelfId, &QueueSizeCounter::get_queue_size_cont,
                                                          std::move(handle), R.move_as_ok());
                                });
                          });
}

void QueueSizeCounter::get_queue_size_cont(BlockHandle handle, td::Ref<ShardState> state) {
  Entry &entry = results_[handle->id()];
  CHECK(entry.started_);
  bool calc_whole = entry.calc_whole_ || handle->id().seqno() == 0;
  if (!calc_whole) {
    CHECK(handle->inited_prev());
    auto prev_blocks = handle->prev();
    bool after_split = prev_blocks.size() == 1 && handle->id().shard_full() != prev_blocks[0].shard_full();
    bool after_merge = prev_blocks.size() == 2;
    calc_whole = after_split || after_merge;
  }
  if (calc_whole) {
    auto r_size = calc_queue_size(state);
    if (r_size.is_error()) {
      on_error(handle->id(), r_size.move_as_error());
      return;
    }
    entry.done_ = true;
    entry.queue_size_ = r_size.move_as_ok();
    for (auto &promise : entry.promises_) {
      promise.set_result(entry.queue_size_);
    }
    entry.promises_.clear();
    return;
  }

  auto prev_block_id = handle->one_prev(true);
  get_queue_size(prev_block_id, [=, SelfId = actor_id(this), manager = manager_](td::Result<td::uint64> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &QueueSizeCounter::on_error, state->get_block_id(), R.move_as_error());
      return;
    }
    td::uint64 prev_size = R.move_as_ok();
    td::actor::send_closure(
        manager, &ValidatorManager::wait_block_state_short, prev_block_id, 0, td::Timestamp::in(10.0),
        [=](td::Result<td::Ref<ShardState>> R) {
          if (R.is_error()) {
            td::actor::send_closure(SelfId, &QueueSizeCounter::on_error, state->get_block_id(), R.move_as_error());
            return;
          }
          td::actor::send_closure(SelfId, &QueueSizeCounter::get_queue_size_cont2, state, R.move_as_ok(), prev_size);
        });
  });
}

void QueueSizeCounter::get_queue_size_cont2(td::Ref<ShardState> state, td::Ref<ShardState> prev_state,
                                            td::uint64 prev_size) {
  BlockIdExt block_id = state->get_block_id();
  Entry &entry = results_[block_id];
  CHECK(entry.started_);
  auto r_size = recalc_queue_size(state, prev_state, prev_size);
  if (r_size.is_error()) {
    on_error(block_id, r_size.move_as_error());
    return;
  }
  entry.done_ = true;
  entry.queue_size_ = r_size.move_as_ok();
  for (auto &promise : entry.promises_) {
    promise.set_result(entry.queue_size_);
  }
  entry.promises_.clear();
}

void QueueSizeCounter::on_error(ton::BlockIdExt block_id, td::Status error) {
  auto it = results_.find(block_id);
  if (it == results_.end()) {
    return;
  }
  Entry &entry = it->second;
  CHECK(!entry.done_);
  for (auto &promise : entry.promises_) {
    promise.set_error(error.clone());
  }
  results_.erase(it);
}

void QueueSizeCounter::process_top_shard_blocks() {
  LOG(DEBUG) << "QueueSizeCounter::process_top_shard_blocks seqno=" << current_seqno_;
  td::actor::send_closure(
      manager_, &ValidatorManager::get_block_by_seqno_from_db, AccountIdPrefixFull{masterchainId, 0}, current_seqno_,
      [SelfId = actor_id(this), manager = manager_](td::Result<ConstBlockHandle> R) {
        if (R.is_error()) {
          LOG(WARNING) << "Failed to get masterchain block id: " << R.move_as_error();
          delay_action([=]() { td::actor::send_closure(SelfId, &QueueSizeCounter::process_top_shard_blocks); },
                       td::Timestamp::in(5.0));
          return;
        }
        td::actor::send_closure(
            manager, &ValidatorManager::wait_block_state_short, R.ok()->id(), 0, td::Timestamp::in(10.0),
            [=](td::Result<td::Ref<ShardState>> R) {
              if (R.is_error()) {
                LOG(WARNING) << "Failed to get masterchain state: " << R.move_as_error();
                delay_action([=]() { td::actor::send_closure(SelfId, &QueueSizeCounter::process_top_shard_blocks); },
                             td::Timestamp::in(5.0));
                return;
              }
              td::actor::send_closure(SelfId, &QueueSizeCounter::process_top_shard_blocks_cont,
                                      td::Ref<MasterchainState>(R.move_as_ok()), false);
            });
      });
}

void QueueSizeCounter::process_top_shard_blocks_cont(td::Ref<MasterchainState> state, bool init) {
  LOG(DEBUG) << "QueueSizeCounter::process_top_shard_blocks_cont seqno=" << current_seqno_ << " init=" << init;
  td::MultiPromise mp;
  auto ig = mp.init_guard();
  last_top_blocks_.clear();
  last_top_blocks_.push_back(state->get_block_id());
  for (auto &shard : state->get_shards()) {
    if (opts_->need_monitor(shard->shard(), state)) {
      last_top_blocks_.push_back(shard->top_block_id());
    }
  }
  for (const BlockIdExt &block_id : last_top_blocks_) {
    get_queue_size_ex_retry(block_id, init, ig.get_promise());
  }
  ig.add_promise([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    if (R.is_error()) {
      return;
    }
    td::actor::send_closure(SelfId, &QueueSizeCounter::process_top_shard_blocks_finish);
  });
  if (init) {
    init_top_blocks_ = last_top_blocks_;
  }
}

void QueueSizeCounter::get_queue_size_ex_retry(BlockIdExt block_id, bool calc_whole, td::Promise<td::Unit> promise) {
  get_queue_size_ex(block_id, calc_whole,
                    [=, promise = std::move(promise), SelfId = actor_id(this)](td::Result<td::uint64> R) mutable {
                      if (R.is_error()) {
                        LOG(WARNING) << "Failed to calculate queue size for block " << block_id.to_str() << ": "
                                     << R.move_as_error();
                        delay_action(
                            [=, promise = std::move(promise)]() mutable {
                              td::actor::send_closure(SelfId, &QueueSizeCounter::get_queue_size_ex_retry, block_id,
                                                      calc_whole, std::move(promise));
                            },
                            td::Timestamp::in(5.0));
                        return;
                      }
                      promise.set_result(td::Unit());
                    });
}

void QueueSizeCounter::process_top_shard_blocks_finish() {
  ++current_seqno_;
  wait_shard_client();
}

void QueueSizeCounter::wait_shard_client() {
  LOG(DEBUG) << "QueueSizeCounter::wait_shard_client seqno=" << current_seqno_;
  td::actor::send_closure(
      manager_, &ValidatorManager::wait_shard_client_state, current_seqno_, td::Timestamp::in(60.0),
      [SelfId = actor_id(this)](td::Result<td::Unit> R) {
        if (R.is_error()) {
          delay_action([=]() mutable { td::actor::send_closure(SelfId, &QueueSizeCounter::wait_shard_client); },
                       td::Timestamp::in(5.0));
          return;
        }
        td::actor::send_closure(SelfId, &QueueSizeCounter::process_top_shard_blocks);
      });
}

void QueueSizeCounter::alarm() {
  for (auto it = results_.begin(); it != results_.end();) {
    if (it->second.done_ && is_block_too_old(it->first)) {
      it = results_.erase(it);
    } else {
      ++it;
    }
  }
  alarm_timestamp() = td::Timestamp::in(td::Random::fast(20.0, 40.0));
}

}  // namespace ton::validator