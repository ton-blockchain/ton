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
#include "candidates-buffer.hpp"
#include "fabric.h"

namespace ton::validator {

void CandidatesBuffer::start_up() {
  alarm_timestamp() = td::Timestamp::in(60.0);
}

void CandidatesBuffer::alarm() {
  alarm_timestamp() = td::Timestamp::in(60.0);
  for (auto it = candidates_.begin(); it != candidates_.end();) {
    Candidate &entry = it->second;
    if (entry.ttl_.is_in_past()) {
      for (auto &p : entry.data_waiters_) {
        p.set_error(td::Status::Error(ErrorCode::timeout, "timeout"));
      }
      for (auto &p : entry.state_waiters_) {
        p.set_error(td::Status::Error(ErrorCode::timeout, "timeout"));
      }
      it = candidates_.erase(it);
    } else {
      ++it;
    }
  }
}

void CandidatesBuffer::add_new_candidate(BlockIdExt id, PublicKey source, FileHash collated_data_file_hash) {
  auto it = candidates_.emplace(id, Candidate{});
  Candidate &entry = it.first->second;
  entry.ttl_ = td::Timestamp::in(120.0);
  if (!it.second) {  // not inserted
    return;
  }
  LOG(DEBUG) << "New block candidate " << id.to_str();
  entry.source_ = source;
  entry.collated_data_file_hash_ = collated_data_file_hash;
}

void CandidatesBuffer::get_block_data(BlockIdExt id, td::Promise<td::Ref<BlockData>> promise) {
  auto it = candidates_.find(id);
  if (it == candidates_.end()) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "unknown block candidate"));
    return;
  }
  Candidate &entry = it->second;
  if (entry.data_.not_null()) {
    promise.set_result(entry.data_);
    return;
  }
  entry.data_waiters_.push_back(std::move(promise));
  if (entry.data_requested_) {
    return;
  }
  entry.data_requested_ = true;
  td::actor::send_closure(manager_, &ValidatorManager::get_block_candidate_from_db, entry.source_, id,
                          entry.collated_data_file_hash_, [SelfId = actor_id(this), id](td::Result<BlockCandidate> R) {
                            td::actor::send_closure(SelfId, &CandidatesBuffer::got_block_candidate, id, std::move(R));
                          });
}

void CandidatesBuffer::got_block_candidate(BlockIdExt id, td::Result<BlockCandidate> R) {
  if (R.is_error()) {
    finish_get_block_data(id, R.move_as_error());
    return;
  }
  BlockCandidate cand = R.move_as_ok();
  CHECK(cand.id == id);
  finish_get_block_data(id, create_block(id, std::move(cand.data)));
}

void CandidatesBuffer::get_block_state(BlockIdExt id, td::Promise<td::Ref<ShardState>> promise) {
  auto it = candidates_.find(id);
  if (it == candidates_.end()) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "unknown block candidate"));
    return;
  }
  Candidate &entry = it->second;
  if (entry.state_.not_null()) {
    promise.set_result(entry.state_);
    return;
  }
  entry.state_waiters_.push_back(std::move(promise));
  if (entry.state_requested_) {
    return;
  }
  entry.state_requested_ = true;
  get_block_data(id, [SelfId = actor_id(this), id](td::Result<td::Ref<BlockData>> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &CandidatesBuffer::finish_get_block_state, id, R.move_as_error());
      return;
    }
    td::actor::send_closure(SelfId, &CandidatesBuffer::get_block_state_cont, id, R.move_as_ok());
  });
}

void CandidatesBuffer::get_block_state_cont(BlockIdExt id, td::Ref<BlockData> data) {
  CHECK(id == data->block_id());
  std::vector<BlockIdExt> prev;
  BlockIdExt mc_blkid;
  bool after_split;
  auto S = block::unpack_block_prev_blk_ext(data->root_cell(), id, prev, mc_blkid, after_split);
  if (S.is_error()) {
    finish_get_block_state(id, std::move(S));
    return;
  }
  get_block_state_cont2(std::move(data), std::move(prev), {});
}

void CandidatesBuffer::get_block_state_cont2(td::Ref<BlockData> block, std::vector<BlockIdExt> prev,
                                             std::vector<td::Ref<ShardState>> prev_states) {
  if (prev_states.size() < prev.size()) {
    BlockIdExt prev_id = prev[prev_states.size()];
    td::actor::send_closure(manager_, &ValidatorManager::get_shard_state_from_db_short, prev_id,
                            [SelfId = actor_id(this), block = std::move(block), prev = std::move(prev),
                             prev_states = std::move(prev_states)](td::Result<td::Ref<ShardState>> R) mutable {
                              if (R.is_error()) {
                                td::actor::send_closure(SelfId, &CandidatesBuffer::finish_get_block_state,
                                                        block->block_id(), R.move_as_error());
                                return;
                              }
                              prev_states.push_back(R.move_as_ok());
                              td::actor::send_closure(SelfId, &CandidatesBuffer::get_block_state_cont2,
                                                      std::move(block), std::move(prev), std::move(prev_states));
                            });
    return;
  }

  BlockIdExt id = block->block_id();
  td::Ref<ShardState> state;
  CHECK(prev_states.size() == 1 || prev_states.size() == 2);
  if (prev_states.size() == 2) {  // after merge
    auto R = prev_states[0]->merge_with(*prev_states[1]);
    if (R.is_error()) {
      finish_get_block_state(id, R.move_as_error());
      return;
    }
    state = R.move_as_ok();
  } else if (id.shard_full() != prev[0].shard_full()) {  // after split
    auto R = prev_states[0]->split();
    if (R.is_error()) {
      finish_get_block_state(id, R.move_as_error());
      return;
    }
    auto s = R.move_as_ok();
    state = is_left_child(id.shard_full()) ? std::move(s.first) : std::move(s.second);
  } else {  // no split/merge
    state = std::move(prev_states[0]);
  }

  auto S = state.write().apply_block(id, std::move(block));
  if (S.is_error()) {
    finish_get_block_state(id, std::move(S));
    return;
  }
  finish_get_block_state(id, std::move(state));
}

void CandidatesBuffer::finish_get_block_data(BlockIdExt id, td::Result<td::Ref<BlockData>> res) {
  auto it = candidates_.find(id);
  if (it == candidates_.end()) {
    return;
  }
  Candidate &entry = it->second;
  for (auto &p : entry.data_waiters_) {
    p.set_result(res.clone());
  }
  entry.data_waiters_.clear();
  entry.data_requested_ = false;
  if (res.is_ok()) {
    entry.data_ = res.move_as_ok();
    LOG(DEBUG) << "Loaded block data for " << id.to_str();
  } else {
    LOG(DEBUG) << "Failed to load block data for " << id.to_str() << ": " << res.move_as_error();
  }
}

void CandidatesBuffer::finish_get_block_state(BlockIdExt id, td::Result<td::Ref<ShardState>> res) {
  auto it = candidates_.find(id);
  if (it == candidates_.end()) {
    return;
  }
  Candidate &entry = it->second;
  for (auto &p : entry.state_waiters_) {
    p.set_result(res.clone());
  }
  entry.state_waiters_.clear();
  entry.state_requested_ = false;
  if (res.is_ok()) {
    entry.state_ = res.move_as_ok();
    LOG(DEBUG) << "Loaded block state for " << id.to_str();
  } else {
    LOG(DEBUG) << "Failed to load block state for " << id.to_str() << ": " << res.move_as_error();
  }
}

}  // namespace ton::validator
