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
#pragma once
#include "vm/cells.h"
#include "ton/ton-types.h"
#include "auto/tl/ton_api.h"
#include "interfaces/out-msg-queue-proof.h"
#include "td/actor/actor.h"

namespace ton {

namespace validator {
using td::Ref;

class ValidatorManager;
class ValidatorManagerInterface;

class WaitOutMsgQueueProof : public td::actor::Actor {
 public:
  WaitOutMsgQueueProof(BlockIdExt block_id, ShardIdFull dst_shard, block::ImportedMsgQueueLimits limits, bool local,
                       td::uint32 priority, td::actor::ActorId<ValidatorManager> manager, td::Timestamp timeout,
                       td::Promise<Ref<OutMsgQueueProof>> promise)
      : block_id_(std::move(block_id))
      , dst_shard_(dst_shard)
      , limits_(limits)
      , local_(local)
      , priority_(priority)
      , manager_(manager)
      , timeout_(timeout)
      , promise_(std::move(promise)) {
  }

  void update_timeout(td::Timestamp timeout, td::uint32 priority) {
    timeout_ = timeout;
    alarm_timestamp() = timeout_;
    priority_ = priority;
  }

  void abort_query(td::Status reason);
  void finish_query(Ref<OutMsgQueueProof> result);
  void alarm() override;

  void start_up() override;

  void run_local();
  void got_state_root(Ref<vm::Cell> root);
  void got_block_root(Ref<vm::Cell> root);
  void run_local_cont();

  void run_net();

 private:
  BlockIdExt block_id_;
  ShardIdFull dst_shard_;
  block::ImportedMsgQueueLimits limits_;
  bool local_;
  td::uint32 priority_;

  td::actor::ActorId<ValidatorManager> manager_;
  td::Timestamp timeout_;
  td::Promise<Ref<OutMsgQueueProof>> promise_;

  Ref<vm::Cell> state_root_, block_root_;
  unsigned pending = 0;
};

class BuildOutMsgQueueProof : public td::actor::Actor {
 public:
  BuildOutMsgQueueProof(BlockIdExt block_id, ShardIdFull dst_shard, block::ImportedMsgQueueLimits limits,
                        td::actor::ActorId<ValidatorManagerInterface> manager,
                        td::Promise<tl_object_ptr<ton_api::tonNode_outMsgQueueProof>> promise)
      : block_id_(std::move(block_id))
      , dst_shard_(dst_shard)
      , limits_(limits)
      , manager_(manager)
      , promise_(std::move(promise)) {
  }

  void abort_query(td::Status reason);
  void start_up() override;
  void got_state_root(Ref<vm::Cell> root);
  void got_block_root(Ref<vm::Cell> root);
  void build_proof();

 private:
  BlockIdExt block_id_;
  ShardIdFull dst_shard_;
  block::ImportedMsgQueueLimits limits_;

  td::actor::ActorId<ValidatorManagerInterface> manager_;
  td::Promise<tl_object_ptr<ton_api::tonNode_outMsgQueueProof>> promise_;

  Ref<vm::Cell> state_root_, block_root_;
  unsigned pending = 0;
};

}  // namespace validator
}  // namespace ton
