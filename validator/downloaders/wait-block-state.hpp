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

namespace ton {

namespace validator {

class WaitBlockState : public td::actor::Actor {
 public:
  WaitBlockState(BlockHandle handle, td::uint32 priority, td::actor::ActorId<ValidatorManager> manager,
                 td::Timestamp timeout, td::Promise<td::Ref<ShardState>> promise)
      : handle_(std::move(handle))
      , priority_(priority)
      , manager_(manager)
      , timeout_(timeout)
      , promise_(std::move(promise))
      , perf_timer_("waitstate", 1.0, [manager](double duration) {
          send_closure(manager, &ValidatorManager::add_perf_timer_stat, "waitstate", duration);
        }) {
  }

  void abort_query(td::Status reason);
  void finish_query();
  void alarm() override;

  void force_read_from_db();

  void start_up() override;
  void got_block_handle(BlockHandle handle);
  void start();
  void got_state_from_db(td::Ref<ShardState> data);
  void got_state_from_static_file(td::Ref<ShardState> state, td::BufferSlice data);
  void failed_to_get_state_from_db(td::Status reason);
  void got_prev_state(td::Ref<ShardState> state);
  void failed_to_get_prev_state(td::Status reason);
  void got_block_data(td::Ref<BlockData> data);
  void failed_to_get_block_data(td::Status reason);
  void got_state_from_net(td::BufferSlice data);
  void failed_to_get_zero_state();
  void failed_to_get_state_from_net(td::Status reason);
  void got_proof_link(td::BufferSlice data);
  void got_proof(td::BufferSlice data);
  void apply();
  void written_state(td::Ref<ShardState> upd_state);
  void written_state_file();
  void update_timeout(td::Timestamp timeout, td::uint32 priority) {
    timeout_ = timeout;
    alarm_timestamp() = timeout_;
    priority_ = priority;
  }

 private:
  BlockHandle handle_;

  td::uint32 priority_;

  td::actor::ActorId<ValidatorManager> manager_;
  td::Timestamp timeout_;
  td::Promise<td::Ref<ShardState>> promise_;

  td::Ref<ShardState> prev_state_;
  td::Ref<BlockData> block_;

  bool reading_from_db_ = false;
  td::Timestamp next_static_file_attempt_;

  td::PerfWarningTimer perf_timer_;
};

}  // namespace validator

}  // namespace ton
