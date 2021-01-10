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

class ValidatorManager;
class WaitBlockState;

class WaitBlockStateMerge : public td::actor::Actor {
 public:
  WaitBlockStateMerge(BlockIdExt left, BlockIdExt right, td::uint32 priority,
                      td::actor::ActorId<ValidatorManager> manager, td::Timestamp timeout,
                      td::Promise<td::Ref<ShardState>> promise)
      : left_(left)
      , right_(right)
      , priority_(priority)
      , manager_(manager)
      , timeout_(timeout)
      , promise_(std::move(promise)) {
  }

  void abort_query(td::Status reason);
  void finish_query(td::Ref<ShardState> result);
  void alarm() override;

  void start_up() override;
  void got_answer(bool left, td::Ref<ShardState> state);

 private:
  BlockIdExt left_;
  BlockIdExt right_;

  td::uint32 priority_;

  td::actor::ActorId<ValidatorManager> manager_;
  td::Timestamp timeout_;
  td::Promise<td::Ref<ShardState>> promise_;

  //td::actor::ActorOwn<WaitBlockState> left_query_;
  //td::actor::ActorOwn<WaitBlockState> right_query_;

  td::Ref<ShardState> left_state_;
  td::Ref<ShardState> right_state_;
};

}  // namespace validator

}  // namespace ton

