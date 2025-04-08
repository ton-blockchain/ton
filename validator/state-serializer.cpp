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
#include "state-serializer.hpp"
#include "td/utils/Random.h"
#include "ton/ton-io.hpp"
#include "common/delay.h"
#include "td/utils/filesystem.h"
#include "td/utils/HashSet.h"

namespace ton {

namespace validator {

void AsyncStateSerializer::start_up() {
  if (!opts_->get_state_serializer_enabled()) {
    LOG(ERROR) << "Persistent state serializer is disabled";
  }
  alarm_timestamp() = td::Timestamp::in(1.0 + td::Random::fast(0, 10) * 1.0);
  running_ = true;

  //next_iteration();
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<AsyncSerializerState> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &AsyncStateSerializer::got_self_state, R.move_as_ok());
  });
  td::actor::send_closure(manager_, &ValidatorManager::get_async_serializer_state, std::move(P));
}

void AsyncStateSerializer::got_self_state(AsyncSerializerState state) {
  if (state.last_block_id.is_valid()) {
    last_block_id_ = state.last_block_id;
    last_key_block_id_ = state.last_written_block_id;
    last_key_block_ts_ = state.last_written_block_ts;

    running_ = false;

    next_iteration();
  } else {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<BlockHandle> R) {
      R.ensure();
      td::actor::send_closure(SelfId, &AsyncStateSerializer::got_init_handle, R.move_as_ok());
    });
    td::actor::send_closure(manager_, &ValidatorManager::get_block_handle, last_block_id_, true, std::move(P));
  }

  inited_block_id_ = true;
  for (auto& promise : wait_init_block_id_) {
    promise.set_value(td::Unit());
  }
  wait_init_block_id_.clear();
}

void AsyncStateSerializer::got_init_handle(BlockHandle handle) {
  CHECK(handle->id().id.seqno == 0 || handle->is_key_block());
  last_key_block_id_ = handle->id();
  last_key_block_ts_ = handle->unix_time();

  masterchain_handle_ = std::move(handle);

  running_ = false;
  saved_to_db_ = false;

  next_iteration();
}

void AsyncStateSerializer::alarm() {
  alarm_timestamp() = td::Timestamp::in(1.0 + td::Random::fast(0, 10) * 1.0);

  next_iteration();

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<BlockIdExt> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &AsyncStateSerializer::got_top_masterchain_handle, R.move_as_ok());
  });
  td::actor::send_closure(manager_, &ValidatorManager::get_top_masterchain_block, std::move(P));
}

void AsyncStateSerializer::request_previous_state_files() {
  td::actor::send_closure(
      manager_, &ValidatorManager::get_previous_persistent_state_files, masterchain_handle_->id().seqno(),
      [SelfId = actor_id(this)](td::Result<std::vector<std::pair<std::string, ShardIdFull>>> R) {
        R.ensure();
        td::actor::send_closure(SelfId, &AsyncStateSerializer::got_previous_state_files, R.move_as_ok());
      });
}

void AsyncStateSerializer::got_previous_state_files(std::vector<std::pair<std::string, ShardIdFull>> files) {
  previous_state_cache_ = std::make_shared<PreviousStateCache>();
  previous_state_cache_->state_files = std::move(files);
  request_masterchain_state();
}

void AsyncStateSerializer::request_masterchain_state() {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), manager = manager_](td::Result<td::Ref<ShardState>> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &AsyncStateSerializer::fail_handler,
                              R.move_as_error_prefix("failed to get masterchain state: "));
    } else {
      td::actor::send_closure(manager, &ValidatorManager::get_cell_db_reader,
                              [SelfId, state = td::Ref<MasterchainState>(R.move_as_ok())](
                                  td::Result<std::shared_ptr<vm::CellDbReader>> R) mutable {
                                if (R.is_error()) {
                                  td::actor::send_closure(SelfId, &AsyncStateSerializer::fail_handler,
                                                          R.move_as_error_prefix("failed to get cell db reader: "));
                                } else {
                                  td::actor::send_closure(SelfId, &AsyncStateSerializer::got_masterchain_state,
                                                          std::move(state), R.move_as_ok());
                                }
                              });
    }
  });
  td::actor::send_closure(manager_, &ValidatorManager::get_shard_state_from_db, masterchain_handle_, std::move(P));
}

void AsyncStateSerializer::request_shard_state(BlockIdExt shard) {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<BlockHandle> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &AsyncStateSerializer::got_shard_handle, R.move_as_ok());
  });
  return td::actor::send_closure(manager_, &ValidatorManager::get_block_handle, shard, true, std::move(P));
}

void AsyncStateSerializer::next_iteration() {
  if (running_) {
    return;
  }
  if (!masterchain_handle_) {
    running_ = true;
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<BlockHandle> R) {
      R.ensure();
      td::actor::send_closure(SelfId, &AsyncStateSerializer::got_masterchain_handle, R.move_as_ok());
    });
    td::actor::send_closure(manager_, &ValidatorManager::get_block_handle, last_block_id_, true, std::move(P));
    return;
  }
  if (!masterchain_handle_->inited_unix_time() || !masterchain_handle_->inited_is_key_block() ||
      !masterchain_handle_->is_applied()) {
    return;
  }
  CHECK(masterchain_handle_->id() == last_block_id_);
  if (attempt_ < max_attempt() && last_key_block_id_.id.seqno < last_block_id_.id.seqno &&
      need_serialize(masterchain_handle_)) {
    if (!stored_persistent_state_description_) {
      LOG(INFO) << "storing persistent state description for " << masterchain_handle_->id().id;
      running_ = true;
      auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<ShardState>> R) {
        if (R.is_error()) {
          td::actor::send_closure(SelfId, &AsyncStateSerializer::fail_handler,
                                  R.move_as_error_prefix("failed to get masterchain state: "));
        } else {
          td::actor::send_closure(SelfId, &AsyncStateSerializer::store_persistent_state_description,
                                  td::Ref<MasterchainState>(R.move_as_ok()));
        }
      });
      td::actor::send_closure(manager_, &ValidatorManager::get_shard_state_from_db, masterchain_handle_, std::move(P));
      return;
    }
    if (!have_masterchain_state_ && !opts_->get_state_serializer_enabled()) {
      LOG(ERROR) << "skipping serializing persistent state for " << masterchain_handle_->id().id.to_str()
                 << ": serializer is disabled (by user)";
    } else if (!have_masterchain_state_ && auto_disabled_) {
      LOG(ERROR) << "skipping serializing persistent state for " << masterchain_handle_->id().id.to_str()
                 << ": serializer is disabled (automatically)";
    } else if (!have_masterchain_state_ && have_newer_persistent_state(masterchain_handle_->unix_time())) {
      LOG(ERROR) << "skipping serializing persistent state for " << masterchain_handle_->id().id.to_str()
                 << ": newer key block with ts=" << last_known_key_block_ts_ << " exists";
    } else {
      if (!have_masterchain_state_) {
        LOG(ERROR) << "started serializing persistent state for " << masterchain_handle_->id().id.to_str();
        // block next attempts immediately, but send actual request later
        running_ = true;
        double delay = td::Random::fast(0, 3600 * 6);
        LOG(WARNING) << "serializer delay = " << delay << "s";
        delay_action(
            [SelfId = actor_id(this)]() {
              td::actor::send_closure(SelfId, &AsyncStateSerializer::request_previous_state_files);
            },
            td::Timestamp::in(delay));
        current_status_ = PSTRING() << "delay before serializing seqno=" << masterchain_handle_->id().seqno() << " "
                                    << (int)delay << "s";
        current_status_ts_ = td::Timestamp::now();
        return;
      }
      if (next_idx_ < shards_.size()) {
        running_ = true;
        request_shard_state(shards_[next_idx_]);
        return;
      }
      LOG(ERROR) << "finished serializing persistent state for " << masterchain_handle_->id().id.to_str();
    }
    last_key_block_ts_ = masterchain_handle_->unix_time();
    last_key_block_id_ = masterchain_handle_->id();
    previous_state_cache_ = {};
  }
  if (!saved_to_db_) {
    running_ = true;
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
      R.ensure();
      td::actor::send_closure(SelfId, &AsyncStateSerializer::saved_to_db);
    });
    td::actor::send_closure(manager_, &ValidatorManager::update_async_serializer_state,
                            AsyncSerializerState{masterchain_handle_->id(), last_key_block_id_, last_key_block_ts_},
                            std::move(P));
    return;
  }
  if (masterchain_handle_->inited_next_left()) {
    last_block_id_ = masterchain_handle_->one_next(true);
    have_masterchain_state_ = false;
    stored_persistent_state_description_ = false;
    masterchain_handle_ = nullptr;
    saved_to_db_ = false;
    shards_.clear();
    next_idx_ = 0;
    next_iteration();
  }
}

void AsyncStateSerializer::got_top_masterchain_handle(BlockIdExt block_id) {
  if (masterchain_handle_ && masterchain_handle_->id().id.seqno < block_id.id.seqno) {
    CHECK(masterchain_handle_->inited_next_left());
  }
}

void AsyncStateSerializer::store_persistent_state_description(td::Ref<MasterchainState> state) {
  stored_persistent_state_description_ = true;
  attempt_ = 0;
  running_ = false;

  PersistentStateDescription desc;
  desc.masterchain_id = state->get_block_id();
  desc.start_time = state->get_unix_time();
  desc.end_time = ValidatorManager::persistent_state_ttl(desc.start_time);
  for (const auto &v : state->get_shards()) {
    desc.shard_blocks.push_back(v->top_block_id());
  }
  td::actor::send_closure(manager_, &ValidatorManager::add_persistent_state_description,
                          td::Ref<PersistentStateDescription>(true, std::move(desc)));

  next_iteration();
}

void AsyncStateSerializer::got_masterchain_handle(BlockHandle handle) {
  CHECK(!masterchain_handle_);
  masterchain_handle_ = std::move(handle);
  running_ = false;
  attempt_ = 0;
  next_iteration();
}

class CachedCellDbReader : public vm::CellDbReader {
 public:
  CachedCellDbReader(std::shared_ptr<vm::CellDbReader> parent,
                     std::shared_ptr<vm::CellHashSet> cache)
      : parent_(std::move(parent)), cache_(std::move(cache)) {
  }
  td::Result<td::Ref<vm::DataCell>> load_cell(td::Slice hash) override {
    ++total_reqs_;
    DCHECK(hash.size() == 32);
    if (cache_) {
      auto it = cache_->find(hash);
      if (it != cache_->end()) {
        ++cached_reqs_;
        TRY_RESULT(loaded_cell, (*it)->load_cell());
        return loaded_cell.data_cell;
      }
    }
    return parent_->load_cell(hash);
  }
  td::Result<std::vector<Ref<vm::DataCell>>> load_bulk(td::Span<td::Slice> hashes) override {
    total_reqs_ += hashes.size();
    if (!cache_) {
      ++bulk_reqs_;
      return parent_->load_bulk(hashes);
    }
    std::vector<td::Slice> missing_hashes;
    std::vector<size_t> missing_indices;
    std::vector<td::Ref<vm::DataCell>> res(hashes.size());
    for (size_t i = 0; i < hashes.size(); i++) {
      auto it = cache_->find(hashes[i]);
      if (it != cache_->end()) {
        ++cached_reqs_;
        TRY_RESULT(loaded_cell, (*it)->load_cell());
        res[i] = loaded_cell.data_cell;
        continue;
      }
      missing_hashes.push_back(hashes[i]);
      missing_indices.push_back(i);
    }
    if (missing_hashes.empty()) {
      return std::move(res);
    }
    TRY_RESULT(missing_cells, parent_->load_bulk(missing_hashes));
    for (size_t i = 0; i < missing_indices.size(); i++) {
      res[missing_indices[i]] = missing_cells[i];
    }
    return res;
  };
  void print_stats() const {
    LOG(WARNING) << "CachedCellDbReader stats : " << total_reqs_ << " reads, " << cached_reqs_ << " cached, " 
                 << bulk_reqs_ << " bulk reqs";
  }
 private:
  std::shared_ptr<vm::CellDbReader> parent_;
  std::shared_ptr<vm::CellHashSet> cache_;

  td::uint64 total_reqs_ = 0;
  td::uint64 cached_reqs_ = 0;
  td::uint64 bulk_reqs_ = 0;
};

void AsyncStateSerializer::PreviousStateCache::prepare_cache(ShardIdFull shard) {
  std::vector<ShardIdFull> prev_shards;
  for (const auto& [_, prev_shard] : state_files) {
    if (shard_intersects(shard, prev_shard)) {
      prev_shards.push_back(prev_shard);
    }
  }
  if (prev_shards == cur_shards) {
    return;
  }
  cur_shards = std::move(prev_shards);
  cache = {};
  if (cur_shards.empty()) {
    return;
  }
  td::Timer timer;
  LOG(WARNING) << "Preloading previous persistent state for shard " << shard.to_str() << " ("
               << cur_shards.size() << " files)";
  vm::CellHashSet cells;
  std::function<void(td::Ref<vm::Cell>)> dfs = [&](td::Ref<vm::Cell> cell) {
    if (!cells.insert(cell).second) {
      return;
    }
    bool is_special;
    vm::CellSlice cs = vm::load_cell_slice_special(cell, is_special);
    for (unsigned i = 0; i < cs.size_refs(); ++i) {
      dfs(cs.prefetch_ref(i));
    }
  };
  for (const auto& [file, prev_shard] : state_files) {
    if (!shard_intersects(shard, prev_shard)) {
      continue;
    }
    auto r_data = td::read_file(file);
    if (r_data.is_error()) {
      LOG(INFO) << "Reading " << file << " : " << r_data.move_as_error();
      continue;
    }
    LOG(INFO) << "Reading " << file << " : " << td::format::as_size(r_data.ok().size());
    auto r_root = vm::std_boc_deserialize(r_data.move_as_ok());
    if (r_root.is_error()) {
      LOG(WARNING) << "Deserialize error : " << r_root.move_as_error();
      continue;
    }
    r_data.clear();
    dfs(r_root.move_as_ok());
  }
  LOG(WARNING) << "Preloaded previous state: " << cells.size() << " cells in " << timer.elapsed() << "s";
  cache = std::make_shared<vm::CellHashSet>(std::move(cells));
}

void AsyncStateSerializer::got_masterchain_state(td::Ref<MasterchainState> state,
                                                 std::shared_ptr<vm::CellDbReader> cell_db_reader) {
  if (!opts_->get_state_serializer_enabled() || auto_disabled_) {
    stored_masterchain_state();
    return;
  }
  LOG(ERROR) << "serializing masterchain state " << masterchain_handle_->id().id.to_str();
  have_masterchain_state_ = true;
  CHECK(next_idx_ == 0);
  CHECK(shards_.size() == 0);

  auto vec = state->get_shards();
  for (auto &v : vec) {
    if (opts_->need_monitor(v->shard(), state)) {
      shards_.push_back(v->top_block_id());
    }
  }

  auto write_data = [shard = state->get_shard(), root = state->root_cell(), cell_db_reader,
                     previous_state_cache = previous_state_cache_,
                     fast_serializer_enabled = opts_->get_fast_state_serializer_enabled(),
                     cancellation_token = cancellation_token_source_.get_cancellation_token()](td::FileFd& fd) mutable {
    if (!cell_db_reader) {
      return vm::std_boc_serialize_to_file(root, fd, 31, std::move(cancellation_token));
    }
    if (fast_serializer_enabled) {
      previous_state_cache->prepare_cache(shard);
    }
    auto new_cell_db_reader = std::make_shared<CachedCellDbReader>(cell_db_reader, previous_state_cache->cache);
    auto res = vm::boc_serialize_to_file_large(new_cell_db_reader, root->get_hash(), fd, 31, std::move(cancellation_token));
    new_cell_db_reader->print_stats();
    return res;
  };
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    if (R.is_error() && R.error().code() == cancelled) {
      LOG(ERROR) << "Persistent state serialization cancelled";
    } else {
      R.ensure();
    }
    td::actor::send_closure(SelfId, &AsyncStateSerializer::stored_masterchain_state);
  });

  td::actor::send_closure(manager_, &ValidatorManager::store_persistent_state_file_gen, masterchain_handle_->id(),
                          masterchain_handle_->id(), write_data, std::move(P));

  current_status_ = PSTRING() << "serializing masterchain state " << state->get_block_id().id.to_str();
  current_status_ts_ = td::Timestamp::now();
}

void AsyncStateSerializer::stored_masterchain_state() {
  current_status_ = "pending";
  current_status_ts_ = {};
  LOG(ERROR) << "finished serializing masterchain state " << masterchain_handle_->id().id.to_str();
  running_ = false;
  next_iteration();
}

void AsyncStateSerializer::got_shard_handle(BlockHandle handle) {
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), handle, manager = manager_](td::Result<td::Ref<ShardState>> R) {
        if (R.is_error()) {
          td::actor::send_closure(SelfId, &AsyncStateSerializer::fail_handler, R.move_as_error());
        } else {
          td::actor::send_closure(
              manager, &ValidatorManager::get_cell_db_reader,
              [SelfId, state = R.move_as_ok(), handle](td::Result<std::shared_ptr<vm::CellDbReader>> R) mutable {
                if (R.is_error()) {
                  td::actor::send_closure(SelfId, &AsyncStateSerializer::fail_handler,
                                          R.move_as_error_prefix("failed to get cell db reader: "));
                } else {
                  td::actor::send_closure(SelfId, &AsyncStateSerializer::got_shard_state, handle, std::move(state),
                                          R.move_as_ok());
                }
              });
        }
      });

  td::actor::send_closure(manager_, &ValidatorManager::get_shard_state_from_db, handle, std::move(P));
}

void AsyncStateSerializer::got_shard_state(BlockHandle handle, td::Ref<ShardState> state,
                                           std::shared_ptr<vm::CellDbReader> cell_db_reader) {
  next_idx_++;
  if (!opts_->get_state_serializer_enabled() || auto_disabled_) {
    success_handler();
    return;
  }
  LOG(ERROR) << "serializing shard state " << handle->id().id.to_str();
  auto write_data = [shard = state->get_shard(), root = state->root_cell(), cell_db_reader,
                     previous_state_cache = previous_state_cache_,
                     fast_serializer_enabled = opts_->get_fast_state_serializer_enabled(),
                     cancellation_token = cancellation_token_source_.get_cancellation_token()](td::FileFd& fd) mutable {
    if (!cell_db_reader) {
      return vm::std_boc_serialize_to_file(root, fd, 31, std::move(cancellation_token));
    }
    if (fast_serializer_enabled) {
      previous_state_cache->prepare_cache(shard);
    }
    auto new_cell_db_reader = std::make_shared<CachedCellDbReader>(cell_db_reader, previous_state_cache->cache);
    auto res = vm::boc_serialize_to_file_large(new_cell_db_reader, root->get_hash(), fd, 31, std::move(cancellation_token));
    new_cell_db_reader->print_stats();
    return res;
  };
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), handle](td::Result<td::Unit> R) {
    if (R.is_error() && R.error().code() == cancelled) {
      LOG(ERROR) << "Persistent state serialization cancelled";
    } else {
      R.ensure();
      LOG(ERROR) << "finished serializing shard state " << handle->id().id.to_str();
    }
    td::actor::send_closure(SelfId, &AsyncStateSerializer::success_handler);
  });
  td::actor::send_closure(manager_, &ValidatorManager::store_persistent_state_file_gen, handle->id(),
                          masterchain_handle_->id(), write_data, std::move(P));
  current_status_ = PSTRING() << "serializing shard state " << next_idx_ << "/" << shards_.size() << " "
                              << state->get_block_id().id.to_str();
  current_status_ts_ = td::Timestamp::now();
}

void AsyncStateSerializer::fail_handler(td::Status reason) {
  current_status_ = PSTRING() << "pending, " << reason;
  current_status_ts_ = {};
  VLOG(VALIDATOR_NOTICE) << "failure: " << reason;
  attempt_++;
  delay_action(
      [SelfId = actor_id(this)]() { td::actor::send_closure(SelfId, &AsyncStateSerializer::fail_handler_cont); },
      td::Timestamp::in(16.0));
}

void AsyncStateSerializer::fail_handler_cont() {
  running_ = false;
  next_iteration();
}

void AsyncStateSerializer::success_handler() {
  current_status_ = "pending";
  current_status_ts_ = {};
  running_ = false;
  next_iteration();
}

void AsyncStateSerializer::update_options(td::Ref<ValidatorManagerOptions> opts) {
  opts_ = std::move(opts);
  if (!opts_->get_state_serializer_enabled()) {
    cancellation_token_source_.cancel();
  }
}

void AsyncStateSerializer::auto_disable_serializer(bool disabled) {
  auto_disabled_ = disabled;
  if (auto_disabled_) {
    cancellation_token_source_.cancel();
  }
}

void AsyncStateSerializer::prepare_stats(td::Promise<std::vector<std::pair<std::string, std::string>>> promise) {
  if (!inited_block_id_) {
    wait_init_block_id_.push_back(
        [SelfId = actor_id(this), promise = std::move(promise)](td::Result<td::Unit> R) mutable {
          TRY_STATUS_PROMISE(promise, R.move_as_status());
          td::actor::send_closure(SelfId, &AsyncStateSerializer::prepare_stats, std::move(promise));
        });
    return;
  }
  std::vector<std::pair<std::string, std::string>> vec;
  vec.emplace_back("stateserializermasterchainseqno", td::to_string(last_block_id_.seqno()));
  td::StringBuilder sb;
  sb << current_status_;
  if (current_status_ts_) {
    sb << " (started " << (int)(td::Timestamp::now() - current_status_ts_) << "s ago)";
  }
  if (!opts_->get_state_serializer_enabled() || auto_disabled_) {
    sb << " (disabled)";
  }
  vec.emplace_back("stateserializerstatus", sb.as_cslice().str());
  promise.set_result(std::move(vec));
}

bool AsyncStateSerializer::need_serialize(BlockHandle handle) {
  if (handle->id().id.seqno == 0 || !handle->is_key_block()) {
    return false;
  }
  return ValidatorManager::is_persistent_state(handle->unix_time(), last_key_block_ts_) &&
         ValidatorManager::persistent_state_ttl(handle->unix_time()) > (UnixTime)td::Clocks::system();
}

bool AsyncStateSerializer::have_newer_persistent_state(UnixTime cur_ts) {
  return cur_ts / (1 << 17) < last_known_key_block_ts_ / (1 << 17);
}

}  // namespace validator

}  // namespace ton
