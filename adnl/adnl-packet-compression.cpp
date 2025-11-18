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
#include "adnl-packet-compression.h"
#include "td/utils/lz4.h"
#include "td/utils/misc.h"
#include "td/utils/logging.h"
#include <cstring>

namespace ton {
namespace adnl {

td::BufferSlice maybe_compress_packet(td::BufferSlice data) {
  // Don't compress if below threshold
  if (data.size() < kCompressionThreshold) {
    return data;
  }

  // Compress the data
  auto compressed = td::lz4_compress(data.as_slice());

  // Only use compression if it actually reduces size (add header overhead)
  if (compressed.size() + kCompressionHeaderSize >= data.size()) {
    LOG(DEBUG) << "Compression not beneficial: " << data.size() << " -> "
               << (compressed.size() + kCompressionHeaderSize) << " bytes";
    return data;
  }

  // Create buffer with header + compressed data
  td::BufferSlice result(kCompressionHeaderSize + compressed.size());
  auto slice = result.as_slice();

  // Write magic bytes (little-endian)
  std::memcpy(slice.data(), &kCompressionMagic, 4);

  // Write uncompressed size (little-endian)
  uint32_t uncompressed_size = static_cast<uint32_t>(data.size());
  std::memcpy(slice.data() + 4, &uncompressed_size, 4);

  // Write compressed data
  std::memcpy(slice.data() + kCompressionHeaderSize, compressed.data(), compressed.size());

  LOG(DEBUG) << "Compressed packet: " << data.size() << " -> " << result.size()
             << " bytes (" << (100 * result.size() / data.size()) << "%)";

  return result;
}

td::Result<td::BufferSlice> maybe_decompress_packet(td::BufferSlice data) {
  // Check if data has compression header
  if (data.size() < kCompressionHeaderSize) {
    return std::move(data);  // Too small to be compressed
  }

  // Check magic bytes
  uint32_t magic;
  std::memcpy(&magic, data.data(), 4);

  if (magic != kCompressionMagic) {
    return std::move(data);  // Not compressed
  }

  // Read uncompressed size
  uint32_t uncompressed_size;
  std::memcpy(&uncompressed_size, data.data() + 4, 4);

  // Sanity check: uncompressed size should be reasonable (< 16MB for ADNL packets)
  constexpr uint32_t kMaxUncompressedSize = 16 * 1024 * 1024;
  if (uncompressed_size == 0 || uncompressed_size > kMaxUncompressedSize) {
    return td::Status::Error("Invalid uncompressed size in packet header");
  }

  // Extract compressed data (skip header)
  auto compressed_slice = data.as_slice();
  compressed_slice.remove_prefix(kCompressionHeaderSize);

  // Decompress
  TRY_RESULT(decompressed, td::lz4_decompress(compressed_slice, uncompressed_size));

  LOG(DEBUG) << "Decompressed packet: " << data.size() << " -> " << decompressed.size() << " bytes";

  return std::move(decompressed);
}

}  // namespace adnl
}  // namespace ton
