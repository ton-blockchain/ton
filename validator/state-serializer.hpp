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
#include "interfaces/shard.h"

#include <map>

namespace ton {

namespace validator {

class AsyncStateSerializer : public td::actor::Actor {
 private:
  td::uint32 attempt_ = 0;
  bool running_ = false;
  BlockIdExt last_block_id_;
  BlockIdExt last_key_block_id_;
  UnixTime last_key_block_ts_ = 0;
  bool saved_to_db_ = true;

  td::Ref<ValidatorManagerOptions> opts_;

  td::actor::ActorId<ValidatorManager> manager_;

  td::uint32 next_idx_ = 0;

  BlockHandle masterchain_handle_;
  bool have_masterchain_state_ = false;

  std::vector<BlockIdExt> shards_;

 public:
  AsyncStateSerializer(BlockIdExt block_id, td::Ref<ValidatorManagerOptions> opts,
                       td::actor::ActorId<ValidatorManager> manager)
      : last_block_id_(block_id), opts_(std::move(opts)), manager_(manager) {
  }

  static constexpr td::uint32 max_attempt() {
    return 128;
  }

  bool need_serialize(BlockHandle handle);
  bool need_monitor(ShardIdFull shard);

  void alarm() override;
  void start_up() override;
  void got_self_state(AsyncSerializerState state);
  void got_init_handle(BlockHandle handle);

  void request_masterchain_state();
  void request_shard_state(BlockIdExt shard);

  void next_iteration();
  void got_top_masterchain_handle(BlockIdExt block_id);
  void got_masterchain_handle(BlockHandle handle_);
  void got_masterchain_state(td::Ref<MasterchainState> state, std::shared_ptr<vm::CellDbReader> cell_db_reader);
  void stored_masterchain_state();
  void got_shard_handle(BlockHandle handle);
  void got_shard_state(BlockHandle handle, td::Ref<ShardState> state, std::shared_ptr<vm::CellDbReader> cell_db_reader);

  void get_masterchain_seqno(td::Promise<BlockSeqno> promise) {
    promise.set_result(last_block_id_.id.seqno);
  }

  void saved_to_db() {
    saved_to_db_ = true;
    running_ = false;
    next_iteration();
  }

  void fail_handler(td::Status reason);
  void fail_handler_cont();
  void success_handler();
};

}  // namespace validator

}  // namespace ton
