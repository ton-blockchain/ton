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
#include "staticfilesdb.hpp"
#include "files-async.hpp"

namespace ton {

namespace validator {

void StaticFilesDb::load_file(FileHash file_hash, td::Promise<td::BufferSlice> promise) {
  auto path = path_ + "/" + file_hash.to_hex();
  td::actor::create_actor<db::ReadFile>("read file", path, 0, -1, db::ReadFile::Flags::f_disable_log,
                                        std::move(promise))
      .release();
}

}  // namespace validator

}  // namespace ton
