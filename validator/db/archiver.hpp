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

    Copyright 2017-2019 Telegram Systems LLP
*/
#pragma once

#include "ton/ton-types.h"
#include "td/actor/actor.h"
#include "validator/interfaces/block-handle.h"
#include "ton/ton-io.hpp"
#include "archive-db.hpp"

namespace ton {

namespace validator {

class RootDb;
class FileDb;

class BlockArchiver : public td::actor::Actor {
 public:
  BlockArchiver(BlockIdExt block_id, td::actor::ActorId<RootDb> root_db, td::actor::ActorId<FileDb> file_db,
                td::actor::ActorId<FileDb> archive_db, td::actor::ActorId<ArchiveManager> archive,
                td::Promise<td::Unit> promise);

  void abort_query(td::Status error);

  void start_up() override;
  void got_block_handle(BlockHandle handle);
  void got_proof(td::BufferSlice data);
  void written_proof();
  void got_proof_link(td::BufferSlice data);
  void written_proof_link();
  void got_block_data(td::BufferSlice data);
  void written_block_data();
  void finish_query();

 private:
  BlockIdExt block_id_;
  td::actor::ActorId<RootDb> root_db_;
  td::actor::ActorId<FileDb> file_db_;
  td::actor::ActorId<FileDb> archive_db_;
  td::actor::ActorId<ArchiveManager> archive_;
  td::Promise<td::Unit> promise_;

  BlockHandle handle_;
};

}  // namespace validator

}  // namespace ton
