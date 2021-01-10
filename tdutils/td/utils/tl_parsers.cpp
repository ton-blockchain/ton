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
#include "td/utils/tl_parsers.h"

#include "td/utils/misc.h"

namespace td {

alignas(4) const unsigned char TlParser::empty_data[sizeof(UInt256)] = {};  // static zero-initialized

TlParser::TlParser(Slice slice) {
  data_len = left_len = slice.size();
  if (is_aligned_pointer<4>(slice.begin())) {
    data = slice.ubegin();
  } else {
    int32 *buf;
    if (data_len <= small_data_array.size() * sizeof(int32)) {
      buf = &small_data_array[0];
    } else {
      LOG(ERROR) << "Unexpected big unaligned data pointer of length " << slice.size() << " at " << slice.begin();
      data_buf = std::make_unique<int32[]>(1 + data_len / sizeof(int32));
      buf = data_buf.get();
    }
    std::memcpy(buf, slice.begin(), slice.size());
    data = reinterpret_cast<unsigned char *>(buf);
  }
}

void TlParser::set_error(const string &error_message) {
  if (error.empty()) {
    CHECK(!error_message.empty());
    error = error_message;
    error_pos = data_len - left_len;
    data = empty_data;
    left_len = 0;
    data_len = 0;
  } else {
    LOG_CHECK(error_pos != std::numeric_limits<size_t>::max() && data_len == 0 && left_len == 0)
        << data_len << " " << left_len << " " << data << " " << &empty_data[0] << " " << error_pos << " " << error
        << " " << data << " " << &empty_data;
    data = empty_data;
  }
}

BufferSlice TlBufferParser::as_buffer_slice(Slice slice) {
  if (slice.empty()) {
    return BufferSlice();
  }
  if (is_aligned_pointer<4>(slice.data())) {
    return parent_->from_slice(slice);
  }
  return BufferSlice(slice);
}

}  // namespace td
