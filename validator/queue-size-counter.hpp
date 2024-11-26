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
#include "interfaces/validator-manager.h"

namespace ton::validator {

class QueueSizeCounter : public td::actor::Actor {
 public:
  QueueSizeCounter(td::Ref<MasterchainState> last_masterchain_state, td::Ref<ValidatorManagerOptions> opts,
                   td::actor::ActorId<ValidatorManager> manager)
      : init_masterchain_state_(last_masterchain_state), opts_(std::move(opts)), manager_(std::move(manager)) {
  }

  void start_up() override;
  void get_queue_size(BlockIdExt block_id, td::Promise<td::uint64> promise);
  void alarm() override;

  void update_options(td::Ref<ValidatorManagerOptions> opts) {
    opts_ = std::move(opts);
  }

 private:
  td::Ref<MasterchainState> init_masterchain_state_;
  td::Ref<ValidatorManagerOptions> opts_;
  td::actor::ActorId<ValidatorManager> manager_;
  bool simple_mode_ = false;

  BlockSeqno current_seqno_ = 0;
  std::vector<BlockIdExt> init_top_blocks_;
  std::vector<BlockIdExt> last_top_blocks_;

  struct Entry {
    bool started_ = false;
    bool done_ = false;
    bool calc_whole_ = false;
    td::uint64 queue_size_ = 0;
    std::vector<td::Promise<td::uint64>> promises_;
  };
  std::map<BlockIdExt, Entry> results_;

  void get_queue_size_ex(BlockIdExt block_id, bool calc_whole, td::Promise<td::uint64> promise);
  void get_queue_size_cont(BlockHandle handle, td::Ref<ShardState> state);
  void get_queue_size_cont2(td::Ref<ShardState> state, td::Ref<ShardState> prev_state, td::uint64 prev_size);
  void on_error(BlockIdExt block_id, td::Status error);

  void process_top_shard_blocks();
  void process_top_shard_blocks_cont(td::Ref<MasterchainState> state, bool init = false);
  void get_queue_size_ex_retry(BlockIdExt block_id, bool calc_whole, td::Promise<td::Unit> promise);
  void process_top_shard_blocks_finish();
  void wait_shard_client();

  bool is_block_too_old(const BlockIdExt& block_id) const {
    for (const BlockIdExt& top_block : last_top_blocks_) {
      if (shard_intersects(block_id.shard_full(), top_block.shard_full())) {
        if (block_id.seqno() + 100 < top_block.seqno()) {
          return true;
        }
        break;
      }
    }
    for (const BlockIdExt& init_top_block : init_top_blocks_) {
      if (shard_intersects(block_id.shard_full(), init_top_block.shard_full())) {
        if (block_id.seqno() < init_top_block.seqno()) {
          return true;
        }
        break;
      }
    }
    return false;
  }
};

}  // namespace ton::validator
