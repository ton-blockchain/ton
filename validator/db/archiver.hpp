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
#include "ton/ton-io.hpp"
#include "ton/ton-types.h"
#include "validator/interfaces/block-handle.h"

#include "archive-manager.hpp"

namespace ton {

namespace validator {

class RootDb;
class FileDb;

class BlockArchiver : public td::actor::Actor {
 public:
  BlockArchiver(BlockHandle handle, td::actor::ActorId<ArchiveManager> archive_db, td::actor::ActorId<Db> db,
                td::Promise<td::Unit> promise);

  void start_up() override;
  td::actor::Task<> run();
  td::actor::Task<> run_inner();

 private:
  BlockHandle handle_;
  td::actor::ActorId<ArchiveManager> archive_;
  td::actor::ActorId<Db> db_;
  td::Promise<td::Unit> promise_;
  td::Timer timer_;
};

}  // namespace validator

}  // namespace ton
