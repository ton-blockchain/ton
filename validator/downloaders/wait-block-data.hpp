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
#include "interfaces/validator-manager.h"

namespace ton {

namespace validator {

class ValidatorManager;

class WaitBlockData : public td::actor::Actor {
 public:
  WaitBlockData(BlockHandle handle, td::uint32 priority, td::actor::ActorId<ValidatorManager> manager,
                td::Timestamp timeout, td::Promise<td::Ref<BlockData>> promise)
      : handle_(std::move(handle))
      , priority_(priority)
      , manager_(manager)
      , timeout_(timeout)
      , promise_(std::move(promise))
      , perf_timer_("waitdata", 1.0, [manager](double duration) {
          send_closure(manager, &ValidatorManager::add_perf_timer_stat, "waitdata", duration);
        }) {
  }

  void update_timeout(td::Timestamp timeout, td::uint32 priority) {
    timeout_ = timeout;
    alarm_timestamp() = timeout_;
    priority_ = priority;
  }

  void abort_query(td::Status reason);
  void finish_query();
  void alarm() override;

  void force_read_from_db();

  void start_up() override;
  void set_is_hardfork(bool value);
  void start();
  void got_block_data_from_db(td::Ref<BlockData> data);
  void got_block_data_from_net(ReceivedBlock data);
  void failed_to_get_block_data_from_net(td::Status reason);

  void got_static_file(td::BufferSlice data);

 private:
  BlockHandle handle_;

  td::uint32 priority_;

  td::actor::ActorId<ValidatorManager> manager_;
  td::Timestamp timeout_;
  td::Promise<td::Ref<BlockData>> promise_;

  td::Ref<BlockData> data_;

  bool reading_from_db_ = false;
  bool is_hardfork_ = false;
  td::Timestamp try_read_static_file_ = td::Timestamp::now();

  td::PerfWarningTimer perf_timer_;
};

}  // namespace validator

}  // namespace ton
