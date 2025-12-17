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
#include "td/utils/port/path.h"
#include "validator/db/package.hpp"
#include "validator/interfaces/validator-manager.h"

namespace ton {

namespace validator {

class ArchiveImporterLocal : public td::actor::Actor {
 public:
  ArchiveImporterLocal(std::string db_root, td::Ref<MasterchainState> state, BlockSeqno shard_client_seqno,
                       td::Ref<ValidatorManagerOptions> opts, td::actor::ActorId<ValidatorManager> manager,
                       std::vector<std::string> to_import_files,
                       td::Promise<std::pair<BlockSeqno, BlockSeqno>> promise);
  void start_up() override;

  void abort_query(td::Status error);
  void finish_query();

  td::Status process_package(std::string path);

  void process_masterchain_blocks();
  void process_masterchain_blocks_cont();

  void import_first_key_block();
  void checked_key_block_proof(BlockHandle handle);
  void applied_key_block(td::Ref<MasterchainState> state);

  void checked_masterchain_proofs();
  void got_shard_client_state(td::Ref<MasterchainState> state);

  void try_advance_shard_client_seqno();
  void try_advance_shard_client_seqno_cont(td::Ref<BlockData> mc_block);

  void processed_shard_blocks();
  void store_data();
  void apply_next_masterchain_block();
  void applied_next_masterchain_block(td::Ref<MasterchainState> state);

  void apply_shard_blocks();
  void applied_shard_blocks();

  void apply_shard_block(BlockIdExt block_id, BlockIdExt mc_block_id, td::Promise<td::Unit> promise);
  void apply_shard_block_cont1(BlockHandle handle, BlockIdExt mc_block_id, td::Promise<td::Unit> promise);
  void apply_shard_block_cont2(BlockHandle handle, BlockIdExt mc_block_id, td::Promise<td::Unit> promise);
  void check_shard_block_applied(BlockIdExt block_id, td::Promise<td::Unit> promise);

 private:
  std::string db_root_;
  td::Ref<MasterchainState> last_masterchain_state_;
  BlockSeqno shard_client_seqno_;

  td::Ref<ValidatorManagerOptions> opts_;

  td::actor::ActorId<ValidatorManager> manager_;

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
  BlockSeqno current_shard_client_seqno_;
  std::set<BlockIdExt> visited_shard_blocks_;
  std::set<BlockIdExt> new_zerostates_;

  std::map<BlockSeqno, std::pair<BlockIdExt, std::vector<BlockIdExt>>> shard_configs_;

  bool imported_any_ = false;

  td::PerfWarningTimer perf_timer_;
};

}  // namespace validator

}  // namespace ton
