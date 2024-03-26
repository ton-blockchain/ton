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
#include "td/utils/buffer.h"
#include "td/utils/misc.h"
#include <lz4.h>

namespace td {

td::BufferSlice lz4_compress(td::Slice data) {
  int size = narrow_cast<int>(data.size());
  int buf_size = LZ4_compressBound(size);
  td::BufferSlice compressed(buf_size);
  int compressed_size = LZ4_compress_default(data.data(), compressed.data(), size, buf_size);
  CHECK(compressed_size > 0);
  return td::BufferSlice{compressed.as_slice().substr(0, compressed_size)};
}

td::Result<td::BufferSlice> lz4_decompress(td::Slice data, int max_decompressed_size) {
  TRY_RESULT(size, narrow_cast_safe<int>(data.size()));
  if (max_decompressed_size < 0) {
    return td::Status::Error("invalid max_decompressed_size");
  }
  td::BufferSlice decompressed(max_decompressed_size);
  int result = LZ4_decompress_safe(data.data(), decompressed.data(), size, max_decompressed_size);
  if (result < 0) {
    return td::Status::Error(PSTRING() << "lz4 decompression failed, error code: " << result);
  }
  if (result == max_decompressed_size) {
    return decompressed;
  }
  return td::BufferSlice{decompressed.as_slice().substr(0, result)};
}

}  // namespace td
