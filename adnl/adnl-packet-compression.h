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
*/
#pragma once

#include "td/utils/buffer.h"
#include "td/utils/Status.h"

namespace ton {
namespace adnl {

// Compression threshold: compress packets larger than 4KB
constexpr size_t kCompressionThreshold = 4096;

// Magic bytes to identify compressed packets: "ADLZ" (ADNL LZ4)
constexpr uint32_t kCompressionMagic = 0x415D4C5A;  // "ADLZ" in ASCII

// Header size: 4 bytes magic + 4 bytes uncompressed size
constexpr size_t kCompressionHeaderSize = 8;

/**
 * Compresses packet data if it exceeds the compression threshold.
 * Format: [4 bytes magic][4 bytes uncompressed_size][compressed data]
 *
 * @param data The packet data to potentially compress
 * @return Compressed data if size > threshold, otherwise original data
 */
td::BufferSlice maybe_compress_packet(td::BufferSlice data);

/**
 * Decompresses packet data if it has the compression magic header.
 *
 * @param data The packet data to potentially decompress
 * @return Decompressed data if compressed, otherwise original data
 */
td::Result<td::BufferSlice> maybe_decompress_packet(td::BufferSlice data);

}  // namespace adnl
}  // namespace ton
