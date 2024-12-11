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

    Copyright 2019-2020 Telegram Systems LLP
*/
#pragma once

#include "td/actor/actor.h"
#include "td/utils/port/path.h"
#include "validator/interfaces/validator-manager.h"
#include "validator/db/package.hpp"

namespace ton {

namespace validator {

class ArchiveImporter : public td::actor::Actor {
 public:
  ArchiveImporter(std::string db_root, td::Ref<MasterchainState> state, BlockSeqno shard_client_seqno,
                  td::Ref<ValidatorManagerOptions> opts, td::actor::ActorId<ValidatorManager> manager,
                  std::vector<std::string> to_import_files, td::Promise<std::pair<BlockSeqno, BlockSeqno>> promise);
  void start_up() override;

  void abort_query(td::Status error);
  void finish_query();

  void downloaded_mc_archive(std::string path);
  td::Status process_package(std::string path, bool with_masterchain);

  void processed_mc_archive();
  void check_masterchain_block(BlockSeqno seqno);
  void checked_masterchain_proof(BlockHandle handle, td::Ref<BlockData> data);
  void applied_masterchain_block(BlockHandle handle);
  void got_new_materchain_state(td::Ref<MasterchainState> state);

  void checked_all_masterchain_blocks();
  void download_shard_archives(td::Ref<MasterchainState> start_state);
  void download_shard_archive(ShardIdFull shard_prefix);
  void downloaded_shard_archive(std::string path);

  void check_next_shard_client_seqno(BlockSeqno seqno);
  void checked_shard_client_seqno(BlockSeqno seqno);
  void got_masterchain_state(td::Ref<MasterchainState> state);
  void apply_shard_block(BlockIdExt block_id, BlockIdExt masterchain_block_id, td::Promise<td::Unit> promise);
  void apply_shard_block_cont1(BlockHandle handle, BlockIdExt masterchain_block_id, td::Promise<td::Unit> promise);
  void apply_shard_block_cont2(BlockHandle handle, BlockIdExt masterchain_block_id, td::Promise<td::Unit> promise);
  void apply_shard_block_cont3(BlockHandle handle, BlockIdExt masterchain_block_id, td::Promise<td::Unit> promise);
  void check_shard_block_applied(BlockIdExt block_id, td::Promise<td::Unit> promise);

 private:
  std::string db_root_;
  td::Ref<MasterchainState> last_masterchain_state_;
  BlockSeqno shard_client_seqno_;
  BlockSeqno start_import_seqno_;

  td::Ref<ValidatorManagerOptions> opts_;

  td::actor::ActorId<ValidatorManager> manager_;

  std::vector<std::string> to_import_files_;
  bool use_imported_files_;
  td::Promise<std::pair<BlockSeqno, BlockSeqno>> promise_;

  std::map<BlockSeqno, BlockIdExt> masterchain_blocks_;
  BlockSeqno last_masterchain_seqno_ = 0;

  struct BlockInfo {
    std::shared_ptr<Package> data_pkg;
    td::uint64 data_offset = 0;
    std::shared_ptr<Package> proof_pkg;
    td::uint64 proof_offset = 0;
  };
  std::map<BlockIdExt, BlockInfo> blocks_;

  td::Ref<MasterchainState> start_state_;
  size_t pending_shard_archives_ = 0;

  bool imported_any_ = false;
  bool have_shard_blocks_ = false;
  std::vector<std::string> files_to_cleanup_;
};

}  // namespace validator

}  // namespace ton
