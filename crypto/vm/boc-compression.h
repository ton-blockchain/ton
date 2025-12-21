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

#include "td/utils/Status.h"
#include "td/utils/buffer.h"
#include "vm/cells/CellBuilder.h"
#include "vm/cells/CellSlice.h"
#include "vm/excno.hpp"

namespace vm {

constexpr size_t kDecompressedSizeBytes = 4;

enum class CompressionAlgorithm : int { BaselineLZ4 = 0, ImprovedStructureLZ4 = 1 };

td::Result<td::BufferSlice> boc_compress_baseline_lz4(const std::vector<td::Ref<vm::Cell>>& boc_roots);
td::Result<std::vector<td::Ref<vm::Cell>>> boc_decompress_baseline_lz4(td::Slice compressed, int max_decompressed_size);

td::Result<td::BufferSlice> boc_compress_improved_structure_lz4(const std::vector<td::Ref<vm::Cell>>& boc_roots);
td::Result<std::vector<td::Ref<vm::Cell>>> boc_decompress_improved_structure_lz4(td::Slice compressed,
                                                                                 int max_decompressed_size);

td::Result<td::BufferSlice> boc_compress(const std::vector<td::Ref<vm::Cell>>& boc_roots,
                                         CompressionAlgorithm algo = CompressionAlgorithm::BaselineLZ4);
td::Result<std::vector<td::Ref<vm::Cell>>> boc_decompress(td::Slice compressed, int max_decompressed_size);

}  // namespace vm
