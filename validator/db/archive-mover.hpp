#pragma once

#include "td/actor/actor.h"
#include "filedb.hpp"
#include "blockdb.hpp"
#include "statedb.hpp"
#include "celldb.hpp"
#include "archive-db.hpp"
#include "archive-manager.hpp"

#include <list>
#include <set>

namespace ton {

namespace validator {

class ArchiveFileMover : public td::actor::Actor {
 public:
  ArchiveFileMover(BlockIdExt block_id, td::actor::ActorId<BlockDb> block_db, td::actor::ActorId<FileDb> file_db,
                   td::actor::ActorId<FileDb> old_archive_db, td::actor::ActorId<OldArchiveManager> old_archive_manager,
                   td::actor::ActorId<ArchiveManager> archive_manager, td::Promise<td::Unit> promise)
      : block_id_(block_id)
      , block_db_(block_db)
      , file_db_(file_db)
      , old_archive_db_(old_archive_db)
      , old_archive_manager_(old_archive_manager)
      , archive_manager_(archive_manager)
      , promise_(std::move(promise)) {
  }
  void start_up() override;
  void got_block_handle0(td::Result<BlockHandle> R);
  void got_block_handle1(td::Result<BlockHandle> R);
  void got_block_handle2(td::Result<BlockHandle> R);
  void got_block_handle();

  void processed_child();
  void processed_all_children();

  void got_block_data(td::Result<td::BufferSlice> R);
  void got_block_proof(td::Result<td::BufferSlice> R);
  void got_block_proof_link(td::Result<td::BufferSlice> R);

  void written_data();
  void written_handle();

  void abort_query(td::Status error);
  void finish_query();

 private:
  BlockIdExt block_id_;
  BlockHandle handle_;
  td::BufferSlice data_;
  td::BufferSlice proof_;
  td::BufferSlice proof_link_;
  bool left_ = true;

  td::actor::ActorId<BlockDb> block_db_;
  td::actor::ActorId<FileDb> file_db_;
  td::actor::ActorId<FileDb> old_archive_db_;
  td::actor::ActorId<OldArchiveManager> old_archive_manager_;
  td::actor::ActorId<ArchiveManager> archive_manager_;

  td::Promise<td::Unit> promise_;
};

class ArchiveKeyBlockMover : public td::actor::Actor {
 public:
  ArchiveKeyBlockMover(BlockIdExt block_id, td::actor::ActorId<BlockDb> block_db, td::actor::ActorId<FileDb> file_db,
                       td::actor::ActorId<FileDb> old_archive_db,
                       td::actor::ActorId<OldArchiveManager> old_archive_manager,
                       td::actor::ActorId<ArchiveManager> archive_manager, td::Promise<td::Unit> promise)
      : block_id_(block_id)
      , block_db_(block_db)
      , file_db_(file_db)
      , old_archive_db_(old_archive_db)
      , old_archive_manager_(old_archive_manager)
      , archive_manager_(archive_manager)
      , promise_(std::move(promise)) {
  }

  void start_up() override;
  void failed_to_get_proof0();
  void failed_to_get_proof1();
  void failed_to_get_proof2();
  void failed_to_get_proof3();
  void got_block_proof(td::BufferSlice data);
  void skip_block_proof(td::BufferSlice data);

  void written_data();

  void abort_query(td::Status error);
  void finish_query();

 private:
  BlockIdExt block_id_;
  td::BufferSlice data_;
  bool proof_link_ = false;

  td::actor::ActorId<BlockDb> block_db_;
  td::actor::ActorId<FileDb> file_db_;
  td::actor::ActorId<FileDb> old_archive_db_;
  td::actor::ActorId<OldArchiveManager> old_archive_manager_;
  td::actor::ActorId<ArchiveManager> archive_manager_;

  td::Promise<td::Unit> promise_;
};

class ArchiveMover : public td::actor::Actor {
 public:
  ArchiveMover(std::string db_root, BlockIdExt masterchain_block_id, BlockIdExt shard_block_id,
               BlockIdExt key_block_id);

  void start_up() override;
  void moved_blocks();
  void got_handle(BlockHandle handle);
  void got_state(td::Ref<MasterchainState> state);
  void moved_key_blocks();
  void run();
  void completed();
  void add_to_move(BlockIdExt block_id);
  void add_to_check(BlockIdExt block_id);

  void got_to_check_handle(td::Result<BlockHandle> R);

  void abort_query(td::Status error);
  void finish_query();

 private:
  std::string db_root_;
  BlockHandle handle_;
  td::Ref<MasterchainState> state_;

  td::actor::ActorOwn<BlockDb> block_db_;
  td::actor::ActorOwn<FileDb> file_db_;
  td::actor::ActorOwn<FileDb> old_archive_db_;
  td::actor::ActorOwn<OldArchiveManager> old_archive_manager_;
  td::actor::ActorOwn<ArchiveManager> archive_manager_;
  td::actor::ActorOwn<CellDb> cell_db_;

  BlockIdExt masterchain_block_id_;
  BlockIdExt shard_block_id_;
  BlockIdExt key_block_id_;
};

}  // namespace validator

}  // namespace ton
