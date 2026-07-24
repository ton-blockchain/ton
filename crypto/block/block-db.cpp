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
#include <limits>

#include "td/utils/common.h"
#include "td/utils/crypto.h"
#include "td/utils/filesystem.h"
#include "td/utils/format.h"
#include "td/utils/misc.h"
#include "td/utils/port/FileFd.h"
#include "td/utils/port/path.h"
#include "ton/ton-io.hpp"
#include "vm/boc.h"
#include "vm/cellslice.h"
#include "vm/db/StaticBagOfCellsDb.h"

#include "block-db.h"

namespace block {

td::Result<td::BufferSlice> load_binary_file(std::string filename, td::int64 max_size) {
  //TODO: use td::read_file
  auto res = [&]() -> td::Result<td::BufferSlice> {
    TRY_RESULT(fd, td::FileFd::open(filename, td::FileFd::Read));
    TRY_RESULT(stat, fd.stat());
    if (!stat.is_reg_) {
      return td::Status::Error("file is not regular");
    }
    td::int64 size = stat.size_;
    if (!size) {
      return td::Status::Error("file is empty");
    }
    if ((max_size && size > max_size) || static_cast<td::uint64>(size) > std::numeric_limits<std::size_t>::max()) {
      return td::Status::Error("file is too long");
    }
    td::BufferSlice res(td::narrow_cast<std::size_t>(size));
    TRY_RESULT(r, fd.read(res.as_slice()));
    if (r != static_cast<td::uint64>(size)) {
      return td::Status::Error(PSLICE() << "read " << r << " bytes out of " << size);
    }
    return std::move(res);
  }();
  LOG_IF(ERROR, res.is_error()) << "error reading file `" << filename << "` : " << res.error();
  return res;
}

td::Status save_binary_file(std::string filename, const td::BufferSlice& data, unsigned long long max_size) {
  //TODO: use td::write_file
  auto status = [&]() {
    if (max_size && data.size() > max_size) {
      return td::Status::Error("contents too long");
    }
    auto size = data.size();
    TRY_RESULT(to_file, td::FileFd::open(filename, td::FileFd::CreateNew | td::FileFd::Write));
    TRY_RESULT(written, to_file.write(data));
    if (written != static_cast<size_t>(size)) {
      return td::Status::Error(PSLICE() << "written " << written << " bytes instead of " << size);
    }
    to_file.close();
    return td::Status::OK();
  }();
  LOG_IF(ERROR, status.is_error()) << "error writing new file `" << filename << "` : " << status;
  return status;
}

FileHash compute_file_hash(td::Slice data) {
  ton::Bits256 data_hash;
  td::sha256(data, td::MutableSlice{data_hash.data(), 32});
  return data_hash;
}

std::string compute_db_filename(std::string base_dir, const FileHash& file_hash, int depth) {
  static const char hex_digits[] = "0123456789ABCDEF";
  assert(depth >= 0 && depth <= 8);
  std::string res = std::move(base_dir);
  res.reserve(res.size() + 32 + depth * 3 + 4);
  for (int i = 0; i < depth; i++) {
    unsigned u = file_hash.data()[i];
    res.push_back(hex_digits[u >> 4]);
    res.push_back(hex_digits[u & 15]);
    res.push_back('/');
  }
  for (int i = 0; i < 32; i++) {
    unsigned u = file_hash.data()[i];
    res.push_back(hex_digits[u >> 4]);
    res.push_back(hex_digits[u & 15]);
  }
  res += ".boc";
  return res;
}

std::string compute_db_tmp_filename(std::string base_dir, const FileHash& file_hash, int i, bool makedirs, int depth) {
  static const char hex_digits[] = "0123456789ABCDEF";
  assert(depth >= 0 && depth <= 8);
  std::string res = std::move(base_dir);
  res.reserve(res.size() + 32 + depth * 3 + 4);
  for (int j = 0; j < depth; j++) {
    unsigned u = file_hash.data()[j];
    res.push_back(hex_digits[u >> 4]);
    res.push_back(hex_digits[u & 15]);
    res.push_back('/');
    if (makedirs) {
      td::mkdir(res, 0755).ignore();
    }
  }
  for (int j = 0; j < 32; j++) {
    unsigned u = file_hash.data()[j];
    res.push_back(hex_digits[u >> 4]);
    res.push_back(hex_digits[u & 15]);
  }
  res += ".tmp";
  if (i > 0) {
    if (i < 10) {
      res.push_back((char)('0' + i));
    } else {
      res.push_back((char)('0' + i / 10));
      res.push_back((char)('0' + i % 10));
    }
  }
  return res;
}

}  // namespace block
