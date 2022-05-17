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
#include "td/utils/port/FileFd.h"
#include "td/utils/crypto.h"
#include <vector>

namespace vm {
namespace boc_writers {
struct BufferWriter {
  BufferWriter(unsigned char* store_start, unsigned char* store_end)
      : store_start(store_start), store_ptr(store_start), store_end(store_end) {}

  size_t position() const {
    return store_ptr - store_start;
  }
  size_t remaining() const {
    return store_end - store_ptr;
  }
  void chk() const {
    DCHECK(store_ptr <= store_end);
  }
  bool empty() const {
    return store_ptr == store_end;
  }
  void store_uint(unsigned long long value, unsigned bytes) {
    unsigned char* ptr = store_ptr += bytes;
    chk();
    while (bytes) {
      *--ptr = value & 0xff;
      value >>= 8;
      --bytes;
    }
    DCHECK(!bytes);
  }
  void store_bytes(unsigned char const* data, size_t s) {
    store_ptr += s;
    chk();
    memcpy(store_ptr - s, data, s);
  }
  unsigned get_crc32() const {
    return td::crc32c(td::Slice{store_start, store_ptr});
  }

 private:
  unsigned char* store_start;
  unsigned char* store_ptr;
  unsigned char* store_end;
};

struct FileWriter {
  FileWriter(td::FileFd& fd, size_t expected_size)
      : fd(fd), expected_size(expected_size) {}

  ~FileWriter() {
    flush();
  }

  size_t position() const {
    return flushed_size + writer.position();
  }
  size_t remaining() const {
    return expected_size - position();
  }
  void chk() const {
    DCHECK(position() <= expected_size);
  }
  bool empty() const {
    return remaining() == 0;
  }
  void store_uint(unsigned long long value, unsigned bytes) {
    flush_if_needed(bytes);
    writer.store_uint(value, bytes);
  }
  void store_bytes(unsigned char const* data, size_t s) {
    flush_if_needed(s);
    writer.store_bytes(data, s);
  }
  unsigned get_crc32() const {
    unsigned char const* start = buf.data();
    unsigned char const* end = start + writer.position();
    return td::crc32c_extend(current_crc32, td::Slice(start, end));
  }

  td::Status finalize() {
    flush();
    return std::move(res);
  }

 private:
  void flush_if_needed(size_t s) {
    DCHECK(s <= BUF_SIZE);
    if (s > BUF_SIZE - writer.position()) {
      flush();
    }
  }

  void flush() {
    chk();
    unsigned char* start = buf.data();
    unsigned char* end = start + writer.position();
    if (start == end) {
      return;
    }
    flushed_size += end - start;
    current_crc32 = td::crc32c_extend(current_crc32, td::Slice(start, end));
    if (res.is_ok()) {
      while (end > start) {
        auto R = fd.write(td::Slice(start, end));
        if (R.is_error()) {
          res = R.move_as_error();
          break;
        }
        size_t s = R.move_as_ok();
        start += s;
      }
    }
    writer = BufferWriter(buf.data(), buf.data() + buf.size());
  }

  td::FileFd& fd;
  size_t expected_size;
  size_t flushed_size = 0;
  unsigned current_crc32 = td::crc32c(td::Slice());

  static const size_t BUF_SIZE = 1 << 22;
  std::vector<unsigned char> buf = std::vector<unsigned char>(BUF_SIZE, '\0');
  BufferWriter writer = BufferWriter(buf.data(), buf.data() + buf.size());
  td::Status res = td::Status::OK();
};
}
}