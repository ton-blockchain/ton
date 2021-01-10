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

class WaitBlockDataDisk : public td::actor::Actor {
 public:
  WaitBlockDataDisk(BlockHandle handle, td::actor::ActorId<ValidatorManager> manager, td::Timestamp timeout,
                    td::Promise<td::Ref<BlockData>> promise)
      : handle_(std::move(handle)), manager_(manager), timeout_(timeout), promise_(std::move(promise)) {
  }

  void update_timeout(td::Timestamp timeout) {
    if (timeout.at() > timeout_.at()) {
      timeout_ = timeout;
      alarm_timestamp() = timeout_;
    }
  }

  void abort_query(td::Status reason);
  void finish_query();
  void alarm() override;

  void start_up() override;
  void got_block_handle(BlockHandle handle);
  void start();
  void got_block_data_from_db(td::Ref<BlockData> data);

 private:
  BlockHandle handle_;

  td::actor::ActorId<ValidatorManager> manager_;
  td::Timestamp timeout_;
  td::Promise<td::Ref<BlockData>> promise_;

  td::Ref<BlockData> data_;
};

}  // namespace validator

}  // namespace ton
