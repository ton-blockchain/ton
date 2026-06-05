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

#include "validator/interfaces/validator-manager.h"

#include "stats-provider.h"

namespace ton {

namespace validator {

class SplitStateDeserializer;

struct SplitStatePart {
  ShardId effective_shard;
  vm::CellHash root_hash;
};

class DownloadShardState : public td::actor::Actor {
 public:
  DownloadShardState(BlockIdExt block_id, BlockIdExt masterchain_block_id, td::uint32 split_depth, td::uint32 priority,
                     td::actor::ActorId<ValidatorManager> manager, td::Timestamp timeout,
                     td::Promise<td::Ref<ShardState>> promise);
  ~DownloadShardState();

  void start_up() override;
  void got_block_handle(BlockHandle handle);
  void retry();
  void downloaded_proof_link(td::BufferSlice data);
  void checked_proof_link();

  void download_state();
  void download_proof_link();

  void download_zero_state();
  void downloaded_zero_state(td::BufferSlice data);

  void downloaded_shard_state(td::BufferSlice data);
  void checked_shard_state();

  void downloaded_split_state_header(td::BufferSlice data);
  void download_next_part_or_finish();
  void downloaded_state_part(td::BufferSlice data);
  void written_state_part_file();
  void saved_state_part_into_celldb(td::Ref<vm::DataCell> cell);

  void written_shard_state_file();
  void written_shard_state(td::Ref<ShardState> state);
  void written_block_handle();

  void finish_query();
  void alarm() override;
  void abort_query(td::Status reason);

  static void fail_handler(td::actor::ActorId<DownloadShardState> SelfId, td::Status error);

 private:
  BlockIdExt block_id_;
  BlockIdExt masterchain_block_id_;
  td::uint32 split_depth_;

  BlockHandle handle_;
  td::uint32 priority_;

  td::actor::ActorId<ValidatorManager> manager_;
  td::Timestamp timeout_;
  td::Promise<td::Ref<ShardState>> promise_;

  std::unique_ptr<SplitStateDeserializer> deserializer_;
  std::vector<SplitStatePart> parts_;
  std::vector<Ref<vm::Cell>> stored_parts_;

  td::BufferSlice data_;
  td::Ref<ShardState> state_;

  ProcessStatus status_;
};

}  // namespace validator

}  // namespace ton
