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

#include "vm/db/DynamicBagOfCellsDb.h"
#include "td/utils/Status.h"
#include "td/utils/buffer.h"

namespace vm {

enum class CompressionAlgorithm : int { BaselineLZ4 = 0, ImprovedStructureLZ4 = 1 };

td::Result<td::BufferSlice> boc_decompress_baseline_lz4(td::Slice data_compressed);
td::Result<td::BufferSlice> boc_decompress_improved_structure_lz4(td::Slice data_compressed);
td::Result<td::BufferSlice> boc_decompress(td::Slice data_compressed);

td::Result<td::BufferSlice> boc_compress_baseline_lz4(td::Slice data_serialized_31);
td::Result<td::BufferSlice> boc_compress_improved_structure_lz4(td::Slice data_serialized_31);
td::Result<td::BufferSlice> boc_compress(td::Slice data_serialized_31, CompressionAlgorithm algo = CompressionAlgorithm::BaselineLZ4);
}  // namespace vm
