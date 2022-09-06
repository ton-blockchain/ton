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

#include "td/actor/actor.h"
#include "interfaces/validator-manager.h"

namespace ton {

namespace validator {

/*
 *
 * only for masterchain
 * --------------------
 * must ensure that block proof / proof_link is written
 * must ensure that prev, before_split, after_merge, state_root_hash and lt initialized
 * must initialize prev's next (to be sure, probably already initialized)
 * must write block data and state
 * must run new_block callback
 *
 */

class ApplyBlock : public td::actor::Actor {
 public:
  ApplyBlock(BlockIdExt id, td::Ref<BlockData> block, BlockIdExt masterchain_block_id,
             td::actor::ActorId<ValidatorManager> manager, td::Timestamp timeout, td::Promise<td::Unit> promise)
      : id_(id)
      , block_(std::move(block))
      , masterchain_block_id_(masterchain_block_id)
      , manager_(manager)
      , timeout_(timeout)
      , promise_(std::move(promise))
      , perf_timer_("applyblock", 0.1, [manager](double duration) {
          send_closure(manager, &ValidatorManager::add_perf_timer_stat, "applyblock", duration);
        }) {
  }

  static constexpr td::uint32 apply_block_priority() {
    return 2;
  }

  void abort_query(td::Status reason);
  void finish_query();
  void alarm() override;

  void start_up() override;
  void got_block_handle(BlockHandle handle);
  void written_block_data();
  void got_prev_state(td::Ref<ShardState> state);
  void got_cur_state(td::Ref<ShardState> state);
  void written_state();
  void written_next();
  void applied_prev();
  void applied_set();

 private:
  BlockIdExt id_;
  td::Ref<BlockData> block_;
  BlockIdExt masterchain_block_id_;
  td::actor::ActorId<ValidatorManager> manager_;
  td::Timestamp timeout_;
  td::Promise<td::Unit> promise_;

  BlockHandle handle_;
  td::Ref<ShardState> state_;

  td::PerfWarningTimer perf_timer_;
};

}  // namespace validator

}  // namespace ton
