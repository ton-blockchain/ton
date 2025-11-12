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

#include "td/actor/actor.h"
#include "td/actor/coro_task.h"
#include "validator/interfaces/validator-manager.h"

namespace ton {

namespace validator {

class ArchiveImporterLocal : public td::actor::Actor {
 public:
  ArchiveImporterLocal(std::string db_root, td::Ref<MasterchainState> state, BlockSeqno shard_client_seqno,
                       td::Ref<ValidatorManagerOptions> opts, td::actor::ActorId<ValidatorManager> manager,
                       td::actor::ActorId<Db> db, std::vector<std::string> to_import_files,
                       td::Promise<std::pair<BlockSeqno, BlockSeqno>> promise);
  void start_up() override;

  td::actor::Task<td::Unit> run();
  td::actor::Task<td::Unit> run_inner();

  void read_files();
  td::Status process_package(std::string path);

  td::actor::Task<td::Unit> process_masterchain_blocks();
  td::actor::Task<td::Unit> import_first_key_block();
  td::actor::Task<td::Unit> check_masterchain_proofs();

  td::actor::Task<td::Unit> process_shard_blocks();
  td::actor::Task<bool> try_advance_shard_client_seqno();

  td::actor::Task<td::Unit> store_data();
  td::actor::Task<td::Unit> store_block_data(td::Ref<BlockData> block);

  td::actor::Task<td::Unit> apply_blocks();
  td::actor::Task<td::Unit> apply_blocks_async(const std::vector<std::pair<BlockIdExt, BlockIdExt>>& blocks);

  td::actor::Task<BlockHandle> apply_block_async_1(BlockIdExt block_id, BlockIdExt mc_block_id);
  td::actor::Task<td::Unit> apply_block_async_2(BlockHandle handle);
  td::actor::Task<td::Unit> apply_block_async_3(BlockHandle handle);
  td::actor::Task<td::Unit> apply_block_async_4(BlockHandle handle);

 private:
  std::string db_root_;
  td::Ref<MasterchainState> last_masterchain_state_;
  BlockSeqno shard_client_seqno_;
  BlockSeqno final_masterchain_state_seqno_ = 0;
  BlockSeqno final_shard_client_seqno_ = 0;

  td::Ref<ValidatorManagerOptions> opts_;

  td::actor::ActorId<ValidatorManager> manager_;
  td::actor::ActorId<Db> db_;

  std::vector<std::string> to_import_files_;
  td::Promise<std::pair<BlockSeqno, BlockSeqno>> promise_;

  struct BlockInfo {
    td::Ref<BlockData> block;
    td::Ref<Proof> proof;
    td::Ref<ProofLink> proof_link;
    bool import = false;
  };
  std::map<BlockIdExt, BlockInfo> blocks_;
  std::map<BlockSeqno, BlockIdExt> masterchain_blocks_;

  td::Ref<MasterchainState> shard_client_state_;
  BlockSeqno new_shard_client_seqno_;
  std::set<BlockIdExt> visited_shard_blocks_;
  std::set<BlockIdExt> new_zerostates_;
  std::vector<std::pair<BlockIdExt, BlockIdExt>> blocks_to_apply_mc_;
  std::vector<std::pair<BlockIdExt, BlockIdExt>> blocks_to_apply_shards_;

  std::map<BlockSeqno, std::pair<BlockIdExt, std::vector<BlockIdExt>>> shard_configs_;

  bool imported_any_ = false;

  td::PerfWarningTimer perf_timer_;
};

}  // namespace validator

}  // namespace ton
