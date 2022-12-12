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
#include "Torrent.h"

#include "td/utils/optional.h"
#include "td/utils/SharedSlice.h"
#include "td/db/utils/BlobView.h"

namespace ton {
class Torrent::Creator {
 public:
  struct Options {
    td::uint32 piece_size{128 * 768};

    // override default dir_name
    // should't be used in a usual workflow
    td::optional<std::string> dir_name;

    std::string description;
  };

  // If path is a file create a torrent with one file in it.
  // If path is a directory, create a torrent with whole directory.
  static td::Result<Torrent> create_from_path(Options options, td::CSlice path);
  struct Blob {
    td::Slice name;
    td::Slice data;
  };
  static td::Result<Torrent> create_from_blobs(Options options, td::Span<Blob> blobs);

  // This api is mostly for internal usage. One should prefer static methods
  explicit Creator(Options options) : options_(options) {
  }
  TD_WARN_UNUSED_RESULT td::Status add_blob(td::Slice name, td::Slice blob);
  TD_WARN_UNUSED_RESULT td::Status add_blob(td::Slice name, td::BlobView blob);
  TD_WARN_UNUSED_RESULT td::Status add_file(td::Slice name, td::CSlice path);
  td::Result<Torrent> finalize();

 private:
  Options options_;
  struct File {
    std::string name;
    td::BlobView data;
  };
  std::vector<File> files_;
  std::string root_dir_;
};
}  // namespace ton
