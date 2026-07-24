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
#include "crypto/common/refcnt.hpp"
#include "td/utils/buffer.h"
#include "td/utils/int_types.h"
#include "ton/ton-types.h"

namespace block {

using FileHash = ton::Bits256;
using RootHash = ton::Bits256;
using td::Ref;

FileHash compute_file_hash(td::Slice data);
td::Result<td::BufferSlice> load_binary_file(std::string filename, td::int64 max_size = 0);
td::Status save_binary_file(std::string filename, const td::BufferSlice& data, unsigned long long max_size = 0);

std::string compute_db_filename(std::string base_dir, const FileHash& file_hash, int depth = 4);
std::string compute_db_tmp_filename(std::string base_dir, const FileHash& file_hash, int i = 0, bool makedirs = true,
                                    int depth = 4);

}  // namespace block
