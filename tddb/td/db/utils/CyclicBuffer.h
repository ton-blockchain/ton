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

#include "StreamInterface.h"

#include <utility>

namespace td {

class CyclicBuffer {
 public:
  struct Options {
    Options() {
    }
    // Optimized: 256KB chunks (was 128KB) for better I/O performance
    size_t chunk_size{256 * 1024};
    // Optimized: 32 chunks (was 16) = 8MB total buffer (fits in modern L3 cache)
    size_t count{32};
    // Optimized: 4KB alignment (was 1KB) for page-aligned access and better TLB performance
    size_t alignment{4096};

    size_t size() const {
      return chunk_size * count;
    }
    size_t max_writable_size() {
      return size() - chunk_size;
    }
  };
  using Reader = StreamReader;
  using Writer = StreamWriter;
  static std::pair<Reader, Writer> create(Options options = {});
};

}  // namespace td
