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

static td::Status validate_name(td::Slice name, bool is_dir_name = false) {
  if (name.empty()) {
    return td::Status::Error("Name can't be empty");
  }
  if (name[0] == '/') {
    return td::Status::Error("Name can't start with '/'");
  }
  if (name.back() == '/' && !is_dir_name) {
    return td::Status::Error("Name can't end with '/'");
  }
  for (size_t l = 0; l < name.size();) {
    size_t r = l + 1;
    while (r < name.size() && name[r] != '/') {
      ++r;
    }
    td::Slice s = name.substr(l, r - l);
    if (s == "") {
      return td::Status::Error("Name can't contain consequitive '/'");
    }
    if (s == ".") {
      return td::Status::Error("Name can't contain component \".\"");
    }
    if (s == "..") {
      return td::Status::Error("Name can't contain component \"..\"");
    }
    l = r + 1;
  }
  return td::Status::OK();
}

td::Status TorrentHeader::validate(td::uint64 total_size, td::uint64 header_size) const {
  if (serialization_size() != header_size) {
    return td::Status::Error("Invalid size");
  }
  if (files_count == 0) {
    return td::Status::Error("No files");
  }
  for (size_t i = 0; i + 1 < files_count; ++i) {
    if (name_index[i] > name_index[i + 1]) {
      return td::Status::Error("Invalid name offset");
    }
  }
  if (name_index.back() != names.size()) {
    return td::Status::Error("Invalid name offset");
  }
  for (size_t i = 0; i < files_count; ++i) {
    if (get_data_offset(i) > get_data_offset(i + 1)) {
      return td::Status::Error("Invalid data offset");
    }
  }
  if (get_data_offset(files_count) != total_size) {
    return td::Status::Error("Invalid data offset");
  }

  std::set<std::string> names;
  for (size_t i = 0; i < files_count; ++i) {
    auto name = get_name(i);
    TRY_STATUS_PREFIX(validate_name(name), PSTRING() << "Invalid filename " << name << ": ");
    if (!names.insert(name.str()).second) {
      return td::Status::Error(PSTRING() << "Duplicate filename " << name);
    }
  }
  if (!dir_name.empty()) {
    TRY_STATUS_PREFIX(validate_name(dir_name, true), "Invalid dir_name: ");
  }
  for (const std::string& name : names) {
    std::string name1 = name + '/';
    auto it = names.lower_bound(name1);
    if (it != names.end() && it->substr(0, name1.size()) == name1) {
      return td::Status::Error(PSTRING() << "Filename " << name << " coincides with directory name");
    }
  }
  return td::Status::OK();
}

}  // namespace ton
