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
#include "td/utils/port/FileFd.h"
#include "td/utils/buffer.h"

namespace ton {

class Package {
 public:
  static td::Result<Package> open(std::string path, bool read_only = false, bool create = false);

  Package(td::FileFd fd);
  Package(Package &&p) = default;
  ~Package();

  td::Status truncate(td::uint64 size);

  td::uint64 append(std::string filename, td::Slice data, bool sync = true);
  void sync();
  td::uint64 size() const;
  td::Result<std::pair<std::string, td::BufferSlice>> read(td::uint64 offset) const;

  td::Result<td::uint64> advance(td::uint64 offset);
  void iterate(std::function<bool(std::string, td::BufferSlice, td::uint64)> func);

  td::FileFd &fd() {
    return fd_;
  }

 private:
  td::FileFd fd_;
};

}  // namespace ton
