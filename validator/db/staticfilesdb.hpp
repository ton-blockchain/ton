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

#include "ton/ton-types.h"
#include "td/actor/actor.h"

namespace ton {

namespace validator {

class RootDb;

class StaticFilesDb : public td::actor::Actor {
 public:
  void load_file(FileHash file_hash, td::Promise<td::BufferSlice> promise);
  StaticFilesDb(td::actor::ActorId<RootDb> root_db, std::string path) : root_db_(root_db), path_(path) {
  }

 private:
  td::actor::ActorId<RootDb> root_db_;
  std::string path_;
};

}  // namespace validator

}  // namespace ton
