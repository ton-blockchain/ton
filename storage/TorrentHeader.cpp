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

#include "TorrentHeader.hpp"

#include "td/utils/tl_helpers.h"

namespace ton {
td::CSlice TorrentHeader::get_dir_name() const {
  return dir_name;
}

td::uint32 TorrentHeader::get_files_count() const {
  return files_count;
}

td::uint64 TorrentHeader::get_data_begin(td::uint64 file_i) const {
  return get_data_offset(file_i);
}
td::uint64 TorrentHeader::get_data_end(td::uint64 file_i) const {
  return get_data_offset(file_i + 1);
}

td::uint64 TorrentHeader::serialization_size() const {
  return td::tl_calc_length(*this);
}
td::uint64 TorrentHeader::get_data_offset(td::uint64 offset_i) const {
  td::uint64 res = serialization_size();
  if (offset_i > 0) {
    CHECK(offset_i <= files_count);
    res += data_index[offset_i - 1];
  }
  return res;
}
td::BufferSlice TorrentHeader::serialize() const {
  return td::BufferSlice(td::serialize(*this));
}
td::uint64 TorrentHeader::get_data_size(td::uint64 file_i) const {
  auto res = data_index[file_i];
  if (file_i > 0) {
    res -= data_index[file_i - 1];
  }
  return res;
}

td::Slice TorrentHeader::get_name(td::uint64 file_i) const {
  CHECK(file_i < files_count);
  auto from = file_i == 0 ? 0 : name_index[file_i - 1];
  auto till = name_index[file_i];
  return td::Slice(names).substr(from, till - from);
}

}  // namespace ton
